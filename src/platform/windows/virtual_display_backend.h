/**
 * @file src/platform/windows/virtual_display_backend.h
 * @brief Selects between the available virtual-display driver backends.
 *
 * LuminalShine supports two virtual-display drivers:
 *   - SudoVDA — current default backend, controlled via custom IOCTLs.
 *     Slated to be replaced by a first-party LuminalShine VDD in a future
 *     release; until then it ships as the recommended driver.
 *   - MTT VDD (MikeTheTech's Virtual Display Driver) — alternative, IDD-based,
 *     controlled via a named pipe + settings XML. Kept as a workaround for
 *     SudoVDA's WUDFHostProblem2 hang on the latest Windows 11 release builds
 *     and on Windows 11 Insider Preview channels.
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
    /// MTT VDD (alternative / Win11-Insider workaround).
    MTT = 1,
    /// SudoVDA (default backend).
    SUDOVDA = 2,
  };

  /// Select and initialize the backend for this process.
  ///
  /// Inspects `config::video.virtual_display_backend` (auto/mtt/sudovda) and
  /// picks accordingly. With `auto`, prefers SudoVDA, falls back to MTT VDD,
  /// and logs the SudoVDA Win11/Insider caveat when SudoVDA is selected.
  ///
  /// Idempotent: calling more than once returns the already-chosen backend.
  BackendType select_backend();

  /// Returns the active backend without re-running selection. Returns NONE
  /// before `select_backend()` has been called.
  BackendType active_backend();

  /// Convenience helpers — these never run selection on their own.
  inline bool is_mtt_active() {
    return active_backend() == BackendType::MTT;
  }

  inline bool is_sudovda_active() {
    return active_backend() == BackendType::SUDOVDA;
  }

  /// Friendly name of the active backend, for logs and the web UI.
  std::string active_backend_name();

}  // namespace VDISPLAY
