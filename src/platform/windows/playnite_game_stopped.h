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

namespace platf::playnite {

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
    ignored_no_playnite_app,
    ignored_other_game,
    ignored_no_prior_start,
    ignored_guard_window,
    terminated,
    generation_changed,
  };

  /**
   * @brief The seams between the `gameStopped` policy and the process, tracker and thread it acts on.
   */
  struct game_stopped_deps_t {
    std::function<game_stopped_guard_t()> get_guard;
    std::function<bool(const std::string &)> allow_stop;
    std::function<bool(int, std::chrono::steady_clock::time_point, std::string_view)> terminate_if_current;
    std::function<void(std::function<void()>)> post;
    std::function<void(game_stopped_action_e)> on_result;
  };

  inline game_stopped_action_e run_game_stopped(
    const game_stopped_deps_t &,
    const std::string &,
    std::chrono::steady_clock::time_point,
    std::chrono::steady_clock::time_point
  ) {
    return game_stopped_action_e::ignored_no_playnite_app;
  }

  inline void handle_game_stopped(
    game_stopped_deps_t deps,
    std::string game_id,
    std::chrono::steady_clock::time_point received_at
  ) {
    const auto result = run_game_stopped(deps, game_id, received_at, std::chrono::steady_clock::now());
    if (deps.on_result) {
      deps.on_result(result);
    }
  }

}  // namespace platf::playnite
