/**
 * @file src/platform/windows/playnite_game_stopped.h
 * @brief Policy for handling Playnite `gameStopped` messages off the IPC pipe thread.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace platf::playnite {

  /**
   * @brief How long after a launch starts a `gameStopped` message is treated as left over from the previous game.
   */
  inline constexpr std::chrono::seconds kGameStoppedLaunchGuardWindow {2};

  /**
   * @brief The active session, reduced to what the `gameStopped` policy needs.
   */
  struct game_stopped_guard_t {
    bool has_active_app {false};
    int app_id {0};
    bool uses_playnite {false};
    std::string playnite_id;
    std::string client_uuid;
    std::chrono::steady_clock::time_point launch_started_at {};
  };

  /**
   * @brief What a `gameStopped` message ended up doing.
   */
  enum class game_stopped_action_e {
    ignored_no_playnite_app,  ///< No active app, or the active app is not Playnite-backed.
    ignored_other_game,  ///< The stopped game is not the active Playnite-backed app.
    ignored_no_prior_start,  ///< No matching `gameStarted` was seen for this session.
    ignored_guard_window,  ///< Received within the launch guard window.
    terminated,  ///< The observed session was terminated.
    generation_changed,  ///< A newer launch replaced the observed session before it could be terminated.
  };

  /**
   * @brief The seams between the `gameStopped` policy and the process, tracker and thread it acts on.
   * @details Everything except `post` runs on the thread `post` hands the task to. `post` is the only
   *          member called on the IPC pipe thread, so it must not block on session state or join threads.
   */
  struct game_stopped_deps_t {
    std::function<game_stopped_guard_t()> get_guard;  ///< Snapshot of the active session.
    std::function<bool(const std::string &)> allow_stop;  ///< Consumes the `gameStarted` mark; true when a stop may proceed.
    std::function<bool(int, std::chrono::steady_clock::time_point, std::string_view)> terminate_if_current;  ///< Terminates the exact launch generation; false when it is no longer current.
    std::function<void(std::function<void()>)> post;  ///< Hands a task to the stop worker.
    std::function<void(game_stopped_action_e, const game_stopped_guard_t &)> on_result;  ///< Optional; logging and tests. Gets the guard the decision was made on.
  };

  namespace detail {
    inline game_stopped_action_e decide_game_stopped(
      const game_stopped_deps_t &deps,
      const game_stopped_guard_t &guard,
      const std::string &game_id,
      std::chrono::steady_clock::time_point received_at
    ) {
      if (!guard.has_active_app || !guard.uses_playnite) {
        return game_stopped_action_e::ignored_no_playnite_app;
      }
      if (!game_id.empty() && !guard.playnite_id.empty() && game_id != guard.playnite_id) {
        return game_stopped_action_e::ignored_other_game;
      }
      if (!deps.allow_stop(game_id)) {
        return game_stopped_action_e::ignored_no_prior_start;
      }
      if (guard.launch_started_at.time_since_epoch().count() != 0 &&
          received_at - guard.launch_started_at < kGameStoppedLaunchGuardWindow) {
        return game_stopped_action_e::ignored_guard_window;
      }
      return deps.terminate_if_current(guard.app_id, guard.launch_started_at, guard.client_uuid) ?
               game_stopped_action_e::terminated :
               game_stopped_action_e::generation_changed;
    }
  }  // namespace detail

  /**
   * @brief Decide what a `gameStopped` message does and terminate the session if it should.
   * @details The checks run in this order because `allow_stop` consumes state and must only run
   *          for a message that matches the active Playnite-backed app. The outcome is also
   *          reported to `deps.on_result`, once, together with the guard it was decided on.
   * @param deps The process, tracker and thread seams.
   * @param game_id The Playnite id carried by the message; may be empty.
   * @param received_at When the message arrived on the IPC thread. The launch guard window is
   *                    measured from here, not from when the task runs, so a delayed task cannot
   *                    turn a message that arrived early in a launch into a termination.
   * @return What the message ended up doing.
   */
  inline game_stopped_action_e run_game_stopped(
    const game_stopped_deps_t &deps,
    const std::string &game_id,
    std::chrono::steady_clock::time_point received_at
  ) {
    const game_stopped_guard_t guard = deps.get_guard();
    const auto action = detail::decide_game_stopped(deps, guard, game_id, received_at);
    if (deps.on_result) {
      deps.on_result(action, guard);
    }
    return action;
  }

  /**
   * @brief Hand a `gameStopped` message to the stop worker; nothing else runs on the calling thread.
   * @details Terminating the session stops the Playnite IPC client, which joins the pipe thread that
   *          delivered this message. Doing that on the pipe thread joins it from itself, and taking the
   *          session lifecycle lock there can deadlock against a quit that joins it.
   * @param deps The process, tracker and thread seams; copied into the posted task.
   * @param game_id The Playnite id carried by the message.
   * @param received_at When the message arrived on the IPC thread.
   */
  inline void handle_game_stopped(
    game_stopped_deps_t deps,
    std::string game_id,
    std::chrono::steady_clock::time_point received_at
  ) {
    auto post = deps.post;
    post([deps = std::move(deps), game_id = std::move(game_id), received_at]() {
      run_game_stopped(deps, game_id, received_at);
    });
  }

  /**
   * @brief Runs at most one task at a time on a thread of its own.
   * @details A task posted while another one is running is dropped, not queued, and so is every task
   *          posted after close(). Neither the lock nor the caller's thread is ever held across a join:
   *          post() joins the previous, already finished worker after releasing the lock, and close()
   *          joins after releasing it too. A task that throws is reported and does not end the process.
   *          A task may call post() (it is refused while the task runs) but must not call close() or
   *          destroy the worker, which would join the task's own thread.
   */
  class single_flight_worker_t {
  public:
    /**
     * @brief What the worker reports through the optional event callback.
     */
    enum class event_e {
      dropped_closed,  ///< post() refused the task because close() was called.
      dropped_in_flight,  ///< post() refused the task because another one is still running.
      task_failed,  ///< A task threw; the detail is the message, or empty for a non-std exception.
      spawn_failed,  ///< The worker thread could not be started; the detail is the error message.
    };

    /**
     * @brief Called without the lock held, on the posting thread (drops, spawn failures) or on the worker
     *        thread (task failures). It must not call back into the worker; an exception it throws is ignored.
     */
    using event_fn = std::function<void(event_e, std::string_view)>;

    explicit single_flight_worker_t(event_fn on_event = {}):
        on_event_ {std::move(on_event)} {}

    single_flight_worker_t(const single_flight_worker_t &) = delete;
    single_flight_worker_t &operator=(const single_flight_worker_t &) = delete;

    ~single_flight_worker_t() {
      close();
    }

    /**
     * @brief Run `task` on the worker thread unless one is running or the worker is closed.
     * @return true when the task was accepted.
     */
    bool post(std::function<void()> task) {
      std::jthread finished;  // The previous worker; declared before the lock so it is joined after it.
      std::optional<event_e> refused;
      std::string detail;
      {
        std::scoped_lock lk(mutex_);
        if (closed_) {
          refused = event_e::dropped_closed;
        } else if (in_flight_.exchange(true, std::memory_order_acq_rel)) {
          refused = event_e::dropped_in_flight;
        } else {
          // The flag was clear, so the previous worker has finished its task and is only exiting.
          finished = std::move(worker_);
          try {
            worker_ = std::jthread([this, task = std::move(task)]() mutable {
              run(task);
            });
            return true;
          } catch (const std::exception &e) {
            in_flight_.store(false, std::memory_order_release);
            refused = event_e::spawn_failed;
            detail = e.what();
          }
        }
      }
      emit(*refused, detail);
      return false;
    }

    /**
     * @brief Refuse every later post and wait for the running task. Safe to call more than once.
     */
    void close() {
      std::jthread worker;
      {
        std::scoped_lock lk(mutex_);
        closed_ = true;
        worker = std::move(worker_);
      }
      if (worker.joinable()) {
        worker.join();
      }
    }

  private:
    void run(std::function<void()> &task) {
      try {
        task();
      } catch (const std::exception &e) {
        emit(event_e::task_failed, e.what());
      } catch (...) {
        emit(event_e::task_failed, {});
      }
      // Last use of `this`: close() joins this thread before the worker can be destroyed.
      in_flight_.store(false, std::memory_order_release);
    }

    void emit(event_e event, std::string_view detail) const noexcept {
      if (!on_event_) {
        return;
      }
      try {
        on_event_(event, detail);
      } catch (...) {
        // A failing observer must not leave the worker marked busy or end the process.
      }
    }

    const event_fn on_event_;
    std::mutex mutex_;  ///< Guards worker_ and closed_; never held across a join.
    std::jthread worker_;  ///< The thread running the current or most recent task.
    bool closed_ {false};
    std::atomic<bool> in_flight_ {false};  ///< True from accepting a task until it has finished.
  };

}  // namespace platf::playnite
