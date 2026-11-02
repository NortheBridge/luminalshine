/**
 * @file tests/unit/platform/windows/test_playnite_game_stopped.cpp
 * @brief Test the Playnite `gameStopped` hand-off policy in src/platform/windows/playnite_game_stopped.h.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <src/platform/windows/playnite_game_stopped.h>

namespace {

  using namespace std::chrono_literals;
  using platf::playnite::game_stopped_action_e;
  using platf::playnite::game_stopped_deps_t;
  using platf::playnite::game_stopped_guard_t;
  using platf::playnite::handle_game_stopped;
  using platf::playnite::run_game_stopped;
  using clock_type = std::chrono::steady_clock;

  /**
   * @brief Fake deps that count every call and keep posted tasks instead of running them.
   */
  class PlayniteGameStopped: public testing::Test {
  protected:
    // An arbitrary non-zero launch time, far enough from the clock epoch to subtract from.
    const clock_type::time_point launch = clock_type::now() - 1h;

    game_stopped_guard_t guard {true, 7, true, "game-a", "client-1", launch};
    bool allow_stop_result = true;
    bool terminate_result = true;

    int get_guard_calls = 0;
    int allow_stop_calls = 0;
    int terminate_calls = 0;
    std::string allow_stop_id;
    int terminate_app_id = 0;
    clock_type::time_point terminate_launch;
    std::string terminate_uuid;
    std::vector<std::function<void()>> posted;
    std::vector<game_stopped_action_e> results;

    game_stopped_deps_t make_deps() {
      game_stopped_deps_t deps;
      deps.get_guard = [this]() {
        ++get_guard_calls;
        return guard;
      };
      deps.allow_stop = [this](const std::string &id) {
        ++allow_stop_calls;
        allow_stop_id = id;
        return allow_stop_result;
      };
      deps.terminate_if_current = [this](int app_id, clock_type::time_point launch_started_at, std::string_view client_uuid) {
        ++terminate_calls;
        terminate_app_id = app_id;
        terminate_launch = launch_started_at;
        terminate_uuid = std::string(client_uuid);
        return terminate_result;
      };
      deps.post = [this](std::function<void()> task) {
        posted.push_back(std::move(task));
      };
      deps.on_result = [this](game_stopped_action_e action) {
        results.push_back(action);
      };
      return deps;
    }

    void run_posted() {
      ASSERT_EQ(posted.size(), 1u);
      posted.front()();
    }
  };

  TEST_F(PlayniteGameStopped, HandlerOnlyPostsOnIpcThread) {
    handle_game_stopped(make_deps(), "game-a", clock_type::now());

    EXPECT_EQ(posted.size(), 1u);
    EXPECT_EQ(get_guard_calls, 0);
    EXPECT_EQ(allow_stop_calls, 0);
    EXPECT_EQ(terminate_calls, 0);
    EXPECT_TRUE(results.empty());

    run_posted();

    EXPECT_EQ(get_guard_calls, 1);
    EXPECT_EQ(allow_stop_calls, 1);
    EXPECT_EQ(terminate_calls, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), game_stopped_action_e::terminated);
  }

  TEST_F(PlayniteGameStopped, PostedTaskTerminatesTheObservedGeneration) {
    handle_game_stopped(make_deps(), "game-a", clock_type::now());
    run_posted();

    EXPECT_EQ(terminate_calls, 1);
    EXPECT_EQ(terminate_app_id, 7);
    EXPECT_EQ(terminate_launch, launch);
    EXPECT_EQ(terminate_uuid, "client-1");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), game_stopped_action_e::terminated);

    // terminate_if_current declines when a newer launch replaced the one the guard saw.
    posted.clear();
    results.clear();
    terminate_result = false;
    handle_game_stopped(make_deps(), "game-a", clock_type::now());
    run_posted();

    EXPECT_EQ(terminate_calls, 2);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), game_stopped_action_e::generation_changed);
  }

  TEST_F(PlayniteGameStopped, IgnoresWhenNoPlayniteAppOrOtherGame) {
    // No active app at all.
    guard.has_active_app = false;
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now(), clock_type::now()), game_stopped_action_e::ignored_no_playnite_app);

    // An active app that is not Playnite-backed.
    guard.has_active_app = true;
    guard.uses_playnite = false;
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now(), clock_type::now()), game_stopped_action_e::ignored_no_playnite_app);

    // A Playnite-backed app, but the stopped game is a different one.
    guard.uses_playnite = true;
    EXPECT_EQ(run_game_stopped(make_deps(), "game-b", clock_type::now(), clock_type::now()), game_stopped_action_e::ignored_other_game);

    // allow_stop consumes tracker state, so it must not run for any of the above.
    EXPECT_EQ(allow_stop_calls, 0);
    EXPECT_EQ(terminate_calls, 0);
  }

  TEST_F(PlayniteGameStopped, IgnoresWithoutPriorStart) {
    allow_stop_result = false;

    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now(), clock_type::now()), game_stopped_action_e::ignored_no_prior_start);

    EXPECT_EQ(allow_stop_calls, 1);
    EXPECT_EQ(allow_stop_id, "game-a");
    EXPECT_EQ(terminate_calls, 0);
  }

  TEST_F(PlayniteGameStopped, GuardWindowMeasuredFromReceipt) {
    // Received 1s after launch: inside the 2s window, even though the task only runs 5s after launch.
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", launch + 1s, launch + 5s), game_stopped_action_e::ignored_guard_window);
    EXPECT_EQ(terminate_calls, 0);

    // Received 3s after launch: outside the window.
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", launch + 3s, launch + 5s), game_stopped_action_e::terminated);
    EXPECT_EQ(terminate_calls, 1);

    // The same receipt time decides when the task runs later through the posted path.
    terminate_calls = 0;
    handle_game_stopped(make_deps(), "game-a", launch + 1s);
    run_posted();
    EXPECT_EQ(terminate_calls, 0);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), game_stopped_action_e::ignored_guard_window);
  }

}  // namespace
