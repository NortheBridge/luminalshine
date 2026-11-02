/**
 * @file tests/unit/test_display_helper_watchdog.cpp
 * @brief Unit tests for Sunshine display helper watchdog.
 */
#include "../tests_common.h"
#include "src/platform/windows/display_helper_watchdog.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>

namespace {
  struct WatchdogHarness {
    bool feature_enabled = true;
    int ensure_calls = 0;
    bool ensure_result = true;
    int ping_calls = 0;
    std::deque<bool> ping_results;
    int reset_calls = 0;
    int session_count = 1;
    int running_processes = 0;
    int session_count_calls = 0;
    int running_processes_calls = 0;

    display_helper_integration::DisplayHelperWatchdog watchdog;

    WatchdogHarness():
        watchdog(display_helper_integration::DisplayHelperWatchdog::Hooks {
          .feature_enabled = [&]() {
            return feature_enabled;
          },
          .ensure_helper_started = [&]() {
            ensure_calls += 1;
            return ensure_result;
          },
          .send_ping = [&]() {
            ping_calls += 1;
            if (ping_results.empty()) {
              return true;
            }
            const bool result = ping_results.front();
            ping_results.pop_front();
            return result;
          },
          .reset_connection = [&]() {
            reset_calls += 1;
          },
          .session_count = [&]() {
            session_count_calls += 1;
            return session_count;
          },
          .running_processes = [&]() {
            running_processes_calls += 1;
            return running_processes;
          },
        }) {}
  };
}  // namespace

TEST(DisplayHelperWatchdog, StartsHelperAndPings) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  const auto interval = harness.watchdog.tick();

  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_EQ(harness.ensure_calls, 1);
  EXPECT_EQ(harness.ping_calls, 1);
  EXPECT_TRUE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, ResetsOnFeatureDisable) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  harness.watchdog.tick();
  harness.feature_enabled = false;
  harness.watchdog.tick();

  EXPECT_EQ(harness.reset_calls, 1);
  EXPECT_FALSE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, ReconnectsAfterPingFailure) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  harness.watchdog.tick();

  harness.ping_results = {false, true};
  harness.watchdog.tick();

  EXPECT_EQ(harness.reset_calls, 1);
  EXPECT_EQ(harness.ensure_calls, 2);
  EXPECT_EQ(harness.ping_calls, 3);
  EXPECT_TRUE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, UsesSuspendedIntervalWhenNoSessions) {
  WatchdogHarness harness;
  harness.session_count = 0;
  harness.running_processes = 1;
  harness.ping_results = {true};

  const auto interval = harness.watchdog.tick();
  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::suspended_interval());
}

namespace {
  using namespace std::chrono_literals;

  constexpr auto kActive = 5000ms;
  constexpr auto kSuspended = 15000ms;  // not the class default, so ignoring the hook field fails

  // The intervals the host loop uses; they differ from the class defaults.
  display_helper_integration::DisplayHelperWatchdog::Hooks host_hooks() {
    display_helper_integration::DisplayHelperWatchdog::Hooks hooks;
    hooks.feature_enabled = [] {
      return true;
    };
    hooks.ensure_helper_started = [] {
      return true;
    };
    hooks.send_ping = [] {
      return true;
    };
    hooks.reset_connection = [] {
    };
    hooks.session_count = [] {
      return 0;
    };
    hooks.running_processes = [] {
      return 1;
    };
    hooks.active_interval = kActive;
    hooks.suspended_interval = kSuspended;
    return hooks;
  }
}  // namespace

TEST(DisplayHelperWatchdog, HonorsConfiguredIntervals) {
  display_helper_integration::DisplayHelperWatchdog suspended(host_hooks());
  EXPECT_EQ(suspended.tick(), kSuspended);

  auto hooks = host_hooks();
  hooks.session_count = [] {
    return 1;
  };
  display_helper_integration::DisplayHelperWatchdog active(std::move(hooks));
  EXPECT_EQ(active.tick(), kActive);
}

