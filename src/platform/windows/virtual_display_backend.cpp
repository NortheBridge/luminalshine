/**
 * @file src/platform/windows/virtual_display_backend.cpp
 * @brief Picks the active virtual-display backend at startup.
 *
 * Resolution order with `virtual_display_backend = auto`:
 *   1. SudoVDA if installed (default backend until the planned LuminalShine
 *      first-party VDD ships).
 *   2. MTT VDD fallback. Emits a warning when SudoVDA is the active choice
 *      because it can hang on the latest Windows 11 release builds and on
 *      Windows 11 Insider Preview channels (WUDFHostProblem2 / HostTimeout);
 *      the warning points users at MTT VDD as the workaround until the
 *      first-party driver ships.
 */

#include "src/platform/windows/virtual_display_backend.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/virtual_display.h"
#include "src/platform/windows/virtual_display_mtt.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

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

    bool sudovda_appears_installed() {
      // Mirror the existing detection logic in virtual_display.cpp's
      // `find_sudovda_device_instance_id()`. We don't link to that helper
      // directly to keep this file independent; a registry probe is enough
      // for "is the user-mode side present" and the existing SudoVDA code
      // will fail-open if the device isn't actually responsive.
      //
      // Bounded one-shot retry: on Windows 11 Insider Preview Canary
      // builds the UMDF host that publishes the SudoVDA registry tree
      // can lag the SunshineService start by a few hundred milliseconds
      // on cold boot. A single hit-and-miss probe loses that race and
      // we fall through to BackendType::NONE for the entire process
      // lifetime. Retry up to ~750 ms total before giving up — plenty
      // of headroom on Canary, still negligible on systems where the
      // key is already present (we exit on the first attempt).
      constexpr int max_attempts = 4;
      constexpr auto retry_backoff = std::chrono::milliseconds(250);
      for (int attempt = 0; attempt < max_attempts; ++attempt) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SudoMaker\\SudoVDA", 0,
                          KEY_READ, &key) == ERROR_SUCCESS) {
          RegCloseKey(key);
          if (attempt > 0) {
            BOOST_LOG(info) << "SudoVDA registry key appeared on attempt " << (attempt + 1)
                            << " (~" << (attempt * retry_backoff.count()) << " ms after first probe).";
          }
          return true;
        }
        if (attempt + 1 < max_attempts) {
          std::this_thread::sleep_for(retry_backoff);
        }
      }
      return false;
    }
  }  // namespace

  BackendType select_backend() {
    std::lock_guard lk(selection_mutex());
    if (selection_initialized().load(std::memory_order_acquire)) {
      return selected_backend().load(std::memory_order_acquire);
    }

    const std::string preference = config::video.virtual_display_backend;
    const bool mtt_installed = mtt::is_driver_installed();
    const bool sudovda_installed = sudovda_appears_installed();

    BackendType chosen = BackendType::NONE;
    if (preference == "mtt") {
      chosen = mtt_installed ? BackendType::MTT : BackendType::NONE;
      if (!mtt_installed) {
        BOOST_LOG(warning) << "virtual_display_backend=mtt requested but MTT VDD is not installed; "
                              "falling back to SudoVDA.";
        chosen = sudovda_installed ? BackendType::SUDOVDA : BackendType::NONE;
      }
    } else if (preference == "sudovda") {
      if (!sudovda_installed) {
        BOOST_LOG(warning) << "virtual_display_backend=sudovda requested but SudoVDA is not installed; "
                              "falling back to MTT VDD.";
        chosen = mtt_installed ? BackendType::MTT : BackendType::NONE;
      } else {
        chosen = BackendType::SUDOVDA;
      }
    } else {
      // "auto" (or unrecognized) — prefer SudoVDA, fall back to MTT VDD.
      if (sudovda_installed) {
        chosen = BackendType::SUDOVDA;
      } else if (mtt_installed) {
        chosen = BackendType::MTT;
      } else {
        chosen = BackendType::NONE;
      }
    }

    if (chosen == BackendType::SUDOVDA) {
      BOOST_LOG(info) << "Virtual-display backend: SudoVDA (default). Heads up: SudoVDA can hang "
                         "on the latest Windows 11 release builds and on Windows 11 Insider "
                         "Preview channels (WUDFHostProblem2 / HostTimeout). If the virtual "
                         "display fails to come up, run \"Reconfigure LuminalShine\" from the "
                         "Start Menu and switch to MTT Virtual Display Driver as a workaround. "
                         "A first-party LuminalShine VDD is planned for a future release.";
    } else if (chosen == BackendType::MTT) {
      BOOST_LOG(info) << "Virtual-display backend: MTT VDD (alternative).";
    } else {
      BOOST_LOG(warning) << "No virtual-display backend installed; per-client virtual displays will not work.";
    }

    selected_backend().store(chosen, std::memory_order_release);
    selection_initialized().store(true, std::memory_order_release);
    return chosen;
  }

  BackendType active_backend() {
    // Lazy-initialize on first query so callers that run before any explicit
    // select_backend() (e.g. should_auto_enable_virtual_display() during
    // startup probe, or the SudoVDA-specific recovery path) still see the
    // correct backend. Without this, active_backend() returned NONE at
    // startup, isSudaVDADriverInstalled() fell through to the SudoVDA path,
    // and we logged misleading "Suda VDA driver not installed" warnings even
    // when MTT VDD was actually installed and would be selected.
    if (selection_initialized().load(std::memory_order_acquire)) {
      return selected_backend().load(std::memory_order_acquire);
    }
    return select_backend();
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
