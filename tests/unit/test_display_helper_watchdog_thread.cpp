/**
 * @file tests/unit/test_display_helper_watchdog_thread.cpp
 * @brief Regression tests for stopping the display helper watchdog thread (issue #212).
 *
 * A session teardown or an app termination can run on the watchdog's own loop, so stopping the
 * watchdog must not join the calling thread: that throws std::system_error (EDEADLK) and,
 * uncaught, crashes the host.
 */
#include "src/platform/windows/display_helper_watchdog.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace {
  using namespace std::chrono_literals;

  constexpr auto kWaitTimeout = 5s;

  struct SelfStopOutcome {
    bool threw = false;
    bool stop_requested = false;
    std::string error;
  };
}  // namespace

TEST(DisplayHelperWatchdogThread, StopFromWatchdogThreadItselfDoesNotSelfJoin) {
  // The watchdog thread may outlive stop_watchdog_thread() (it detaches itself), so everything it
  // touches after that call is owned by shared_ptr/shared_future copies, not test-stack state.
  auto outcome = std::make_shared<std::promise<SelfStopOutcome>>();
  auto result = outcome->get_future();
  std::promise<void> assigned;
  const std::shared_future<void> assigned_future = assigned.get_future().share();

  // Heap-owned so a regression that blocks in a self-join can be leaked on timeout: destroying a
  // jthread that is stuck joining itself would hang the test binary instead of failing the test.
  auto watchdog = std::make_unique<std::jthread>();
  std::jthread *const self = watchdog.get();
  *watchdog = std::jthread([self, assigned_future, outcome](std::stop_token token) {
    assigned_future.wait();  // `watchdog` must be fully assigned before it is stopped from inside
    SelfStopOutcome out;
    try {
      display_helper_integration::stop_watchdog_thread(*self);
    } catch (const std::exception &e) {
      out.threw = true;
      out.error = e.what();
    }
    out.stop_requested = token.stop_requested();
    outcome->set_value(std::move(out));
  });
  assigned.set_value();

  if (result.wait_for(kWaitTimeout) != std::future_status::ready) {
    (void) watchdog.release();
    FAIL() << "stop_watchdog_thread did not return on the watchdog thread";
  }
  const auto out = result.get();
  EXPECT_FALSE(out.threw) << out.error;
  EXPECT_TRUE(out.stop_requested);
  EXPECT_FALSE(watchdog->joinable());
}

TEST(DisplayHelperWatchdogThread, StopFromAnotherThreadWaitsForExit) {
  auto exited = std::make_shared<std::atomic<bool>>(false);
  std::jthread watchdog([exited](std::stop_token token) {
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(50ms);  // a stop that returned without joining would be observed here
    exited->store(true);
  });

  display_helper_integration::stop_watchdog_thread(watchdog);

  EXPECT_TRUE(exited->load());
  EXPECT_FALSE(watchdog.joinable());
}

TEST(DisplayHelperWatchdogThread, NonJoinableThreadIsNoOp) {
  std::jthread idle;

  EXPECT_NO_THROW(display_helper_integration::stop_watchdog_thread(idle));
  EXPECT_FALSE(idle.joinable());
}
