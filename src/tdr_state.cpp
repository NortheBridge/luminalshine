/**
 * @file src/tdr_state.cpp
 * @brief Implementation of the process-wide TDR / WDDM-failure tracker.
 */
#include "tdr_state.h"

#include "logging.h"

#include <atomic>
#include <mutex>
#include <string_view>

#if defined(SUNSHINE_BUILD_TRAY) || defined(_WIN32)
  #include "system_tray.h"
#endif

namespace tdr {

  namespace {
    std::mutex g_mutex;
    std::optional<event_t> g_last;
    std::atomic<std::uint64_t> g_count {0};
    std::chrono::steady_clock::time_point g_last_log_at {};
    std::chrono::steady_clock::time_point g_last_notify_at {};

    // Cooldowns. Each TDR-class failure cascades through several call
    // sites (encoder D3D11, DD test, virtual display enumerate, ...), all
    // firing within seconds of each other. The first event in a cluster
    // emits the high-signal log line and the tray toast; the rest record
    // into `g_last` and increment the counter but stay silent. After the
    // cooldown elapses with no new events, the next event is treated as
    // a fresh incident.
    constexpr std::chrono::seconds kLogCooldown {30};
    constexpr std::chrono::minutes kNotifyCooldown {2};
  }  // namespace

  void mark_event(source_t source, long hresult, std::string detail) {
    using clock = std::chrono::steady_clock;
    const auto now_steady = clock::now();
    const auto now_system = std::chrono::system_clock::now();

    event_t event;
    event.at = now_system;
    event.source = source;
    event.hresult = hresult;
    event.detail = detail;

    bool should_log = false;
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_last = event;
      g_count.fetch_add(1, std::memory_order_relaxed);

      if (g_last_log_at == clock::time_point {} || (now_steady - g_last_log_at) >= kLogCooldown) {
        g_last_log_at = now_steady;
        should_log = true;
      }
      if (g_last_notify_at == clock::time_point {} || (now_steady - g_last_notify_at) >= kNotifyCooldown) {
        g_last_notify_at = now_steady;
        should_notify = true;
      }
    }

    if (should_log) {
      // The signature log line. Lets a future incident be triaged with
      // a single grep instead of correlating D3D11 + QueryDisplayConfig
      // + virtual-display-enumerate failures by hand.
      BOOST_LOG(error) << "GPU TDR escalated to WDDM stack failure ("
                       << source_label(source) << ", hresult=0x"
                       << std::hex << hresult << std::dec
                       << "): " << detail
                       << ". Active session will be terminated; recovery is in progress.";
    }

#if defined(SUNSHINE_BUILD_TRAY) || defined(_WIN32)
    if (should_notify) {
      // Rate-limited so the user gets one toast per incident, not one per
      // call site that fires inside the same TDR cascade.
      const std::string body = std::string("GPU TDR detected (")
        + source_label(source) + "). The active stream was ended; "
        + "the display stack is recovering.";
      try {
        system_tray::tray_notify(
          "LuminalShine: Virtual Display Adapter Failure",
          body.c_str(),
          nullptr);
      } catch (...) {
        // Tray subsystem may not be up yet. Non-fatal.
      }
    }
#else
    (void) should_notify;
#endif
  }

  bool recovery_recent(std::chrono::seconds within) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_last) {
      return false;
    }
    const auto now = std::chrono::system_clock::now();
    return (now - g_last->at) <= within;
  }

  std::optional<event_t> last_event() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_last;
  }

  std::uint64_t event_count() {
    return g_count.load(std::memory_order_relaxed);
  }

  const char *source_label(source_t source) {
    switch (source) {
      case source_t::encoder_d3d11:
        return "encoder D3D11";
      case source_t::dd_test_d3d11:
        return "Desktop Duplication D3D11";
      case source_t::virtual_display_enumerate:
        return "virtual display enumerate";
      case source_t::query_display_config:
        return "QueryDisplayConfig";
    }
    return "unknown";
  }

}  // namespace tdr
