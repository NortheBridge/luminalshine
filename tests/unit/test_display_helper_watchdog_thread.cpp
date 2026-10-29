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
    bool stopped = false;
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

TEST(DisplayHelperWatchdogThread, SlotStartIsRejectedWhileRunning) {
  display_helper_integration::WatchdogThreadSlot slot;
  auto second_ran = std::make_shared<std::atomic<bool>>(false);

  EXPECT_TRUE(slot.start([](std::stop_token token) {
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(1ms);
    }
  }));
  EXPECT_FALSE(slot.start([second_ran](std::stop_token) {
    second_ran->store(true);
  }));

  EXPECT_TRUE(slot.stop());
  EXPECT_FALSE(second_ran->load());
}

TEST(DisplayHelperWatchdogThread, SlotStopOnEmptySlotIsNoOp) {
  display_helper_integration::WatchdogThreadSlot slot;

  EXPECT_FALSE(slot.stop());
  EXPECT_TRUE(slot.start([](std::stop_token) {
  }));
  EXPECT_TRUE(slot.stop());
  EXPECT_FALSE(slot.stop());
}

TEST(DisplayHelperWatchdogThread, SlotStopFromWatchdogThreadItselfDoesNotSelfJoin) {
  // The watchdog thread detaches itself, so it only touches shared_ptr-owned state after stop().
  auto slot = std::make_shared<display_helper_integration::WatchdogThreadSlot>();
  auto outcome = std::make_shared<std::promise<SelfStopOutcome>>();
  auto result = outcome->get_future();

  // start() holds the slot lock while it assigns the thread, so the stop() below cannot run early.
  ASSERT_TRUE(slot->start([slot, outcome](std::stop_token token) {
    SelfStopOutcome out;
    try {
      out.stopped = slot->stop();
    } catch (const std::exception &e) {
      out.threw = true;
      out.error = e.what();
    }
    out.stop_requested = token.stop_requested();
    outcome->set_value(std::move(out));
  }));

  ASSERT_EQ(result.wait_for(kWaitTimeout), std::future_status::ready) << "slot stop did not return on the watchdog thread";
  const auto out = result.get();
  EXPECT_FALSE(out.threw) << out.error;
  EXPECT_TRUE(out.stopped);
  EXPECT_TRUE(out.stop_requested);
  EXPECT_FALSE(slot->stop());
}

// stop() joins outside the slot lock: a start() racing with it must neither block on the join nor
// touch the thread being stopped (assigning over a joinable std::jthread is undefined behavior).
TEST(DisplayHelperWatchdogThread, SlotStartDuringStopJoinDoesNotBlock) {
  display_helper_integration::WatchdogThreadSlot slot;
  std::promise<void> in_loop;
  std::promise<void> stop_seen;
  std::promise<void> release;
  const auto release_future = release.get_future().share();
  auto in_loop_future = in_loop.get_future();
  auto stop_seen_future = stop_seen.get_future();

  ASSERT_TRUE(slot.start([&in_loop, &stop_seen, release_future](std::stop_token token) {
    in_loop.set_value();
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(1ms);
    }
    stop_seen.set_value();
    release_future.wait();  // keeps stop() inside join() until the test lets go
  }));
  ASSERT_EQ(in_loop_future.wait_for(kWaitTimeout), std::future_status::ready);

  auto stopper = std::async(std::launch::async, [&slot] {
    return slot.stop();
  });
  // request_stop() runs after the slot lock was released, so from here stop() is joining.
  const bool stop_reached = stop_seen_future.wait_for(kWaitTimeout) == std::future_status::ready;

  std::future<bool> restarter;
  if (stop_reached) {
    restarter = std::async(std::launch::async, [&slot] {
      return slot.start([](std::stop_token token) {
        while (!token.stop_requested()) {
          std::this_thread::sleep_for(1ms);
        }
      });
    });
  }
  const bool restart_returned = stop_reached && restarter.wait_for(kWaitTimeout) == std::future_status::ready;

  release.set_value();  // lets every pending future finish, so none of them can hang the test binary
  EXPECT_TRUE(stopper.get());
  ASSERT_TRUE(stop_reached);
  ASSERT_TRUE(restart_returned) << "start() blocked while stop() was joining";
  EXPECT_TRUE(restarter.get());
  EXPECT_TRUE(slot.stop());
}
