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
#include "src/platform/windows/vgd_devnode.h"
#include "src/platform/windows/vgd_transition.h"
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

    // Selection returned NONE at least once this process — the host was in
    // WGC fallback. A later promotion to LUMINALVGD is then a genuine
    // WGC->VGD transition and logs the transition sentence (a first-ever
    // selection that lands on LuminalVGD is normal startup, not a
    // transition).
    std::atomic<bool> &was_in_wgc_fallback() {
      static std::atomic<bool> b {false};
      return b;
    }

  }  // namespace

  namespace {
    /// Selection body; caller holds selection_mutex(). Shared by
    /// select_backend() (first-query path) and reselect_if_none() (the
    /// forced WGC->VGD transition path, which clears the latch first).
    BackendType select_backend_locked();
  }  // namespace

  BackendType select_backend() {
    std::lock_guard lk(selection_mutex());
    if (selection_initialized().load(std::memory_order_acquire)) {
      return selected_backend().load(std::memory_order_acquire);
    }
    return select_backend_locked();
  }

  BackendType reselect_if_none() {
    std::lock_guard lk(selection_mutex());
    if (selection_initialized().load(std::memory_order_acquire) &&
        selected_backend().load(std::memory_order_acquire) == BackendType::LUMINALVGD) {
      return BackendType::LUMINALVGD;
    }
    // Clear a latched NONE so the selection body runs (and may latch)
    // again. Under the mutex this is invisible to concurrent queries:
    // they either see the old latch or the re-selection result.
    selection_initialized().store(false, std::memory_order_release);
    selected_backend().store(BackendType::NONE, std::memory_order_release);
    return select_backend_locked();
  }

  namespace {
    BackendType select_backend_locked() {
      const std::string preference = config::video.virtual_display_backend;
      if (preference == "mtt" || preference == "sudovda") {
        // Once per process: while selection stays unlatched (fresh install,
        // driver start in flight) this function re-runs on every backend
        // query, and the warning would repeat each time.
        static std::atomic<bool> warned_once {false};
        if (!warned_once.exchange(true)) {
          BOOST_LOG(warning) << "virtual_display_backend=" << preference
                             << " is no longer supported (LuminalVGD is the only backend); "
                                "falling back to auto selection.";
        }
      }

      // Service-owned devnode: adopt a present root\luminal_vgd device or
      // create the software device (fresh MSI installs stage the driver
      // package only), then heal a stale binding after an upgrade — the
      // no-reboot driver switchover. One-shot; failures degrade to the
      // driver simply appearing not installed. MUST run before the
      // reachability probe below or fresh installs always select NONE.
      // (The forced WGC->VGD transition re-attempts a failed ensure via
      // vgd_devnode::ensure_retry() before calling reselect_if_none().)
      platf::vgd_devnode::startup_ensure_and_heal();

      const bool luminalvgd_installed = vgd::driver_appears_installed();

      if (!luminalvgd_installed && platf::vgd_devnode::device_expected()) {
        // A devnode exists (adopted or just created) but the control
        // interface is not up yet — a fresh install's driver start can
        // outlive our waits. Do NOT latch NONE: leave selection
        // uninitialized so the next backend query re-probes (the probe is
        // a cheap open/close). Latching here permanently disabled virtual
        // displays whenever driver start lost the race (review finding).
        static std::atomic<bool> logged_once {false};
        if (!logged_once.exchange(true)) {
          BOOST_LOG(info) << "LuminalVGD device present/created but its control interface is "
                             "not up yet; backend selection will retry on the next query.";
        }
        // This branch is also the NORMAL fresh-install/slow-boot driver
        // start race — losing that race is not "living in WGC fallback".
        // Only count it as fallback once a WGC/DDA capture actually
        // opened this process, so the transition sentence never fires on
        // a plain startup that merely won the race late (review finding).
        const auto note = platf::vgd_transition::last_capture_note();
        if (note.sequence != 0 && note.kind != platf::vgd_transition::kCaptureKindVgdRing) {
          was_in_wgc_fallback().store(true, std::memory_order_release);
        }
        return BackendType::NONE;
      }

      const BackendType chosen = luminalvgd_installed ? BackendType::LUMINALVGD : BackendType::NONE;

      if (chosen == BackendType::LUMINALVGD) {
        BOOST_LOG(info) << "Virtual-display backend: LuminalVGD (first-party IddCx driver).";
        if (was_in_wgc_fallback().exchange(false)) {
          // The greppable transition marker (task #61): earlier selection
          // passes returned NONE (WGC fallback), and the driver is now
          // reachable — the host has moved onto the VGD backend without a
          // service restart, whichever path re-ran selection (transition
          // watcher, web UI button, or a lazy backend query).
          BOOST_LOG(info) << "LuminalShine has transitioned LuminalVGD into VGD Mode "
                             "(driver reachable; virtual displays available without a "
                             "service restart).";
        }
      } else {
        // Once per process: reselect_if_none() can re-run this body (the
        // watcher/kick path), and the multi-line install instructions
        // would otherwise repeat on every failed re-selection.
        static std::atomic<bool> warned_not_installed {false};
        if (!warned_not_installed.exchange(true)) {
          BOOST_LOG(warning) << "The LuminalVGD driver is not installed/reachable; per-client "
                                "virtual displays will not work. Install the LuminalVGD driver "
                                "(drivers\\luminalvgd\\install.ps1 or the installer) to enable them.";
        }
        was_in_wgc_fallback().store(true, std::memory_order_release);
      }

      selected_backend().store(chosen, std::memory_order_release);
      selection_initialized().store(true, std::memory_order_release);
      return chosen;
    }
  }  // namespace

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
