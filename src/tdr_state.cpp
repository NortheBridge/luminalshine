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

    std::optional<incident_t> g_incident;
    std::chrono::steady_clock::time_point g_incident_last_steady {};
    std::uint64_t g_incident_count {0};
    std::atomic<bool> g_stack_down {false};

    // How long an incident stays open. A dead stack is re-detected on
    // every client attempt (~33s apart in the 2026-07-27 field incident),
    // and those detections are the same failure, not new ones. Comfortably
    // longer than that cadence so a persistent wedge reads as one incident
    // with a growing duration.
    constexpr std::chrono::minutes kIncidentWindow {5};

    // Cooldowns. Each TDR-class failure cascades through several call
    // sites (encoder D3D11, DD test, virtual display enumerate, ...), all
    // firing within seconds of each other. The first event in a cluster
    // emits the high-signal log line and the tray toast; the rest record
    // into `g_last` and increment the counter but stay silent. After the
    // cooldown elapses with no new events, the next event is treated as
    // a fresh incident.
    constexpr std::chrono::seconds kLogCooldown {30};
    constexpr std::chrono::minutes kNotifyCooldown {2};

    // Observer installed via set_event_hook(). Guarded by g_mutex; copied
    // out and invoked only after the lock is released (see mark_event).
    std::function<void(source_t)> g_event_hook;

    // Platform probe installed via set_stack_recheck(). Guarded by
    // g_mutex; copied out and invoked lock-free (see stack_down).
    std::function<bool()> g_stack_recheck;
    std::atomic<std::int64_t> g_last_recheck_ms {0};
    constexpr std::chrono::seconds kStackRecheckEvery {5};
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
    std::function<void(source_t)> hook_copy;
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      hook_copy = g_event_hook;
      g_last = event;
      g_count.fetch_add(1, std::memory_order_relaxed);

      // Fold into the open incident, or open a new one after a quiet
      // window. `terminal` is sticky for the life of the incident.
      const bool fold = g_incident.has_value()
        && g_incident_last_steady != clock::time_point {}
        && (now_steady - g_incident_last_steady) < kIncidentWindow;
      if (!fold) {
        incident_t fresh;
        fresh.started_at = now_system;
        fresh.first_source = source;
        g_incident = std::move(fresh);
        ++g_incident_count;
      }
      g_incident->last_at = now_system;
      g_incident->last_source = source;
      g_incident->hresult = hresult;
      g_incident->detail = detail;
      g_incident->events += 1;
      if (source == source_t::display_stack_down) {
        g_incident->terminal = true;
      }
      g_incident_last_steady = now_steady;

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

    // Invoked strictly after g_mutex is released so a hook may call back
    // into tdr:: accessors. The hook contract (tdr_state.h) requires it to
    // be non-blocking — we are on the caller's thread, which can be a
    // time-critical path such as the NvEnc encode loop.
    if (hook_copy) {
      try {
        hook_copy(source);
      } catch (...) {
        // A misbehaving observer must never break event recording.
      }
    }
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

  void mark_stack_down(long hresult, std::string detail) {
    const bool first = !g_stack_down.exchange(true, std::memory_order_release);
    if (first) {
      // Loud, once, with the actual remedy — not the generic TDR line.
      BOOST_LOG(error) << "Display stack down: " << detail
                       << " (hresult=0x" << std::hex << hresult << std::dec
                       << "). Windows' display API is unavailable process-wide, "
                       << "which no user-mode recovery can clear — the machine "
                       << "must be rebooted. New sessions will be refused until "
                       << "the display stack returns.";
    }
    // Still recorded as an event so the incident (and the ring/telemetry
    // consumers watching event_count) see it; mark_event handles the
    // rate-limited logging and the tray toast.
    mark_event(source_t::display_stack_down, hresult, std::move(detail));
  }

  bool stack_down() {
    if (!g_stack_down.load(std::memory_order_acquire)) {
      return false;
    }
    // A latched process must be able to DISCOVER recovery on its own: the
    // gates guarded by this function refuse sessions and encoder probes,
    // so the D3D11 success path that calls note_stack_healthy() can never
    // run while latched (2026-07-28 review finding — without this, a
    // false latch is a permanent healthy-machine denial of service).
    // Re-verify through the platform probe at most once per
    // kStackRecheckEvery and self-clear on a healthy read. Unlatched
    // callers still pay only the single atomic load above.
    std::function<bool()> probe;
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      probe = g_stack_recheck;
    }
    if (!probe) {
      return true;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    auto last = g_last_recheck_ms.load(std::memory_order_relaxed);
    const auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kStackRecheckEvery).count();
    if (now_ms - last < interval_ms) {
      return true;
    }
    if (!g_last_recheck_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) {
      // Another thread won the slot and is re-checking right now.
      return true;
    }
    bool healthy = false;
    try {
      healthy = probe();
    } catch (...) {
      // A failing probe is an unhealthy read; stay latched.
    }
    if (healthy) {
      note_stack_healthy();
      return false;
    }
    return true;
  }

  void note_stack_healthy() {
    if (!g_stack_down.exchange(false, std::memory_order_release)) {
      return;  // nothing was latched — cheap no-op on the hot success path
    }
    BOOST_LOG(info) << "Display stack recovered: Windows is enumerating displays "
                    << "and D3D11 device creation succeeds again. Resuming normal "
                    << "session handling.";
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_incident) {
      // Close the incident so a later failure reads as a new one.
      g_incident_last_steady = std::chrono::steady_clock::time_point {};
    }
  }

  std::optional<incident_t> current_incident() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_incident;
  }

  std::uint64_t incident_count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_incident_count;
  }

  void set_event_hook(std::function<void(source_t)> hook) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_event_hook = std::move(hook);
  }

  void set_stack_recheck(std::function<bool()> probe) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_stack_recheck = std::move(probe);
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
      case source_t::encoder_stall:
        return "encoder stall";
      case source_t::display_stack_down:
        return "display stack down";
    }
    return "unknown";
  }

}  // namespace tdr
