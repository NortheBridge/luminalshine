/**
 * @file tests/unit/test_session_teardown_policy.cpp
 * @brief Unit tests for session teardown timeout policy.
 */

#include "src/session_teardown_policy.h"

#ifdef _WIN32
  #include "src/platform/windows/virtual_display_cleanup.h"

  #include <filesystem>
  #include <fstream>
  #include <regex>
  #include <sstream>
  #include <string>
#endif

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

  TEST(SessionTeardownPolicy, DisplayCleanupDeadlineOutlastsItsBudget) {
    constexpr auto budget = std::chrono::seconds(60);
    EXPECT_GT(stream::session::teardown::display_cleanup_deadline(budget), budget);
  }

  TEST(SessionTeardownPolicy, DisplayCleanupDeadlineNeverDropsBelowItsFloor) {
    using namespace std::chrono_literals;
    EXPECT_EQ(stream::session::teardown::display_cleanup_deadline(5s), 30s);
    EXPECT_EQ(stream::session::teardown::display_cleanup_deadline(5s, 60s), 60s);
  }

#ifdef _WIN32
  namespace cleanup = platf::virtual_display_cleanup;

  TEST(SessionTeardownPolicy, DisplayCleanupDeadlineOutlastsTheCleanupWorstCase) {
    for (const bool enforce_db_restore : {true, false}) {
      const auto worst_case = cleanup::worst_case_duration(enforce_db_restore);
      EXPECT_GT(stream::session::teardown::display_cleanup_deadline(worst_case), worst_case) << "enforce_db_restore=" << enforce_db_restore;
    }
  }

  TEST(SessionTeardownPolicy, DisplayCleanupDeadlineCoversTheHelperRestartPath) {
    // First REVERT wait, helper restart, second REVERT wait: the path that
    // timed out on 26.09.1 before the recovery ladder could run.
    const auto restart_path = cleanup::kRevertCompletionTimeout + cleanup::detail::kHelperRestartBound + cleanup::kRevertCompletionTimeout;
    EXPECT_GT(stream::session::teardown::display_cleanup_deadline(cleanup::worst_case_duration()), restart_path);
  }

  TEST(SessionTeardownPolicy, DisplayCleanupBudgetAndDeadlinesArePinned) {
    using namespace std::chrono_literals;
    // A change to a cleanup wait or to a mirrored helper timeout moves these
    // numbers. Update them in the same change so the effect on the teardown
    // deadline is visible in review.
    EXPECT_EQ(cleanup::worst_case_duration(), 145s);
    EXPECT_EQ(cleanup::worst_case_duration(false), 5s);
    EXPECT_EQ(stream::session::teardown::display_cleanup_deadline(cleanup::worst_case_duration()), 155s);
    EXPECT_EQ(stream::session::teardown::display_cleanup_deadline(cleanup::worst_case_duration(false)), 30s);
  }

  TEST(SessionTeardownPolicy, MirroredHelperTimeoutsMatchTheirSources) {
  #ifndef SUNSHINE_SOURCE_DIR
    FAIL() << "SUNSHINE_SOURCE_DIR is not defined. test_sunshine gets it from tests/CMakeLists.txt; "
           << "without it the mirrored helper timeouts cannot be checked against their sources.";
  #else
    // virtual_display_cleanup.h mirrors timeouts that are file-local in their
    // sources. Each pattern captures the value, in milliseconds.
    struct mirror_t {
      const char *source_file;
      const char *pattern;
      std::chrono::milliseconds mirror;
    };

    const mirror_t mirrors[] = {
      {"src/platform/windows/display_helper_integration.cpp", R"(constexpr DWORD kHelperForceKillWaitMs = (\d+);)", cleanup::detail::kHelperForceKillWait},
      {"src/platform/windows/display_helper_integration.cpp", R"(kHelperIpcReadyTimeout \{(\d+)\})", cleanup::detail::kHelperIpcReadyTimeout},
      {"src/platform/windows/ipc/display_settings_client.cpp", R"(kConnectTimeoutMs = (\d+);)", cleanup::detail::kHelperIpcConnectTimeout},
      {"src/platform/windows/ipc/display_settings_client.cpp", R"(kRevertAcceptedTimeoutMs = (\d+);)", cleanup::detail::kHelperRevertAcceptedTimeout},
      {"src/platform/windows/ipc/display_settings_client.cpp", R"(kApplyResultTimeoutMs = (\d+);)", cleanup::detail::kHelperApplyResultTimeout},
    };

    for (const auto &[source_file, pattern, mirror] : mirrors) {
      SCOPED_TRACE(std::string(source_file) + ": " + pattern);
      const auto path = std::filesystem::path(SUNSHINE_SOURCE_DIR) / source_file;
      std::ifstream in(path, std::ios::binary);
      ASSERT_TRUE(in.is_open()) << "cannot read " << path.string();
      std::ostringstream text;
      text << in.rdbuf();
      const auto source = text.str();
      std::smatch match;
      ASSERT_TRUE(std::regex_search(source, match, std::regex(pattern)))
        << "the constant was renamed or moved; update its mirror in virtual_display_cleanup.h and this test";
      EXPECT_EQ(std::chrono::milliseconds(std::stoll(match[1].str())), mirror) << "the source changed; update the mirror in virtual_display_cleanup.h";
    }
  #endif
  }
#endif

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
