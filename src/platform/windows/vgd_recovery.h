/**
 * @file src/platform/windows/vgd_recovery.h
 * @brief Diagnostics and manual recovery for the LuminalVGD virtual
 *        display driver.
 *
 * Replaces the legacy SudoVDA equivalents (sudovda_recovery.h). Those
 * probed `root\sudomaker\sudovda` — a device this product no longer
 * installs and actively removes — so on every LuminalVGD host they
 * reported "Not present" while the real driver was handshaking fine in
 * the same process, and the recovery ladder they drove bailed on its
 * first step. During the 2026-07-27 display-stack failure that meant the
 * Troubleshooting page told the user their virtual display driver was
 * missing, and offered a "Restart Virtual Display Driver" button that
 * could not do anything.
 *
 * The recovery here works with what LuminalVGD actually exposes:
 * destroying the tracked monitor sessions, recycling the control-device
 * handle (which re-runs the handshake), and — only as a last resort —
 * PnP-cycling the `root\luminal_vgd` device node.
 */
#pragma once

#include <string>

namespace platf::vgd_recovery {

  enum class recovery_level_t : int {
    /// Nothing ran (driver absent, or already healthy on a probe-only call).
    none = 0,
    /// Tracked monitor sessions destroyed and the control handle recycled;
    /// the next create re-handshakes against a fresh device binding.
    session_reset = 1,
    /// The `root\luminal_vgd` device node was PnP-disabled and re-enabled.
    pnp_restart = 2,
  };

  struct recovery_result_t {
    bool success {false};
    recovery_level_t level {recovery_level_t::none};
    /// Human-readable summary, surfaced verbatim in the Web UI.
    std::string message;
    /// Device instance ID at the time of the call, when known.
    std::string instance_id;
  };

  struct diagnostic_t {
    /// The control device could be opened (driver installed and reachable).
    bool driver_reachable {false};
    /// A handshake succeeded and returned protocol/build info.
    bool handshake_ok {false};
    /// "proto <maj>.<min> build <n>" when the handshake succeeded.
    std::string driver_version;
    /// Win32 error from the last failing FFI call, when the driver could
    /// not be opened. 5 (access denied) is the signature of the control
    /// ACL refusing a non-SYSTEM caller.
    unsigned long last_error {0};

    /// The device node exists in the PnP tree.
    bool device_present {false};
    std::string instance_id;
    std::string hardware_ids;
    /// Human-readable device state ("Healthy (DN_STARTED)", "Disabled
    /// (CM_PROB_DISABLED, code 22)", "Not present", ...).
    std::string status_string;
    unsigned long problem_code {0};

    /// Last recovery this process ran, for support bundles. `at` is a
    /// Unix timestamp, 0 when nothing has run.
    long long last_recovery_at {0};
    recovery_level_t last_recovery_level {recovery_level_t::none};
    std::string last_recovery_message;
  };

  /// Snapshot of driver + device state for the Troubleshooting card and
  /// the support-diagnostic copy button. Never throws.
  diagnostic_t collect_diagnostic();

  /**
   * @brief User-initiated recovery ("Restart Virtual Display Driver").
   *
   * Destroys every tracked monitor session, closes and reopens the
   * control device (re-running the handshake), and falls back to a PnP
   * disable/enable cycle of `root\luminal_vgd` if the handshake still
   * fails. Any active streaming session using a virtual display ends.
   *
   * @param allow_pnp_cycle When false, stop after the session reset —
   *        used by automatic paths that must not yank the device out
   *        from under an unrelated caller.
   */
  recovery_result_t manual_restart(bool allow_pnp_cycle = true);

}  // namespace platf::vgd_recovery
