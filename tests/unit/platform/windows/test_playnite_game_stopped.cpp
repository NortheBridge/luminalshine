/**
 * @file tests/unit/platform/windows/test_playnite_game_stopped.cpp
 * @brief Test the Playnite `gameStopped` hand-off policy in src/platform/windows/playnite_game_stopped.h.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <src/platform/windows/playnite_game_stopped.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  using platf::playnite::game_stopped_action_e;
  using platf::playnite::game_stopped_deps_t;
  using platf::playnite::game_stopped_guard_t;
  using platf::playnite::handle_game_stopped;
  using platf::playnite::run_game_stopped;
  using platf::playnite::single_flight_worker_t;
  using worker_event_e = single_flight_worker_t::event_e;
  using clock_type = std::chrono::steady_clock;

  // Upper bound for every wait on another thread. A correct run never reaches it; a regression
  // fails the test instead of hanging it.
  constexpr auto kWaitLimit = 10s;

  /**
   * @brief A one-shot latch that cannot hang a test.
   */
  class gate_t {
  public:
    void open() {
      {
        std::scoped_lock lk(mutex_);
        open_ = true;
      }
      cv_.notify_all();
    }

    /**
     * @return true when the gate opened within the wait limit.
     */
    bool wait() {
      std::unique_lock lk(mutex_);
      return cv_.wait_for(lk, kWaitLimit, [this]() {
        return open_;
      });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
  };

  /**
   * @brief A worker plus a thread-safe record of the events it reported.
   * @details The worker is the last member, so it is joined before the record it reports into dies.
   */
  struct recorded_worker_t {
    std::mutex mutex;
    std::vector<std::pair<worker_event_e, std::string>> events;
    single_flight_worker_t worker {[this](worker_event_e event, std::string_view detail) {
      std::scoped_lock lk(mutex);
      events.emplace_back(event, std::string(detail));
    }};

    std::size_t count(worker_event_e event) {
      std::scoped_lock lk(mutex);
      std::size_t n = 0;
      for (const auto &entry : events) {
        n += entry.first == event ? 1 : 0;
      }
      return n;
    }

    std::vector<std::string> details(worker_event_e event) {
      std::scoped_lock lk(mutex);
      std::vector<std::string> out;
      for (const auto &entry : events) {
        if (entry.first == event) {
          out.push_back(entry.second);
        }
      }
      return out;
    }

    /**
     * @brief Post once the previous task has finished and freed the worker.
     * @details A task signalling its own completion cannot tell when the worker becomes free again, so
     *          retry until a post is accepted. Dropped attempts are recorded as events.
     * @return false when no post was accepted within the wait limit.
     */
    bool post_when_idle(const std::function<void()> &task) {
      const auto deadline = clock_type::now() + kWaitLimit;
      while (!worker.post(task)) {
        if (clock_type::now() > deadline) {
          return false;
        }
        std::this_thread::yield();
      }
      return true;
    }
  };

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
    std::vector<game_stopped_guard_t> result_guards;

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
      deps.on_result = [this](game_stopped_action_e action, const game_stopped_guard_t &observed) {
        results.push_back(action);
        result_guards.push_back(observed);
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
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now()), game_stopped_action_e::ignored_no_playnite_app);

    // An active app that is not Playnite-backed.
    guard.has_active_app = true;
    guard.uses_playnite = false;
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now()), game_stopped_action_e::ignored_no_playnite_app);

    // A Playnite-backed app, but the stopped game is a different one.
    guard.uses_playnite = true;
    EXPECT_EQ(run_game_stopped(make_deps(), "game-b", clock_type::now()), game_stopped_action_e::ignored_other_game);

    // allow_stop consumes tracker state, so it must not run for any of the above.
    EXPECT_EQ(allow_stop_calls, 0);
    EXPECT_EQ(terminate_calls, 0);

    // Every outcome is reported with the guard it was decided on, so the log can name the active id.
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], game_stopped_action_e::ignored_no_playnite_app);
    EXPECT_EQ(results[1], game_stopped_action_e::ignored_no_playnite_app);
    EXPECT_EQ(results[2], game_stopped_action_e::ignored_other_game);
    ASSERT_EQ(result_guards.size(), 3u);
    EXPECT_EQ(result_guards[2].playnite_id, "game-a");
    EXPECT_EQ(result_guards[2].app_id, 7);
  }

  TEST_F(PlayniteGameStopped, IgnoresWithoutPriorStart) {
    allow_stop_result = false;

    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", clock_type::now()), game_stopped_action_e::ignored_no_prior_start);

    EXPECT_EQ(allow_stop_calls, 1);
    EXPECT_EQ(allow_stop_id, "game-a");
    EXPECT_EQ(terminate_calls, 0);
  }

  TEST_F(PlayniteGameStopped, GuardWindowMeasuredFromReceipt) {
    // Received 1s after launch: inside the 2s window.
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", launch + 1s), game_stopped_action_e::ignored_guard_window);
    EXPECT_EQ(terminate_calls, 0);
    // The tracker mark is consumed before the window is checked, so the early message still used it up.
    EXPECT_EQ(allow_stop_calls, 1);

    // Received 3s after launch: outside the window.
    EXPECT_EQ(run_game_stopped(make_deps(), "game-a", launch + 3s), game_stopped_action_e::terminated);
    EXPECT_EQ(terminate_calls, 1);

    // The posted task runs an hour after launch, but the message was received 1s after it:
    // the receipt time decides, not the time the task runs.
    terminate_calls = 0;
    results.clear();
    handle_game_stopped(make_deps(), "game-a", launch + 1s);
    run_posted();
    EXPECT_EQ(terminate_calls, 0);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), game_stopped_action_e::ignored_guard_window);
  }

  TEST_F(PlayniteGameStopped, WorkerDropsPostWhileTaskInFlight) {
    // Declared before the worker so they outlive the task even if an assertion returns early.
    gate_t started;
    gate_t release;
    std::atomic<int> ran {0};
    recorded_worker_t rec;

    EXPECT_TRUE(rec.worker.post([&]() {
      ++ran;
      started.open();
      release.wait();
    }));
    ASSERT_TRUE(started.wait());

    // The first task is still running: a second one is refused, not queued.
    EXPECT_FALSE(rec.worker.post([&]() {
      ++ran;
    }));
    EXPECT_EQ(rec.count(worker_event_e::dropped_in_flight), 1u);

    release.open();
    rec.worker.close();

    EXPECT_EQ(ran, 1);
  }

  TEST_F(PlayniteGameStopped, WorkerDropsPostAfterClose) {
    gate_t first_ran;
    std::atomic<int> ran {0};
    recorded_worker_t rec;

    EXPECT_TRUE(rec.worker.post([&]() {
      ++ran;
      first_ran.open();
    }));
    ASSERT_TRUE(first_ran.wait());
    rec.worker.close();

    // Closed for good: the finished worker is not restarted.
    EXPECT_FALSE(rec.worker.post([&]() {
      ++ran;
    }));
    EXPECT_EQ(rec.count(worker_event_e::dropped_closed), 1u);

    rec.worker.close();  // Closing twice is harmless.
    EXPECT_EQ(ran, 1);
  }

  TEST_F(PlayniteGameStopped, WorkerCloseWaitsForRunningTaskThatPosts) {
    gate_t started;
    gate_t release;
    std::atomic<bool> task_finished {false};
    std::atomic<bool> close_returned {false};
    std::atomic<bool> inner_accepted {true};
    std::atomic<int> inner_ran {0};
    recorded_worker_t rec;

    EXPECT_TRUE(rec.worker.post([&]() {
      started.open();
      release.wait();
      // Posting from inside the running task must be refused, not deadlock against close().
      inner_accepted = rec.worker.post([&]() {
        ++inner_ran;
      });
      task_finished = true;
    }));
    ASSERT_TRUE(started.wait());

    std::jthread closer([&]() {
      rec.worker.close();
      close_returned = true;
    });

    // While close() has not marked the worker closed yet, a post is refused as in flight, so this
    // loop is a safe way to learn that close() is now in progress, without sleeping.
    const auto deadline = clock_type::now() + kWaitLimit;
    while (rec.count(worker_event_e::dropped_closed) == 0 && clock_type::now() < deadline) {
      EXPECT_FALSE(rec.worker.post([]() {
      }));
      std::this_thread::yield();
    }
    ASSERT_GT(rec.count(worker_event_e::dropped_closed), 0u);
    const auto closed_drops_before = rec.count(worker_event_e::dropped_closed);

    // The task is still blocked, so close() cannot have returned yet.
    EXPECT_FALSE(close_returned);

    release.open();
    closer.join();

    EXPECT_TRUE(task_finished);
    EXPECT_TRUE(close_returned);
    EXPECT_FALSE(inner_accepted);
    EXPECT_EQ(inner_ran, 0);
    EXPECT_GT(rec.count(worker_event_e::dropped_closed), closed_drops_before);
  }

  TEST_F(PlayniteGameStopped, WorkerSurvivesThrowingTask) {
    gate_t done;
    std::atomic<int> ran {0};
    recorded_worker_t rec;

    // A std::exception and a non-std exception, each followed by a task that must still run.
    EXPECT_TRUE(rec.worker.post([]() {
      throw std::runtime_error("boom");
    }));
    EXPECT_TRUE(rec.post_when_idle([]() {
      throw 42;
    }));
    EXPECT_TRUE(rec.post_when_idle([&]() {
      ++ran;
      done.open();
    }));
    ASSERT_TRUE(done.wait());
    rec.worker.close();

    EXPECT_EQ(ran, 1);
    // The failure is reported before the worker is freed, so the order matches the posts.
    const auto failures = rec.details(worker_event_e::task_failed);
    ASSERT_EQ(failures.size(), 2u);
    EXPECT_EQ(failures[0], "boom");
    EXPECT_TRUE(failures[1].empty());
  }

}  // namespace
