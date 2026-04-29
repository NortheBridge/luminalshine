/**
 * @file src/platform/windows/virtual_display_backend.cpp
 * @brief Picks the active virtual-display backend at startup.
 *
 * Resolution order with `virtual_display_backend = auto`:
 *   1. MTT VDD if installed (preferred for current/Insider Windows builds).
 *   2. SudoVDA fallback. Emits a warning when SudoVDA is the only option,
 *      because SudoVDA is in maintenance mode and is the documented cause
 *      of the WUDFHostProblem2 hangs on recent Insider builds.
 */

#include "src/platform/windows/virtual_display_backend.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/virtual_display.h"
#include "src/platform/windows/virtual_display_mtt.h"

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

    bool sudovda_appears_installed() {
      // Mirror the existing detection logic in virtual_display.cpp's
      // `find_sudovda_device_instance_id()`. We don't link to that helper
      // directly to keep this file independent; a registry probe is enough
      // for "is the user-mode side present" and the existing SudoVDA code
      // will fail-open if the device isn't actually responsive.
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SudoMaker\\SudoVDA", 0,
                        KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
      }
      return false;
    }
  }  // namespace

  BackendType select_backend() {
    std::lock_guard lk(selection_mutex());
    auto current = selected_backend().load(std::memory_order_acquire);
    if (current != BackendType::NONE) {
      return current;
    }

    const std::string preference = config::video.virtual_display_backend;
    const bool mtt_installed = mtt::is_driver_installed();
    const bool sudovda_installed = sudovda_appears_installed();

    BackendType chosen = BackendType::NONE;
    if (preference == "mtt") {
      if (!mtt_installed) {
        BOOST_LOG(warning) << "virtual_display_backend=mtt requested but MTT VDD is not installed; "
                              "falling back to SudoVDA.";
        chosen = sudovda_installed ? BackendType::SUDOVDA : BackendType::NONE;
      } else {
        chosen = BackendType::MTT;
      }
    } else if (preference == "sudovda") {
      chosen = sudovda_installed ? BackendType::SUDOVDA : BackendType::NONE;
      if (!sudovda_installed) {
        BOOST_LOG(warning) << "virtual_display_backend=sudovda requested but SudoVDA is not installed.";
      }
    } else {
      // "auto" (or unrecognized) — prefer MTT.
      if (mtt_installed) {
        chosen = BackendType::MTT;
      } else if (sudovda_installed) {
        chosen = BackendType::SUDOVDA;
      } else {
        chosen = BackendType::NONE;
      }
    }

    if (chosen == BackendType::SUDOVDA && !mtt_installed) {
      BOOST_LOG(warning) << "Using SudoVDA as the virtual-display backend. SudoVDA is in "
                            "maintenance mode and is known to fail on Windows Insider builds "
                            "(WUDFHostProblem2 / HostTimeout). Run the LuminalShine installer "
                            "and choose 'Modify' to install MTT Virtual Display Driver as the "
                            "primary backend.";
    } else if (chosen == BackendType::MTT) {
      BOOST_LOG(info) << "Virtual-display backend: MTT VDD.";
    } else if (chosen == BackendType::SUDOVDA) {
      BOOST_LOG(info) << "Virtual-display backend: SudoVDA (user override).";
    } else {
      BOOST_LOG(warning) << "No virtual-display backend installed; per-client virtual displays will not work.";
    }

    selected_backend().store(chosen, std::memory_order_release);
    return chosen;
  }

  BackendType active_backend() {
    return selected_backend().load(std::memory_order_acquire);
  }

  std::string active_backend_name() {
    switch (active_backend()) {
      case BackendType::MTT:     return "mtt";
      case BackendType::SUDOVDA: return "sudovda";
      case BackendType::NONE:    return "none";
    }
    return "none";
  }

}  // namespace VDISPLAY
