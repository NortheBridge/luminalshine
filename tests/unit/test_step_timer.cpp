/**
 * @file tests/unit/test_step_timer.cpp
 * @brief Unit tests for the named step-duration recorder used by teardown logs.
 */

#include "src/step_timer.h"

#include <chrono>
#include <gtest/gtest.h>

namespace {

  using namespace std::chrono_literals;

  /**
   * @brief A clock the test moves by hand, so no assertion depends on real time.
   */
  struct fake_clock_t {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<fake_clock_t>;

    static inline time_point current {};

    static time_point now() noexcept {
      return current;
    }
  };

  using fake_timer_t = util::basic_step_timer_t<fake_clock_t>;

  TEST(StepTimer, NothingRecordedFormatsAsEmpty) {
    const util::step_timer_t timer;
    EXPECT_EQ(timer.format(), "");
  }

  TEST(StepTimer, StepsFormatInRecordingOrder) {
    util::step_timer_t timer;
    timer.record("sessions", 12ms);
    timer.record("app", 5210ms);
    timer.record("config", 0ms);
    EXPECT_EQ(timer.format(), "sessions=12ms app=5210ms config=0ms");
  }

  TEST(StepTimer, SkippedStepsAreMarked) {
    util::step_timer_t timer;
    timer.record("sessions", 3ms);
    timer.skip("app");
    timer.skip("display");
    EXPECT_EQ(timer.format(), "sessions=3ms app=skipped display=skipped");
  }

  TEST(StepTimer, RecordingTheSameStepAgainAddsToItInPlace) {
    util::step_timer_t timer;
    timer.record("undo", 100ms);
    timer.record("display", 7ms);
    timer.record("undo", 250ms);
    EXPECT_EQ(timer.format(), "undo=350ms display=7ms");
  }

  TEST(StepTimer, TimeWinsOverSkipForTheSameStep) {
    util::step_timer_t timer;
    timer.skip("undo");
    timer.record("undo", 40ms);
    timer.record("app", 9ms);
    timer.skip("app");
    EXPECT_EQ(timer.format(), "undo=40ms app=9ms");
  }

  TEST(StepTimer, RecordSinceMeasuresFromTheGivenStartOnTheTimersClock) {
    fake_clock_t::current = fake_clock_t::time_point {} + 1h;
    fake_timer_t timer;

    fake_clock_t::current += 250ms;
    const auto step_started = timer.now();
    fake_clock_t::current += 1999us;
    EXPECT_EQ(timer.record_since("cleanup_wait", step_started), 1ms);  // Whole milliseconds, truncated.

    fake_clock_t::current += 3s;
    EXPECT_EQ(timer.record_since("app", step_started), 3001ms);

    EXPECT_EQ(timer.format(), "cleanup_wait=1ms app=3001ms");
    EXPECT_EQ(timer.elapsed(), 3251ms);  // From construction, timed steps or not.
  }

  TEST(StepTimer, GivenStartCountsTimeBeforeConstructionInElapsed) {
    fake_clock_t::current = fake_clock_t::time_point {} + 2h;
    const auto queued_at = fake_clock_t::current;

    fake_clock_t::current += 700ms;  // Waiting before the work began.
    fake_timer_t timer {queued_at};
    EXPECT_EQ(timer.record_since("queue", queued_at), 700ms);

    fake_clock_t::current += 50ms;
    EXPECT_EQ(timer.elapsed(), 750ms);
    EXPECT_EQ(timer.format(), "queue=700ms");
  }

}  // namespace
