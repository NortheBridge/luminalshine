/**
 * @file tests/unit/test_display_helper_wedge_escalation.cpp
 * @brief Unit tests for the display helper wedge escalation state machine.
 *
 * Covers the decision logic added for POSTMORTEM-2026-07-27 helper blocker #6:
 * a machine-wide WDDM wedge must escalate loudly once (then re-log at a bounded
 * rate) instead of silently rejecting restore snapshots every poll cycle, and
 * must trigger exactly one retry when enumeration recovers.
 */
#include "../tests_common.h"
#include "src/platform/windows/display_helper_wedge_escalation.h"

#include <chrono>

using display_helper_integration::WedgeEscalation;
using Action = WedgeEscalation::Action;
using namespace std::chrono_literals;

namespace {
  WedgeEscalation::time_point at(std::chrono::milliseconds offset) {
    return WedgeEscalation::time_point {} + offset;
  }
}  // namespace

TEST(DisplayHelperWedgeEscalation, HealthyResultsStayQuiet) {
  WedgeEscalation esc;

  EXPECT_EQ(esc.on_enumeration_result(true, at(0s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(true, at(10s)), Action::Proceed);
  EXPECT_FALSE(esc.engaged());
  EXPECT_FALSE(esc.wedged());
  EXPECT_EQ(esc.consecutive_failures(), 0u);
  EXPECT_EQ(esc.failing_for(at(20s)), 0ms);
}

TEST(DisplayHelperWedgeEscalation, ShortFailureStreakDoesNotEscalate) {
  WedgeEscalation esc;

  // Four failures — below the default threshold of five — even over a long span.
  EXPECT_EQ(esc.on_enumeration_result(false, at(0s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(120s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(180s)), Action::Proceed);

  EXPECT_TRUE(esc.engaged());
  EXPECT_FALSE(esc.wedged());
  EXPECT_EQ(esc.consecutive_failures(), 4u);
}

TEST(DisplayHelperWedgeEscalation, FastFailureBurstBelowMinDurationDoesNotEscalate) {
  WedgeEscalation esc;

  // Ten rapid-fire failures within 45s: count threshold met, wall-clock gate not.
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(esc.on_enumeration_result(false, at(i * 5s)), Action::Proceed) << "sample " << i;
  }

  EXPECT_TRUE(esc.engaged());
  EXPECT_FALSE(esc.wedged());
  EXPECT_EQ(esc.consecutive_failures(), 10u);
}

TEST(DisplayHelperWedgeEscalation, EscalatesOnceAfterThresholdAndDuration) {
  WedgeEscalation esc;

  EXPECT_EQ(esc.on_enumeration_result(false, at(0s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(15s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(30s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(45s)), Action::Proceed);

  // Fifth consecutive failure, 60s after the first: both gates satisfied.
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s)), Action::Escalate);
  EXPECT_TRUE(esc.wedged());
  EXPECT_EQ(esc.consecutive_failures(), 5u);
  EXPECT_EQ(esc.failing_for(at(60s)), 60000ms);

  // Escalation is emitted exactly once; further failures hold quietly.
  EXPECT_EQ(esc.on_enumeration_result(false, at(63s)), Action::Hold);
  EXPECT_EQ(esc.on_enumeration_result(false, at(66s)), Action::Hold);
}

TEST(DisplayHelperWedgeEscalation, RelogsAtMostEveryInterval) {
  WedgeEscalation esc;

  for (int i = 0; i < 5; ++i) {
    (void) esc.on_enumeration_result(false, at(i * 15s));  // escalates on the fifth sample (t=60s)
  }
  ASSERT_TRUE(esc.wedged());

  // Still inside the 10-minute re-log window: quiet.
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s + 5min)), Action::Hold);
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s + 10min - 1s)), Action::Hold);

  // Interval elapsed: one re-log, then quiet again until the next interval.
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s + 10min)), Action::Relog);
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s + 10min + 3s)), Action::Hold);
  EXPECT_EQ(esc.on_enumeration_result(false, at(60s + 20min)), Action::Relog);
}

TEST(DisplayHelperWedgeEscalation, RecoveryFromWedgeRetriesExactlyOnce) {
  WedgeEscalation esc;

  for (int i = 0; i < 5; ++i) {
    (void) esc.on_enumeration_result(false, at(i * 20s));
  }
  ASSERT_TRUE(esc.wedged());

  // Enumeration transitions from failing to succeeding: retry the held restore now.
  EXPECT_EQ(esc.on_enumeration_result(true, at(30min)), Action::RetryNow);
  EXPECT_FALSE(esc.wedged());
  EXPECT_FALSE(esc.engaged());
  EXPECT_EQ(esc.consecutive_failures(), 0u);

  // Subsequent healthy samples are ordinary.
  EXPECT_EQ(esc.on_enumeration_result(true, at(30min + 3s)), Action::Proceed);
}

TEST(DisplayHelperWedgeEscalation, SuccessResetsFailureStreakBeforeEscalation) {
  WedgeEscalation esc;

  (void) esc.on_enumeration_result(false, at(0s));
  (void) esc.on_enumeration_result(false, at(30s));
  (void) esc.on_enumeration_result(false, at(60s));

  // Recovery before the streak matured is NOT a wedge recovery — no forced retry.
  EXPECT_EQ(esc.on_enumeration_result(true, at(65s)), Action::Proceed);
  EXPECT_FALSE(esc.engaged());
  EXPECT_EQ(esc.consecutive_failures(), 0u);

  // A fresh streak must clear both gates from scratch again.
  EXPECT_EQ(esc.on_enumeration_result(false, at(70s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(200s)), Action::Proceed);
  EXPECT_EQ(esc.consecutive_failures(), 2u);
  EXPECT_EQ(esc.failing_for(at(200s)), 130000ms);
}

TEST(DisplayHelperWedgeEscalation, WedgedStatePersistsAcrossManualReset) {
  WedgeEscalation esc;

  for (int i = 0; i < 5; ++i) {
    (void) esc.on_enumeration_result(false, at(i * 20s));
  }
  ASSERT_TRUE(esc.wedged());

  esc.reset();
  EXPECT_FALSE(esc.wedged());
  EXPECT_FALSE(esc.engaged());
  EXPECT_EQ(esc.consecutive_failures(), 0u);
  EXPECT_EQ(esc.failing_for(at(100s)), 0ms);
}

TEST(DisplayHelperWedgeEscalation, CustomConfigIsHonored) {
  WedgeEscalation esc(WedgeEscalation::Config {
    .failure_threshold = 2,
    .min_failure_duration = 5s,
    .relog_interval = 30s,
  });

  EXPECT_EQ(esc.on_enumeration_result(false, at(0s)), Action::Proceed);
  EXPECT_EQ(esc.on_enumeration_result(false, at(5s)), Action::Escalate);
  EXPECT_EQ(esc.on_enumeration_result(false, at(10s)), Action::Hold);
  EXPECT_EQ(esc.on_enumeration_result(false, at(5s + 30s)), Action::Relog);
  EXPECT_EQ(esc.on_enumeration_result(true, at(60s)), Action::RetryNow);
}
