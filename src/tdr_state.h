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
#include <functional>
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
    /// The encoder's in-flight frame never signaled completion (NvEnc
    /// encode-wait timeout) — the earliest observable symptom of a GPU
    /// hang, minutes before the D3D11 retry ladder can exhaust.
    encoder_stall = 5,
    /// The WDDM display stack is dead machine-wide: D3D11 device creation
    /// fails AND QueryDisplayConfig reports the display API unavailable /
    /// zero paths. Recovery is not possible from user mode — only a
    /// reboot clears it. Terminal; see `stack_down()`.
    display_stack_down = 6,
  };

  struct event_t {
    std::chrono::system_clock::time_point at;
    source_t source;
    long hresult;  ///< Native HRESULT or 0 if not applicable.
    std::string detail;  ///< Free-text context (call_site, attempt count, etc.).
  };

  /**
   * @brief One failure incident: a cluster of events, not a raw count.
   *
   * A single GPU hang cascades through several detectors and, on a stack
   * that never recovers, is re-detected on every client attempt. Counting
   * detections told the user "39 TDR events" for what was one failure.
   * Events within the incident window fold into the incident that is
   * already open; the first event after a quiet window opens a new one.
   */
  struct incident_t {
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point last_at;
    std::uint64_t events {0};  ///< Detections folded into this incident.
    source_t first_source;  ///< What detected it first (closest to the cause).
    source_t last_source;
    long hresult {0};
    std::string detail;
    /// The display stack was confirmed dead (D3D11 + QueryDisplayConfig
    /// both failing). User-visible remedy is a reboot.
    bool terminal {false};
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
  ///
  /// This is a raw detection count and is deliberately NOT what the Web UI
  /// shows — it exists so in-process consumers can watch for "something
  /// changed" edges (the LuminalVGD ring drops its shared textures when
  /// this advances, and the per-session `gpu_resets` telemetry series is
  /// its delta). Use `incident_count()` / `current_incident()` for
  /// anything a human reads.
  std::uint64_t event_count();

  /**
   * @brief Record a confirmed machine-wide display-stack failure.
   *
   * Called when a D3D11 device creation failure is corroborated by
   * QueryDisplayConfig reporting the display API unavailable or zero
   * paths — i.e. the failure is below every user-mode component and no
   * amount of retrying will fix it. Latches `stack_down()` until
   * `note_stack_healthy()` observes the stack working again, so the
   * retry ladders can stop instead of re-detecting the same corpse every
   * ~33 seconds.
   */
  void mark_stack_down(long hresult, std::string detail);

  /**
   * @brief Whether the display stack is currently known to be dead.
   *
   * Latched by `mark_stack_down`, cleared by `note_stack_healthy`. Call
   * sites use this to fail fast and loudly (with "reboot required")
   * instead of burning the backoff ladder per encoder, per attempt.
   */
  bool stack_down();

  /**
   * @brief Report that the display stack was just observed working.
   *
   * Clears the terminal latch (a successful D3D11 device creation or a
   * healthy QueryDisplayConfig proves the stack came back — e.g. the GPU
   * driver recovered on its own, or the user reset it). Cheap and safe to
   * call on every success path; it is a no-op when nothing is latched.
   */
  void note_stack_healthy();

  /// The incident currently open (or the last one, if it has closed).
  std::optional<incident_t> current_incident();

  /// Number of distinct incidents since process start.
  std::uint64_t incident_count();

  /// Human-readable label for `source_t` (used by logs and the Web UI).
  const char *source_label(source_t source);

  /**
   * @brief Install a process-wide observer invoked after every `mark_event`.
   *
   * The hook runs on the thread that called `mark_event`, after the
   * internal lock has been released (it is never invoked under the lock,
   * so hook implementations may call back into `tdr::` accessors freely).
   *
   * IMPORTANT: the hook MUST NOT block. `mark_event` is called from
   * time-critical paths — e.g. the NvEnc encode loop detecting an encoder
   * stall — so the hook should do the cheapest possible dispatch (a
   * rate-limit check, a task-pool push) and post the actual work to a
   * worker thread. Exceptions thrown by the hook are swallowed.
   *
   * Replaces any previously-installed hook; pass `nullptr` to remove.
   * Intended for a single consumer wired up at process init (currently the
   * TDR topology lifeboat, src/platform/windows/tdr_lifeboat.h).
   */
  void set_event_hook(std::function<void(source_t)> hook);

}  // namespace tdr
