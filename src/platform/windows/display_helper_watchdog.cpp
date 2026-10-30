#include "src/platform/windows/display_helper_watchdog.h"

#include "src/logging.h"

#include <utility>

using namespace std::chrono_literals;

namespace display_helper_integration {
  DisplayHelperWatchdog::DisplayHelperWatchdog(Hooks hooks):
      hooks_(std::move(hooks)) {}

  std::chrono::milliseconds DisplayHelperWatchdog::tick() {
    // Only a healthy helper waits the suspended interval. running_processes() can terminate the app
    // (and stop this watchdog), so it is asked only once the session count says nothing is streaming.
    const auto healthy_interval = [this]() {
      const bool suspended = hooks_.session_count && hooks_.running_processes &&
                             hooks_.session_count() == 0 && hooks_.running_processes() > 0;
      return suspended ? hooks_.suspended_interval : hooks_.active_interval;
    };

    if (hooks_.feature_enabled && !hooks_.feature_enabled()) {
      if (helper_ready_) {
        BOOST_LOG(debug) << "Display helper watchdog: feature disabled, releasing helper connection.";
        if (hooks_.reset_connection) {
          hooks_.reset_connection();
        }
      }
      helper_ready_ = false;
      return hooks_.active_interval;
    }

    if (!helper_ready_) {
      if (hooks_.ensure_helper_started && hooks_.ensure_helper_started()) {
        helper_ready_ = true;
        if (hooks_.send_ping) {
          (void) hooks_.send_ping();
        }
        return healthy_interval();
      }
      helper_ready_ = false;
      return hooks_.active_interval;
    }

    if (hooks_.send_ping && !hooks_.send_ping()) {
      BOOST_LOG(debug) << "Display helper watchdog: ping failed, helper may have crashed or become unresponsive.";
      if (hooks_.reset_connection) {
        hooks_.reset_connection();
      }
      if (hooks_.ensure_helper_started && hooks_.ensure_helper_started()) {
        helper_ready_ = hooks_.send_ping ? hooks_.send_ping() : true;
        if (helper_ready_) {
          BOOST_LOG(debug) << "Display helper watchdog: successfully restarted helper after ping failure.";
        } else {
          BOOST_LOG(debug) << "Display helper watchdog: helper restarted but ping still failing.";
        }
      } else {
        BOOST_LOG(debug) << "Display helper watchdog: failed to restart helper after ping failure.";
        helper_ready_ = false;
      }
      if (!helper_ready_) {
        // Retry right away, which is how the host loop behaved before it used tick().
        return 0ms;
      }
    }

    return healthy_interval();
  }

  void DisplayHelperWatchdog::reset() {
    helper_ready_ = false;
  }

  bool DisplayHelperWatchdog::helper_ready() const {
    return helper_ready_;
  }

  std::chrono::milliseconds DisplayHelperWatchdog::active_interval() {
    return 10s;
  }

  std::chrono::milliseconds DisplayHelperWatchdog::suspended_interval() {
    return 20s;
  }
}  // namespace display_helper_integration
