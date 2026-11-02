/**
 * @file src/platform/windows/playnite_game_stopped.h
 * @brief Policy for handling Playnite `gameStopped` messages off the IPC pipe thread.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
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
    std::function<void(game_stopped_action_e)> on_result;  ///< Optional; logging and tests.
  };

  /**
   * @brief Decide what a `gameStopped` message does and terminate the session if it should.
   * @details The checks run in this order because `allow_stop` consumes state and must only run
   *          for a message that matches the active Playnite-backed app.
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
      const auto result = run_game_stopped(deps, game_id, received_at);
      if (deps.on_result) {
        deps.on_result(result);
      }
    });
  }

  /**
   * @brief Runs one task at a time on its own thread.
   * @details STUB for the RED commit: accepts nothing.
   */
  class single_flight_worker_t {
  public:
    enum class event_e {
      dropped_closed,
      dropped_in_flight,
      task_failed,
      spawn_failed,
    };

    using event_fn = std::function<void(event_e, std::string_view)>;

    explicit single_flight_worker_t(event_fn on_event = {}):
        on_event_ {std::move(on_event)} {}

    bool post(std::function<void()>) {
      return false;
    }

    void close() {}

  private:
    event_fn on_event_;
  };

}  // namespace platf::playnite
