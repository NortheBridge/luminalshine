/**
 * @file src/platform/windows/virtual_display_backend.h
 * @brief Selects the active virtual-display driver backend.
 *
 * LuminalShine currently ships a single virtual-display driver:
 *   - SudoVDA — default backend, controlled via custom IOCTLs. Slated to be
 *     replaced by a first-party LuminalShine VDD in a future release; until
 *     then it ships as the recommended driver.
 *
 * Public functions in `virtual_display.h` dispatch to the active backend
 * through this thin selector. The backend is chosen at startup based on the
 * `virtual_display_backend` config option and which drivers are actually
 * installed; once chosen it remains fixed for the process lifetime.
 */
#pragma once

#include <string>

namespace VDISPLAY {

  enum class BackendType : int {
    /// No virtual-display driver available.
    NONE = 0,
    /// SudoVDA (legacy default; being replaced by LuminalVGD).
    SUDOVDA = 1,
    /// LuminalVGD — the first-party IddCx driver (preferred when installed).
    LUMINALVGD = 2,
  };

  /// Select and initialize the backend for this process.
  ///
  /// Inspects `config::video.virtual_display_backend` (auto/sudovda) and picks
  /// accordingly, logging the SudoVDA Win11/Insider caveat when SudoVDA is
  /// selected.
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