TEST(DisplayHelperWatchdog, ConsultsSessionCountOncePerHealthyTick) {
  WatchdogHarness harness;
  harness.session_count = 0;
  harness.running_processes = 1;

  harness.watchdog.tick();  // starts the helper
  harness.watchdog.tick();  // healthy ping

  EXPECT_EQ(harness.session_count_calls, 2);
  EXPECT_EQ(harness.running_processes_calls, 2);
}

// running_processes() is proc::proc.running() on the host, which can terminate the app and stop the
// watchdog, so it is only asked when the session count already says the app is idle.
TEST(DisplayHelperWatchdog, DoesNotAskRunningProcessesWhileSessionsExist) {
  WatchdogHarness harness;
  harness.session_count = 2;
  harness.running_processes = 1;

  const auto interval = harness.watchdog.tick();

  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_EQ(harness.session_count_calls, 1);
  EXPECT_EQ(harness.running_processes_calls, 0);
}

TEST(DisplayHelperWatchdog, SessionHooksAreNotConsultedWhileDisabled) {
  WatchdogHarness harness;
  harness.feature_enabled = false;
  harness.session_count = 0;
  harness.running_processes = 1;

  const auto interval = harness.watchdog.tick();

  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_EQ(harness.session_count_calls, 0);
  EXPECT_EQ(harness.running_processes_calls, 0);
}

TEST(DisplayHelperWatchdog, HelperStartFailureUsesActiveIntervalWithoutSessionHooks) {
  WatchdogHarness harness;
  harness.ensure_result = false;
  harness.session_count = 0;
  harness.running_processes = 1;

  const auto interval = harness.watchdog.tick();

  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_FALSE(harness.watchdog.helper_ready());
  EXPECT_EQ(harness.session_count_calls, 0);
  EXPECT_EQ(harness.running_processes_calls, 0);
}

TEST(DisplayHelperWatchdog, RetriesImmediatelyWhenReconnectFails) {
  WatchdogHarness harness;
  harness.ping_results = {true};
  harness.watchdog.tick();

  harness.ping_results = {false};
  harness.ensure_result = false;

  EXPECT_EQ(harness.watchdog.tick(), 0ms);
  EXPECT_FALSE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, RetriesImmediatelyWhenRestartedHelperStillFailsPing) {
  WatchdogHarness harness;
  harness.ping_results = {true};
  harness.watchdog.tick();

  harness.ping_results = {false, false};

  EXPECT_EQ(harness.watchdog.tick(), 0ms);
  EXPECT_FALSE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, SuccessfulReconnectWaitsTheNormalInterval) {
  WatchdogHarness harness;
  harness.ping_results = {true};
  harness.watchdog.tick();

  harness.ping_results = {false, true};

  EXPECT_EQ(harness.watchdog.tick(), display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_TRUE(harness.watchdog.helper_ready());
}

// proc.running() can run terminate(), which stops the watchdog from the watchdog's own thread.
TEST(DisplayHelperWatchdog, StopFromRunningProcessesHookDoesNotSelfJoin) {
  auto slot = std::make_shared<display_helper_integration::WatchdogThreadSlot>();
  auto ticks = std::make_shared<std::atomic<int>>(0);
  auto threw = std::make_shared<std::atomic<bool>>(false);
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();

  ASSERT_TRUE(slot->start([slot, ticks, threw, done](std::stop_token token) {
    auto hooks = host_hooks();
    hooks.active_interval = 1ms;
    hooks.suspended_interval = 1ms;
    hooks.running_processes = [slot, ticks, threw] {
      ticks->fetch_add(1);
      try {
        (void) slot->stop();  // what proc_t::terminate() does through stop_watchdog()
      } catch (...) {
        threw->store(true);
      }
      return 1;
    };
    display_helper_integration::DisplayHelperWatchdog watchdog(std::move(hooks));
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(watchdog.tick());
    }
    done->set_value();
  }));

  ASSERT_EQ(finished.wait_for(5s), std::future_status::ready) << "watchdog loop did not exit after stop";
  EXPECT_FALSE(threw->load());
  EXPECT_EQ(ticks->load(), 1);
}
