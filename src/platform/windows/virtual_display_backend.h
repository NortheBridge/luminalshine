/**
 * @file src/platform/windows/virtual_display_backend.h
 * @brief Selects the active virtual-display driver backend.
 *
 * LuminalShine ships a single virtual-display driver:
 *   - LuminalVGD — the first-party IddCx driver. SudoVDA was retired in
 *     2026-07 and is never selected; its enum value remains only until the
 *     legacy code excision completes.
 *
 * Public functions in `virtual_display.h` dispatch to the active backend
 * through this thin selector. The backend is LuminalVGD when the driver is
 * installed, NONE otherwise; once chosen it remains fixed for the process
 * lifetime.
 */
#pragma once

#include <string>

namespace VDISPLAY {

  enum class BackendType : int {
    /// No virtual-display driver available.
    NONE = 0,
    /// SudoVDA (retired 2026-07; never selected — kept until code excision).
    SUDOVDA = 1,
    /// LuminalVGD — the first-party IddCx driver (the only supported backend).
    LUMINALVGD = 2,
  };

  /// Select and initialize the backend for this process.
  ///
  /// Picks LuminalVGD when the driver is installed, NONE otherwise. Legacy
  /// `virtual_display_backend` tokens ("sudovda", "mtt") log a warning and
  /// map to auto.
  ///
  /// Idempotent: calling more than once returns the already-chosen backend.
  BackendType select_backend();

  /// Returns the active backend without re-running selection. Returns NONE
  /// before `select_backend()` has been called.
  BackendType active_backend();

  /// Convenience helper — never runs selection on its own.
  inline bool is_sudovda_active() {
    return active_backend() == BackendType::SUDOVDA;
  }

  inline bool is_luminalvgd_active() {
    return active_backend() == BackendType::LUMINALVGD;
  }

  /// Friendly name of the active backend, for logs and the web UI.
  std::string active_backend_name();

}  // namespace VDISPLAY
