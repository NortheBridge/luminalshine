/**
 * @file src/session_teardown_policy.h
 * @brief Policy for session teardown watchdog phases.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace stream::session::teardown {

  enum class phase_e : int {
    starting = 0,
    waiting_video = 1,
    waiting_audio = 2,
    waiting_control = 3,
    resetting_input = 4,
    session_state = 5,
    display_cleanup = 6,
    integration_cleanup = 7,
    frame_limiter_cleanup = 8,
    platform_cleanup = 9,
    deferred_config = 10,
    done = 11,
  };

  enum class timeout_action_e {
    fast_exit,
    controlled_restart,
  };

  constexpr const char *phase_name(phase_e phase) noexcept {
    switch (phase) {
      case phase_e::starting:
        return "starting";
      case phase_e::waiting_video:
        return "videoThread.join";
      case phase_e::waiting_audio:
        return "audioThread.join";
      case phase_e::waiting_control:
        return "controlEnd.view";
      case phase_e::resetting_input:
        return "input::reset";
      case phase_e::session_state:
        return "session-state release";
      case phase_e::display_cleanup:
        return "virtual-display cleanup";
      case phase_e::integration_cleanup:
        return "HDR/RTSS integration cleanup";
      case phase_e::frame_limiter_cleanup:
        return "frame-limiter/NVCP cleanup";
      case phase_e::platform_cleanup:
        return "platform cleanup";
      case phase_e::deferred_config:
        return "deferred-config apply";
      case phase_e::done:
        return "done";
    }
    return "unknown";
  }

  /**
   * Thread joins and input reset can leave live capture resources behind, so
   * the process must be terminated promptly.  Cleanup after running_sessions
   * reaches zero is different: the control plane has already released the
   * session, so request an orderly service recycle rather than killing the
   * host from the task-pool thread.
   */
  constexpr timeout_action_e timeout_action(phase_e phase) noexcept {
    return phase <= phase_e::resetting_input ?
             timeout_action_e::fast_exit :
             timeout_action_e::controlled_restart;
  }

  constexpr bool watchdog_arm_is_current(
    std::uint64_t expected_epoch,
    std::uint64_t current_epoch,
    phase_e current_phase
  ) noexcept {
    return expected_epoch == current_epoch && current_phase != phase_e::done;
  }

  /**
   * @brief A watchdog that cannot be starved by the single-thread task pool.
   *
   * Cleanup work itself may run on task_pool. A delayed task queued to that
   * same pool can therefore never observe a blocked cleanup. This helper owns
   * a dedicated sleeping thread and joins it promptly when complete().
   */
  class independent_watchdog_t {
  public:
    independent_watchdog_t(
      std::chrono::milliseconds timeout,
      std::function<void()> on_timeout
    ):
        worker_ {[this, timeout, on_timeout = std::move(on_timeout)]() mutable {
          std::unique_lock lock {mutex_};
          if (cv_.wait_for(lock, timeout, [this]() { return completed_; })) {
            return;
          }
          lock.unlock();
          on_timeout();
        }} {
    }

    independent_watchdog_t(const independent_watchdog_t &) = delete;
    independent_watchdog_t &operator=(const independent_watchdog_t &) = delete;

    ~independent_watchdog_t() {
      complete();
    }

    void complete() noexcept {
      {
        std::lock_guard lock {mutex_};
        completed_ = true;
      }
      cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool completed_ = false;
    // Declared last so its destructor joins before synchronization members die.
    std::jthread worker_;
  };

}  // namespace stream::session::teardown
