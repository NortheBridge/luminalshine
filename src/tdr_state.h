/**
 * @file src/tdr_state.h
 * @brief Process-wide GPU-TDR / WDDM-stack failure state.
 *
 * Records when the streaming stack detects an escalated GPU TDR (Timeout
 * Detection and Recovery) event — typically the symptom is
 *   - D3D11CreateDevice retries exhausted (DXGI_ERROR_UNSUPPORTED), or
 *   - Windows refusing to enumerate a newly-added virtual display, or
 *   - QueryDisplayConfig returning ERROR_NOT_SUPPORTED repeatedly.
 *
 * Centralising the signal lets:
 *   - the active session unwind itself quickly (raise mail::shutdown)
 *   - the system tray surface a one-shot notification to the user
 *   - the Troubleshooting Web UI display the most recent event
 *
 * Thread-safe. The header is cross-platform; the implementation lives in
 * tdr_state.cpp and is compiled on every platform so the API can be called
 * unconditionally (call sites stay short).
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace tdr {

  enum class source_t : int {
    /// D3D11CreateDeviceWithRecovery exhausted retries from the encoder path.
    encoder_d3d11 = 1,
    /// D3D11CreateDeviceWithRecovery exhausted retries from the Desktop
    /// Duplication probe path.
    dd_test_d3d11 = 2,
    /// SudoVDA `AddVirtualDisplay` returned, but Windows never
    /// enumerated the new display surface within the timeout.
    virtual_display_enumerate = 3,
    /// QueryDisplayConfig returned ERROR_NOT_SUPPORTED for several
    /// consecutive calls — the WDDM display API itself is wedged.
    query_display_config = 4,
  };

  struct event_t {
    std::chrono::system_clock::time_point at;
    source_t source;
    long hresult;  ///< Native HRESULT or 0 if not applicable.
    std::string detail;  ///< Free-text context (call_site, attempt count, etc.).
  };

  /**
   * @brief Record a TDR-class event.
   *
   * Logs a single high-signal `Error:` line at most once per cooldown
   * window and fires a rate-limited system-tray notification. Always
   * safe to call from any thread or from a context where the tray
   * subsystem isn't initialised (the notification call is a no-op then).
   *
   * Callers that own a session should poll `recovery_recent()` to decide
   * whether to short-circuit out of the encode / capture loop or refuse a
   * new session — `mark_event` only records the signal, it does not raise
   * any per-session shutdown events itself (which would require the
   * session-local mail manager).
   *
   * @param source Which subsystem detected the failure.
   * @param hresult Native error code, or 0 if not applicable.
   * @param detail Short context string for the log line and the Web UI.
   */
  void mark_event(source_t source, long hresult, std::string detail);

  /**
   * @brief Whether a TDR event has been recorded within the recent past.
   * @param within Lookback window. Default 30s, which matches the empirical
   *               worst-case D3D11CreateDeviceWithRecovery + virtual
   *               display recreate window observed in the wild.
   */
  bool recovery_recent(std::chrono::seconds within = std::chrono::seconds {30});

  /**
   * @brief Most recent recorded event, for the /api/health/tdr endpoint
   *        and the Troubleshooting "GPU / Display Stack Health" card.
   */
  std::optional<event_t> last_event();

  /// Total recorded events since process start.
  std::uint64_t event_count();

  /// Human-readable label for `source_t` (used by logs and the Web UI).
  const char *source_label(source_t source);

}  // namespace tdr
