/**
 * @file src/platform/windows/ipc/pipe_connect_retry.h
 * @brief Pure retry policy for connecting a named-pipe client to a server that may still be starting.
 *
 * A pipe client that races the server's startup sees ERROR_FILE_NOT_FOUND until the server has
 * called CreateNamedPipe, and ERROR_PIPE_BUSY while every instance is taken. This header holds the
 * decision logic for that loop — how long to keep polling, what to wait on, and when to stop —
 * without touching any Win32 API, so it can be unit-tested on every platform. The Win32 loop in
 * NamedPipeFactory::create_client_pipe feeds it one classified attempt at a time.
 *
 * The wait is bounded in two ways: by a deadline (`ConnectRetryPolicy::max_wait`, an upper bound
 * the caller keeps short for fire-and-forget commands and long for a freshly launched server),
 * and by server-process liveness. When the caller knows the server process has exited there is
 * no point in polling until the deadline, so the policy gives up immediately.
 */
#pragma once

#include <algorithm>
#include <chrono>

namespace platf::ipc {

  /**
   * @brief Classification of a single CreateFileW attempt against a named pipe.
   */
  enum class ConnectAttempt {
    connected,  ///< The handle was opened.
    server_not_found,  ///< ERROR_FILE_NOT_FOUND: no server has created the pipe (yet).
    server_busy,  ///< ERROR_PIPE_BUSY: every instance is in use; WaitNamedPipe is the right wait.
    fatal,  ///< Any other error. Retrying cannot help.
  };

  /**
   * @brief What the connect loop should do after an attempt.
   */
  enum class ConnectAction {
    done,  ///< The attempt succeeded; stop.
    poll_not_found,  ///< Sleep for `ConnectStep::wait`, then call CreateFileW again.
    wait_busy,  ///< WaitNamedPipe for up to `ConnectStep::wait`, then call CreateFileW again.
    give_up,  ///< Stop trying; `ConnectStep::reason` says why.
  };

  /**
   * @brief Why the loop stopped without a connection.
   */
  enum class GiveUpReason {
    none,
    fatal_error,  ///< The last attempt failed with an error retrying cannot fix.
    server_exited,  ///< The caller reported the server process gone.
    deadline,  ///< `ConnectRetryPolicy::max_wait` elapsed.
  };

  /**
   * @brief Tunables for the connect loop.
   *
   * The defaults reproduce the historical behaviour (500 ms cap, 50 ms polling while the pipe does
   * not exist, 250 ms WaitNamedPipe while busy) so callers that do not opt in are unchanged.
   */
  struct ConnectRetryPolicy {
    std::chrono::milliseconds max_wait {500};  ///< Upper bound on the whole connect, including the first attempt.
    std::chrono::milliseconds not_found_poll {50};  ///< Sleep between attempts while the pipe does not exist.
    std::chrono::milliseconds busy_wait {250};  ///< WaitNamedPipe timeout while all instances are busy.
  };

  /**
   * @brief One decision of the connect loop.
   */
  struct ConnectStep {
    ConnectAction action {ConnectAction::give_up};
    std::chrono::milliseconds wait {0};  ///< How long to sleep / wait before the next attempt (never longer than the time left).
    GiveUpReason reason {GiveUpReason::none};

    constexpr bool operator==(const ConnectStep &) const = default;
  };

  /**
   * @brief Decide what to do after a connect attempt.
   * @param policy Tunables (deadline and per-state waits).
   * @param elapsed Time since the first attempt started.
   * @param attempt Classification of the attempt that just finished.
   * @param server_exited True when the caller knows the server process is gone. Ignored for a
   *                      successful attempt: an open handle is an open handle.
   * @return The next step. Waits are clamped to the remaining budget so the loop never overshoots
   *         `policy.max_wait` by more than one attempt, and are always at least 1 ms when non-zero.
   */
  constexpr ConnectStep next_connect_step(
    const ConnectRetryPolicy &policy,
    std::chrono::milliseconds elapsed,
    ConnectAttempt attempt,
    bool server_exited
  ) {
    using std::chrono::milliseconds;

    if (attempt == ConnectAttempt::connected) {
      return {ConnectAction::done, milliseconds {0}, GiveUpReason::none};
    }
    if (attempt == ConnectAttempt::fatal) {
      return {ConnectAction::give_up, milliseconds {0}, GiveUpReason::fatal_error};
    }
    if (server_exited) {
      return {ConnectAction::give_up, milliseconds {0}, GiveUpReason::server_exited};
    }

    const milliseconds remaining = policy.max_wait - elapsed;
    if (remaining <= milliseconds {0}) {
      return {ConnectAction::give_up, milliseconds {0}, GiveUpReason::deadline};
    }

    if (attempt == ConnectAttempt::server_busy) {
      const auto wait = std::clamp(policy.busy_wait, milliseconds {1}, remaining);
      return {ConnectAction::wait_busy, wait, GiveUpReason::none};
    }

    const auto wait = std::clamp(policy.not_found_poll, milliseconds {1}, remaining);
    return {ConnectAction::poll_not_found, wait, GiveUpReason::none};
  }

}  // namespace platf::ipc
