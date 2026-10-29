#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace display_helper_integration {
  /**
   * @brief Request stop on a watchdog thread and wait for it to exit.
   *
   * When called from the watchdog thread itself the thread is detached instead of joined.
   */
  inline void stop_watchdog_thread(std::jthread &thread) {
    if (thread.joinable()) {
      thread.request_stop();
      if (thread.get_id() == std::this_thread::get_id()) {
        // A session teardown or an app termination can run on the watchdog's own loop; joining itself throws EDEADLK.
        thread.detach();
        return;
      }
      thread.join();
    }
  }

  /**
   * @brief Owns the watchdog thread and serializes start and stop.
   *
   * The thread is moved out under the lock and stopped outside it, so a concurrent start() never
   * assigns over a thread that is still being stopped, and the stopping side never holds the lock
   * while it joins.
   */
  class WatchdogThreadSlot {
  public:
    /**
     * @brief Start the thread unless one is already running.
     * @return true when this call started it.
     */
    template<typename Fn>
    bool start(Fn &&fn) {
      std::lock_guard lock(mutex_);
      if (thread_.joinable()) {
        return false;
      }
      thread_ = std::jthread(std::forward<Fn>(fn));
      return true;
    }

    /**
     * @brief Stop the running thread, if any.
     * @return true when a thread was running and this call stopped it.
     */
    bool stop() {
      std::jthread thread;
      {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) {
          return false;
        }
        thread = std::move(thread_);
      }
      stop_watchdog_thread(thread);
      return true;
    }

  private:
    std::mutex mutex_;
    std::jthread thread_;
  };

  class DisplayHelperWatchdog {
  public:
    struct Hooks {
      std::function<bool()> feature_enabled;
      std::function<bool()> ensure_helper_started;
      std::function<bool()> send_ping;
      std::function<void()> reset_connection;
      std::function<int()> session_count;
      std::function<int()> running_processes;
      /// Wait before the next tick; the host loop picks its own, the defaults match active_interval().
      std::chrono::milliseconds active_interval {10000};
      /// Wait when no session is streaming but a process is still running.
      std::chrono::milliseconds suspended_interval {20000};
    };

    explicit DisplayHelperWatchdog(Hooks hooks);

    std::chrono::milliseconds tick();
    void reset();
    bool helper_ready() const;

    static std::chrono::milliseconds active_interval();
    static std::chrono::milliseconds suspended_interval();

  private:
    Hooks hooks_;
    bool helper_ready_ = false;
  };
}  // namespace display_helper_integration
