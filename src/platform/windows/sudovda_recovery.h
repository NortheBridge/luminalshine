/**
 * @file src/platform/windows/sudovda_recovery.h
 * @brief Public API for SudoVDA recovery operations.
 *
 * The streaming pipeline lives behind a UMDF / IddCx driver host
 * (WUDFHost.exe). When a GPU TDR fires, that host can be left in a
 * stuck state that user-mode handle recycling alone cannot break. This
 * header exposes the escalating recovery ladder we run automatically
 * after a TDR, plus a manual entry point for the "Restart Virtual
 * Display Driver" button in the Troubleshooting view and a diagnostic
 * snapshot for the "Show diagnostic" support button.
 *
 * Recovery ladder (escalation order):
 *
 *   Level 1 — handle_recycle (already in virtual_display.cpp)
 *     closeVDisplayDevice() + initVDisplayDriver(). Cheapest. Helps
 *     when only the user-mode driver-control IPC went out of sync.
 *
 *   Level 2 — pnp_restart
 *     SetupDi DICS_DISABLE + DICS_ENABLE on the SudoVDA root device
 *     keyed by hardware ID "root\\sudomaker\\sudovda". Forces a full
 *     IddCx unbind / rebind through the PnP manager. Equivalent to
 *     right-click Disable → Enable in Device Manager, run from the
 *     SYSTEM service.
 *
 *   (Level 3 — WDDM reset via Ctrl+Win+Shift+B — handled in a future
 *    PR; out of scope for this header.)
 *
 * Everything in here is scoped to the SudoVDA device by hardware ID;
 * MTT VDD and other UMDF drivers are never touched. The MTT/SudoVDA
 * coexistence design (mutually exclusive) is preserved.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace platf::sudovda {

  enum class recovery_level_t : int {
    /// No level fired (e.g. recovery skipped or device already healthy).
    none = 0,
    /// User-mode SudoVDA control handle was recycled.
    handle_recycle = 1,
    /// SudoVDA root device was PnP-disabled and re-enabled.
    pnp_restart = 2,
    /// Display helper synthesised Ctrl+Win+Shift+B to trigger a system-
    /// wide WDDM reset, then the SudoVDA handle was recycled again so
    /// the next AddVirtualDisplay opens a fresh kernel binding against
    /// the recovered display port. Last resort; requires an interactive
    /// user session and is rate-limited at 15 minutes per process.
    wddm_reset = 3,
  };

  struct recovery_result_t {
    /// True when the operation completed without an error. False on
    /// "device not present" or any underlying SetupDi / CM failure.
    bool success;
    /// Highest level the call actually executed.
    recovery_level_t level;
    /// Human-readable summary. Surfaced verbatim to the support
    /// diagnostic button and to the Troubleshooting card.
    std::string message;
    /// SudoVDA device instance ID at the time of the call, if known.
    /// Useful for support bundles.
    std::string instance_id;
  };

  /**
   * @brief Run the recovery ladder up to the requested level.
   *
   * Rate-limited internally: Level 2 is gated by a per-process cooldown
   * (5 minutes by default) so a permanently-broken driver can't be reset
   * in a tight loop. Lower levels have shorter cooldowns. Pass `force`
   * to bypass cooldowns — used by the manual Restart button.
   *
   * @param max_level Highest level to attempt. Pass `wddm_reset` for
   *                  the full ladder, `pnp_restart` to stop short of
   *                  the user-disruptive keystroke synthesis, or
   *                  `handle_recycle` for the cheap recycle only.
   * @param force When true, ignore the per-level cooldown gates. Use
   *              this for user-initiated recovery; auto-recovery from
   *              the post-TDR detection path should leave it false.
   */
  recovery_result_t run_recovery_ladder(
    recovery_level_t max_level = recovery_level_t::wddm_reset,
    bool force = false
  );

  /**
   * @brief Manual "Restart Virtual Display Driver" entry point.
   *
   * Always runs Level 2 (PnP disable/enable) and ignores cooldowns.
   * Wired to `POST /api/state/vdd-restart`.
   */
  recovery_result_t manual_restart();

  /**
   * @brief Snapshot of the SudoVDA device's PnP / driver state.
   * Wired to `GET /api/state/vdd-diagnostic` so the user can paste the
   * full status string into a support ticket without ever opening
   * Device Manager.
   */
  struct diagnostic_t {
    /// True when SetupDi finds a device matching the SudoVDA HWID.
    bool device_present;
    /// PnP instance ID (e.g. "ROOT\\SUDOMAKER\\SUDOVDA\\0000"). Empty
    /// when device_present is false.
    std::string instance_id;
    /// Comma-separated hardware IDs reported by the device.
    std::string hardware_ids;
    /// Human-readable status: "Healthy", "Disabled", "Has problem (code N)",
    /// "Not present", etc.
    std::string status_string;
    /// Raw CM problem code (e.g. CM_PROB_DISABLED = 22). 0 means no
    /// problem.
    std::uint32_t problem_code;
    /// Timestamp of last recovery_ladder execution, if any.
    std::optional<std::chrono::system_clock::time_point> last_recovery_at;
    /// Level of the last recovery attempt.
    recovery_level_t last_recovery_level;
    /// Result message from the last recovery attempt.
    std::string last_recovery_message;
  };

  diagnostic_t collect_diagnostic();

}  // namespace platf::sudovda
