/**
 * @file tests/unit/test_session_teardown_policy.cpp
 * @brief Unit tests for session teardown timeout policy.
 */

#include "src/session_teardown_policy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

  using stream::session::teardown::phase_e;
  using stream::session::teardown::timeout_action_e;

  TEST(SessionTeardownPolicy, CaptureOwnershipTimeoutsRequireFastExit) {
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::waiting_video), timeout_action_e::fast_exit);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::waiting_audio), timeout_action_e::fast_exit);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::waiting_control), timeout_action_e::fast_exit);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::resetting_input), timeout_action_e::fast_exit);
  }

  TEST(SessionTeardownPolicy, PostReleaseTimeoutsUseControlledRestart) {
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::session_state), timeout_action_e::controlled_restart);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::display_cleanup), timeout_action_e::controlled_restart);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::frame_limiter_cleanup), timeout_action_e::controlled_restart);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::platform_cleanup), timeout_action_e::controlled_restart);
    EXPECT_EQ(stream::session::teardown::timeout_action(phase_e::deferred_config), timeout_action_e::controlled_restart);
  }

  TEST(SessionTeardownPolicy, CompletedTeardownIsNotARecoveryPhase) {
    EXPECT_STREQ(stream::session::teardown::phase_name(phase_e::done), "done");
  }

  TEST(SessionTeardownPolicy, OnlyCurrentIncompleteWatchdogArmMayAct) {
    EXPECT_TRUE(stream::session::teardown::watchdog_arm_is_current(7, 7, phase_e::waiting_video));
    EXPECT_FALSE(stream::session::teardown::watchdog_arm_is_current(6, 7, phase_e::waiting_video));
    EXPECT_FALSE(stream::session::teardown::watchdog_arm_is_current(7, 7, phase_e::done));
  }

  TEST(SessionTeardownPolicy, PhaseNamesRemainDiagnostic) {
    EXPECT_STREQ(stream::session::teardown::phase_name(phase_e::frame_limiter_cleanup), "frame-limiter/NVCP cleanup");
    EXPECT_STREQ(stream::session::teardown::phase_name(phase_e::platform_cleanup), "platform cleanup");
  }

  TEST(SessionTeardownPolicy, IndependentWatchdogFiresWithoutATaskPool) {
    std::mutex mutex;
    std::condition_variable cv;
    bool fired = false;
    stream::session::teardown::independent_watchdog_t watchdog {
      std::chrono::milliseconds(10),
      [&]() {
        {
          std::lock_guard lock {mutex};
          fired = true;
        }
        cv.notify_all();
      }
    };

    std::unique_lock lock {mutex};
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return fired; }));
  }

  TEST(SessionTeardownPolicy, CompletedIndependentWatchdogCannotFire) {
    std::atomic_bool fired {false};
    stream::session::teardown::independent_watchdog_t watchdog {
      std::chrono::milliseconds(40),
      [&]() { fired.store(true, std::memory_order_release); }
    };
    watchdog.complete();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(fired.load(std::memory_order_acquire));
  }

}  // namespace
