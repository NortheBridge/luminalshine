/**
 * @file src/platform/windows/virtual_display_backend.cpp
 * @brief Picks the active virtual-display backend at startup.
 *
 * LuminalVGD (the first-party IddCx driver) is the only supported backend.
 * SudoVDA was retired in 2026-07 — the installer evicts it and the runtime
 * never selects it; the legacy `virtual_display_backend` config tokens
 * ("sudovda", "mtt") map forward to auto with a warning.
 */

#include "src/platform/windows/virtual_display_backend.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/virtual_display.h"
#include "src/platform/windows/virtual_display_vgd.h"

#include <atomic>
#include <mutex>

namespace VDISPLAY {

  namespace {
    std::mutex &selection_mutex() {
      static std::mutex m;
      return m;
    }

    std::atomic<BackendType> &selected_backend() {
      static std::atomic<BackendType> b {BackendType::NONE};
      return b;
    }

    // Distinguishes "select_backend() has never run" from "select_backend()
    // ran and chose NONE". Lets active_backend() skip taking the selection
    // mutex on every call after the first when the host genuinely has no
    // backend installed.
    std::atomic<bool> &selection_initialized() {
      static std::atomic<bool> b {false};
      return b;
    }

  }  // namespace

  BackendType select_backend() {
    std::lock_guard lk(selection_mutex());
    if (selection_initialized().load(std::memory_order_acquire)) {
      return selected_backend().load(std::memory_order_acquire);
    }

    const std::string preference = config::video.virtual_display_backend;
    if (preference == "mtt" || preference == "sudovda") {
      BOOST_LOG(warning) << "virtual_display_backend=" << preference
                         << " is no longer supported (LuminalVGD is the only backend); "
                            "falling back to auto selection.";
    }

    const bool luminalvgd_installed = vgd::driver_appears_installed();

    const BackendType chosen = luminalvgd_installed ? BackendType::LUMINALVGD : BackendType::NONE;

    if (chosen == BackendType::LUMINALVGD) {
      BOOST_LOG(info) << "Virtual-display backend: LuminalVGD (first-party IddCx driver).";
    } else {
      BOOST_LOG(warning) << "The LuminalVGD driver is not installed/reachable; per-client "
                            "virtual displays will not work. Install the LuminalVGD driver "
                            "(drivers\\luminalvgd\\install.ps1 or the installer) to enable them.";
    }

    selected_backend().store(chosen, std::memory_order_release);
    selection_initialized().store(true, std::memory_order_release);
    return chosen;
  }

  BackendType active_backend() {
    // Lazy-initialize on first query so callers that run before any explicit
    // select_backend() (e.g. should_auto_enable_virtual_display() during
    // startup probe, or the SudoVDA-specific recovery path) still see the
    // correct backend instead of a pre-selection NONE.
    if (selection_initialized().load(std::memory_order_acquire)) {
      return selected_backend().load(std::memory_order_acquire);
    }
    return select_backend();
  }

  std::string active_backend_name() {
    switch (active_backend()) {
      case BackendType::LUMINALVGD: return "luminalvgd";
      case BackendType::SUDOVDA:    return "sudovda";
      case BackendType::NONE:       return "none";
    }
    return "none";
  }

}  // namespace VDISPLAY
