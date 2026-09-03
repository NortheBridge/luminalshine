/**
 * @file tests/unit/test_cursor_visibility_filter.cpp
 * @brief Unit tests for the cursor visibility flap filter.
 *
 * Windows 11 Insider flights (Microsoft's known-issue text for build
 * 29648.1000; fixed earlier on the 26300 line in 26300.8935) toggle the
 * OS cursor-visibility flag on and off, and every capture path mirrors that
 * flag into whether the cursor is blended, so the client sees the cursor
 * blink. These tests pin the filter's contract:
 *  - Normal visibility changes pass through with no added latency, including
 *    a game hiding the cursor and a couple of quick toggles.
 *  - A sustained flap (`flap_threshold` hides inside `flap_window`) starts a
 *    hold that keeps the cursor visible while the flag keeps toggling.
 *  - The hold ends only once the flag has been stable for `settle_time`, and
 *    the reported state, hidden included, is honoured again.
 *  - A flap's hides never combine with a later unrelated hide.
 */
#include "../tests_common.h"
#include "src/platform/windows/cursor_visibility_filter.h"

#include <chrono>

using namespace std::chrono_literals;
using platf::dxgi::cursor_visibility_filter_t;
using filter_clock = cursor_visibility_filter_t::clock;

namespace {

  filter_clock::time_point base() {
    // A fixed origin keeps the arithmetic readable; the filter only ever looks at differences.
    return filter_clock::time_point {} + 1h;
  }

  /// Feed alternating hidden/visible reports every `half_period`, starting with a hide at `t`.
  /// Returns the time after the last report.
  filter_clock::time_point blink(cursor_visibility_filter_t &f, filter_clock::time_point t, std::chrono::milliseconds half_period, int cycles) {
    for (int i = 0; i < cycles; ++i) {
      f.apply(false, t);
      t += half_period;
      f.apply(true, t);
      t += half_period;
    }
    return t;
  }

}  // namespace

TEST(CursorVisibilityFilter, SteadyStatesPassThrough) {
  cursor_visibility_filter_t f;
  auto t = base();
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(f.apply(true, t));
    t += 1ms;
  }
  EXPECT_FALSE(f.holding());
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(f.apply(false, t));
    t += 1ms;
  }
  EXPECT_FALSE(f.holding());
  EXPECT_FALSE(f.take_event().has_value());
}

TEST(CursorVisibilityFilter, GameHidingCursorIsHonouredImmediately) {
  cursor_visibility_filter_t f;
  auto t = base();
  EXPECT_TRUE(f.apply(true, t));
  t += 5s;
  // The very report that hides the cursor is honoured: no debounce latency.
  EXPECT_FALSE(f.apply(false, t));
  t += 30s;
  EXPECT_FALSE(f.apply(false, t));
  t += 1ms;
  EXPECT_TRUE(f.apply(true, t));
  EXPECT_FALSE(f.holding());
}

TEST(CursorVisibilityFilter, TwoQuickTogglesDoNotStartAHold) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 100ms, 2);  // two hides inside 400 ms: below the threshold of three
  EXPECT_FALSE(f.holding());
  EXPECT_TRUE(f.apply(true, t));
  EXPECT_FALSE(f.take_event().has_value());
  // A third hide well outside the window (hides at 0, 200 and 3900 ms span
  // more than 3 s) is an ordinary hide and passes through.
  t += 3500ms;
  EXPECT_FALSE(f.apply(false, t));
  EXPECT_FALSE(f.holding());
}

TEST(CursorVisibilityFilter, SustainedFlapStartsAHoldOnTheThirdHide) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 400ms, 2);  // hides at 0 and 800 ms, both honoured
  EXPECT_FALSE(f.holding());
  // Third hide at 1600 ms: 3 hides inside 1.6 s -> hold starts, and this hide is suppressed.
  EXPECT_TRUE(f.apply(false, t));
  EXPECT_TRUE(f.holding());
  auto ev = f.take_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, cursor_visibility_filter_t::event_e::hold_started);
  EXPECT_EQ(ev->hide_count, 3);
  EXPECT_EQ(ev->span, 1600ms);
  EXPECT_FALSE(f.take_event().has_value());  // one-shot
  EXPECT_EQ(f.holds_started(), 1);
}

TEST(CursorVisibilityFilter, HoldKeepsCursorVisibleWhileFlagKeepsToggling) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 400ms, 3);
  ASSERT_TRUE(f.holding());
  (void) f.take_event();
  // A minute of blinking: every report, hidden or visible, comes back visible.
  for (int i = 0; i < 75; ++i) {
    EXPECT_TRUE(f.apply(false, t));
    t += 400ms;
    EXPECT_TRUE(f.apply(true, t));
    t += 400ms;
  }
  EXPECT_TRUE(f.holding());
  EXPECT_FALSE(f.take_event().has_value());  // no churn while the hold simply continues
}

