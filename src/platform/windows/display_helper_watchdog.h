#pragma once

#include <chrono>
#include <functional>
#include <thread>

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

  class DisplayHelperWatchdog {
  public:
    struct Hooks {
      std::function<bool()> feature_enabled;
      std::function<bool()> ensure_helper_started;
      std::function<bool()> send_ping;
      std::function<void()> reset_connection;
      std::function<int()> session_count;
      std::function<int()> running_processes;
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
