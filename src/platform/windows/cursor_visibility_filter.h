/**
 * @file src/platform/windows/cursor_visibility_filter.h
 * @brief Pure decision logic that keeps a flapping OS cursor-visibility flag from blinking the streamed cursor.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace platf::dxgi {

  /**
   * @brief Rate filter for the OS-reported mouse cursor visibility flag.
   *
   * Every capture path blends the cursor only while Windows says it is visible
   * (DDA `PointerPosition.Visible`, the LuminalVGD hardware-cursor plane's
   * `IsCursorVisible`). That is the right contract: a game hides its cursor and
   * the stream must follow within a frame.
   *
   * Windows 11 Insider flights have shipped a fault in which "the mouse cursor
   * gets into a state where it blinks repeatedly" (Microsoft's known-issue text
   * for build 29648.1000; the same fault was fixed on the 26300 line in
   * 26300.8935). The OS toggles that flag on and off, the capture path mirrors
   * it faithfully, and the client sees a blinking cursor.
   *
   * The two are told apart by rate. A legitimate visibility change happens at
   * human cadence — a menu opening, a game grabbing the mouse — a few times a
   * minute at most. A blink is periodic and sustained. So:
   *  - Every report passes straight through until `flap_threshold` hide
   *    transitions land inside `flap_window`: no added latency in the normal case.
   *  - Once they do, the filter HOLDS the cursor visible and ignores hidden
   *    reports: a cursor the OS cannot stop toggling is one the user is using.
   *  - The hold ends once the reported state has been stable (no transition at
   *    all) for `settle_time`, and the reported state is honoured again. A game
   *    that hides the cursor while a hold is active gets its hidden cursor
   *    within `settle_time`; a resumed blink re-arms the hold after another
   *    `flap_threshold` cycles.
   *  - The hold is therefore bounded by `settle_time` AFTER THE FLAG STOPS
   *    CHANGING, not by `flap_window`: while the OS keeps toggling, the
   *    cursor stays visible, which is what the local display shows too.
   *  - A caller that only hears from the OS on pointer updates (DDA) must
   *    tick() between reports so a hold can end while the mouse is still.
   *  - take_stats() reports transition counts per `stats_interval`, so a
   *    flap that never reaches the threshold still shows up in the log.
   *
   * Removal criterion: drop this once no supported Windows build reports the
   * flap (fixed on the 26300 line in 26300.8935, documented open on 29648.1000).
   *
   * Pure and allocation-free: the ring capture path calls it at its 1 kHz
   * claim-poll cadence.
   */
  class cursor_visibility_filter_t {
  public:
    using clock = std::chrono::steady_clock;

    struct config_t {
      /// Hide transitions must all land inside this window to count as a flap
      /// (3 hides in 3 s = a blink period of 1.5 s or faster).
      std::chrono::milliseconds flap_window {3000};
      /// Number of visible->hidden transitions inside `flap_window` that starts a hold (2..8).
      int flap_threshold {3};
      /// A hold ends once the reported flag has not changed for this long.
      std::chrono::milliseconds settle_time {2000};
      /// How often take_stats() has something to say (only when the flag moved).
      std::chrono::milliseconds stats_interval {30000};
    };

    enum class event_e {
      hold_started,
      hold_ended,
    };

    /// One-shot notification for the caller's log line (see take_event()).
    struct event_t {
      event_e kind;
      /// hold_started: hides inside the window. hold_ended: hides suppressed during the hold.
      int hide_count;
      /// hold_started: span covered by those hides. hold_ended: hold duration.
      std::chrono::milliseconds span;
    };

    /// Periodic transition telemetry (see take_stats()).
    struct stats_t {
      /// Visibility transitions (either direction) inside the interval.
      int transitions;
      /// Shortest gap between consecutive transitions seen in the interval.
      std::chrono::milliseconds min_gap;
      /// Actual length of the interval reported.
      std::chrono::milliseconds interval;
    };

    cursor_visibility_filter_t() = default;

    explicit cursor_visibility_filter_t(config_t cfg):
        _cfg(cfg) {}

    /**
     * @brief Feed one OS report and get the visibility the frame should use.
     * @param reported_visible The flag as the OS reported it.
     * @param now Monotonic time of the report.
     * @return The effective visibility for this frame.
     */
    bool apply(bool reported_visible, clock::time_point now) {
      if (_last_reported && *_last_reported != reported_visible) {
        if (_transition_count > 0) {
          const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_transition_at);
          _stats_min_gap = _stats_min_gap ? std::min(*_stats_min_gap, gap) : gap;
        }
        ++_transition_count;
        ++_stats_transitions;
        _last_transition_at = now;
        if (!reported_visible) {
          push_hide(now);
          if (_holding) {
            ++_hides_during_hold;
          }
        }
      }
      _last_reported = reported_visible;

      if (!_holding) {
        const int n = threshold();
        if (_hide_count >= n) {
          const auto oldest = nth_most_recent_hide(n);
          if (now - oldest <= _cfg.flap_window) {
            _holding = true;
            ++_holds_started;
            _hold_started_at = now;
            _hides_during_hold = 1;  // the hide that tripped the threshold is suppressed too
            _event = event_t {event_e::hold_started, n, std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest)};
          }
        }
      } else if (now - _last_transition_at >= _cfg.settle_time) {
        _holding = false;
        _event = event_t {event_e::hold_ended, _hides_during_hold, std::chrono::duration_cast<std::chrono::milliseconds>(now - _hold_started_at)};
        // Forget the flap's hides so an unrelated later hide cannot combine
        // with stale ones and re-arm the hold on its own.
        _hide_count = 0;
        _hides_during_hold = 0;
      }

      roll_stats(now);
      return _holding ? true : reported_visible;
    }

    /**
     * @brief Re-evaluate without a new report (the OS only speaks on pointer updates).
     * @return The effective visibility, or nullopt before the first report.
     */
    std::optional<bool> tick(clock::time_point now) {
      if (!_last_reported) {
        return std::nullopt;
      }
      return apply(*_last_reported, now);  // same value: no transition is recorded
    }

    /// True while hidden reports are being overridden.
    bool holding() const {
      return _holding;
    }

    /// Number of holds started since construction (the first one is the interesting log line).
    int holds_started() const {
      return _holds_started;
    }

    /// Drain the pending hold_started / hold_ended notification, if any.
    std::optional<event_t> take_event() {
      auto e = _event;
      _event.reset();
      return e;
    }

    /// Drain the pending per-interval telemetry; set only when the flag moved in that interval.
    std::optional<stats_t> take_stats() {
      auto s = _stats;
      _stats.reset();
      return s;
    }

  private:
    static constexpr std::size_t kCapacity = 8;

    int threshold() const {
      return std::clamp(_cfg.flap_threshold, 2, static_cast<int>(kCapacity));
    }

    void push_hide(clock::time_point at) {
      _hides[_hide_head] = at;
      _hide_head = (_hide_head + 1) % kCapacity;
      _hide_count = std::min(_hide_count + 1, static_cast<int>(kCapacity));
    }

    /// Timestamp of the n-th most recent hide (n == 1 is the newest); caller guarantees _hide_count >= n.
    clock::time_point nth_most_recent_hide(int n) const {
      return _hides[(_hide_head + kCapacity - static_cast<std::size_t>(n)) % kCapacity];
    }

    /// Close the telemetry interval when it has elapsed; publish only if the flag moved.
    void roll_stats(clock::time_point now) {
      if (_stats_window_started == clock::time_point {}) {
        _stats_window_started = now;
        return;
      }
      const auto elapsed = now - _stats_window_started;
      if (elapsed < _cfg.stats_interval) {
        return;
      }
      if (_stats_transitions > 0) {
        _stats = stats_t {_stats_transitions, _stats_min_gap.value_or(std::chrono::milliseconds {0}), std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)};
      }
      _stats_transitions = 0;
      _stats_min_gap.reset();
      _stats_window_started = now;
    }

    config_t _cfg {};
    std::optional<bool> _last_reported;
    clock::time_point _last_transition_at {};
    std::array<clock::time_point, kCapacity> _hides {};
    std::size_t _hide_head = 0;
    int _hide_count = 0;
    bool _holding = false;
    clock::time_point _hold_started_at {};
    int _hides_during_hold = 0;
    int _holds_started = 0;
    std::optional<event_t> _event;
    /// Total transitions ever seen (the first one has no predecessor to measure a gap against).
    std::uint64_t _transition_count = 0;
    clock::time_point _stats_window_started {};
    int _stats_transitions = 0;
    /// Shortest gap between consecutive transitions in the current interval (unset until two have been seen).
    std::optional<std::chrono::milliseconds> _stats_min_gap;
    std::optional<stats_t> _stats;
  };

}  // namespace platf::dxgi