TEST(CursorVisibilityFilter, HoldEndsAfterSettleTimeAndHonoursHidden) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 400ms, 3);
  ASSERT_TRUE(f.holding());
  (void) f.take_event();
  // The OS settles on hidden (a game grabbed the mouse mid-flap). The flag
  // stays overridden until it has been stable for settle_time, then honoured.
  const auto hidden_at = t;
  EXPECT_TRUE(f.apply(false, hidden_at));
  EXPECT_TRUE(f.apply(false, hidden_at + 1999ms));
  EXPECT_TRUE(f.holding());
  EXPECT_FALSE(f.apply(false, hidden_at + 2000ms));
  EXPECT_FALSE(f.holding());
  auto ev = f.take_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, cursor_visibility_filter_t::event_e::hold_ended);
  EXPECT_GE(ev->hide_count, 1);
  EXPECT_GE(ev->span, 2000ms);
}

TEST(CursorVisibilityFilter, HoldEndsQuietlyWhenFlagSettlesVisible) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 400ms, 3);
  ASSERT_TRUE(f.holding());
  (void) f.take_event();
  // Last report was visible; nothing changes for settle_time -> hold ends, output unchanged.
  EXPECT_TRUE(f.apply(true, t + 2000ms));
  EXPECT_FALSE(f.holding());
  auto ev = f.take_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, cursor_visibility_filter_t::event_e::hold_ended);
  // And a legitimate hide afterwards passes straight through again.
  EXPECT_FALSE(f.apply(false, t + 5s));
}

TEST(CursorVisibilityFilter, HidesOutsideTheWindowNeverCombine) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  // A hide every 1.6 s: any three span 3.2 s, more than the 3 s window, so never a hold.
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(f.apply(false, t));
    t += 800ms;
    EXPECT_TRUE(f.apply(true, t));
    t += 800ms;
  }
  EXPECT_FALSE(f.holding());
}

TEST(CursorVisibilityFilter, FlapHistoryIsDroppedWhenAHoldEnds) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  t = blink(f, t, 400ms, 3);
  ASSERT_TRUE(f.holding());
  t += 2000ms;
  EXPECT_TRUE(f.apply(true, t));
  ASSERT_FALSE(f.holding());
  (void) f.take_event();
  // Two more hides right after the hold ended must not re-arm it by combining
  // with the flap's old hides: they are a fresh count of two.
  t += 10ms;
  EXPECT_FALSE(f.apply(false, t));
  t += 100ms;
  EXPECT_TRUE(f.apply(true, t));
  t += 100ms;
  EXPECT_FALSE(f.apply(false, t));
  EXPECT_FALSE(f.holding());
}

TEST(CursorVisibilityFilter, TickEndsAHoldWithoutANewReport) {
  cursor_visibility_filter_t f;
  auto t = base();
  EXPECT_FALSE(f.tick(t).has_value());  // nothing reported yet
  f.apply(true, t);
  t = blink(f, t, 400ms, 2);
  EXPECT_TRUE(f.apply(false, t));  // third hide -> hold; last report is hidden
  ASSERT_TRUE(f.holding());
  (void) f.take_event();
  // The pointer goes quiet (DDA reports nothing more). Ticks alone must end
  // the hold once settle_time has passed and hand back the last reported state.
  auto v = f.tick(t + 1999ms);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(*v);
  EXPECT_TRUE(f.holding());
  v = f.tick(t + 2000ms);
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(*v);
  EXPECT_FALSE(f.holding());
  auto ev = f.take_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, cursor_visibility_filter_t::event_e::hold_ended);
}

TEST(CursorVisibilityFilter, StatsReportTransitionsPerInterval) {
  cursor_visibility_filter_t f;
  auto t = base();
  f.apply(true, t);
  EXPECT_FALSE(f.take_stats().has_value());
  // A quiet interval produces nothing.
  EXPECT_TRUE(f.apply(true, t + 30s));
  EXPECT_FALSE(f.take_stats().has_value());
  // Two slow toggles (never a hold) inside the next interval.
  f.apply(false, t + 35s);
  f.apply(true, t + 40s);
  f.apply(false, t + 47s);
  f.apply(true, t + 50s);
  EXPECT_FALSE(f.holding());
  EXPECT_FALSE(f.take_stats().has_value());  // interval not over yet
  EXPECT_TRUE(f.apply(true, t + 60s));
  auto s = f.take_stats();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->transitions, 4);
  EXPECT_EQ(s->min_gap, 3000ms);
  EXPECT_EQ(s->interval, 30000ms);
  EXPECT_FALSE(f.take_stats().has_value());  // one-shot
}

TEST(CursorVisibilityFilter, ConfigIsHonoured) {
  cursor_visibility_filter_t::config_t cfg;
  cfg.flap_window = 500ms;
  cfg.flap_threshold = 2;
  cfg.settle_time = 300ms;
  cursor_visibility_filter_t f {cfg};
  auto t = base();
  f.apply(true, t);
  EXPECT_FALSE(f.apply(false, t + 100ms));
  EXPECT_TRUE(f.apply(true, t + 200ms));
  EXPECT_TRUE(f.apply(false, t + 300ms));  // second hide inside 500 ms -> hold
  EXPECT_TRUE(f.holding());
  EXPECT_FALSE(f.apply(false, t + 600ms));  // stable hidden for 300 ms -> hold ends
  EXPECT_FALSE(f.holding());
}
