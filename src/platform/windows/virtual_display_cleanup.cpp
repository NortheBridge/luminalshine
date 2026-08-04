#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <chrono>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <string>
  #include <thread>

namespace platf::virtual_display_cleanup {
  namespace {
    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    std::size_t active_virtual_display_count() {
      const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      return static_cast<std::size_t>(std::count_if(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      ));
    }

    bool wait_for_virtual_display_teardown(std::chrono::steady_clock::duration timeout) {
      constexpr auto kPollInterval = std::chrono::milliseconds(100);

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (true) {
        const auto remaining = active_virtual_display_count();
        if (remaining == 0) {
          return true;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
          BOOST_LOG(warning) << "Virtual display cleanup: teardown wait expired with "
                             << remaining << " virtual display(s) still enumerated.";
          return false;
        }

        std::this_thread::sleep_for(kPollInterval);
      }
    }

    bool wait_for_physical_display(std::chrono::steady_clock::duration timeout) {
      constexpr auto kPollInterval = std::chrono::milliseconds(100);
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      do {
        if (VDISPLAY::has_active_physical_display()) {
          return true;
        }
        std::this_thread::sleep_for(kPollInterval);
      } while (std::chrono::steady_clock::now() < deadline);
      return VDISPLAY::has_active_physical_display();
    }

    bool restore_windows_display_database() {
      try {
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto win_dd = std::make_shared<display_device::WinDisplayDevice>(api);
        auto impersonating_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(win_dd);
        return impersonating_dd->restoreMonitorSettings();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw exception: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw unknown exception.";
      }
      return false;
    }
  }  // namespace

  cleanup_result_t run(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing
  ) {
    cleanup_result_t result;

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false")
                    << ", revert_order="
                    << (revert_order == revert_order_t::restore_before_remove ? "restore_before_remove" : "remove_before_restore")
                    << ")";

    const bool had_active_virtual_display = has_active_virtual_display();
    VDISPLAY::setWatchdogFeedingEnabled(false);

    // Restoration is intentionally completed while the VGD output still
    // exists. Removing the only active path first can destroy the helper's
    // desktop and its IPC pipe before it has a chance to restore Windows.
    if (enforce_db_restore) {
      if (revert_order == revert_order_t::remove_before_restore) {
        BOOST_LOG(warning) << "Virtual display cleanup: overriding legacy remove-before-restore ordering for safety.";
      }

      result.helper_revert_dispatched = display_helper_integration::revert(prefer_golden_if_current_missing);
      if (result.helper_revert_dispatched) {
        // A retained OLED recovery anchor is already an active physical
        // display, so presence is not proof of restoration. The helper exits
        // only after strict snapshot + layout verification; wait for that
        // transaction boundary before removing LuminalVGD.
        const bool restore_completed =
          display_helper_integration::wait_for_revert_completion(std::chrono::seconds(30));
        result.physical_display_verified = restore_completed &&
                                           wait_for_physical_display(std::chrono::seconds(2));
      }

      if (!result.physical_display_verified) {
        BOOST_LOG(warning) << "Virtual display cleanup: helper restore was not verified; restarting the helper and retrying.";
        result.helper_restarted = display_helper_integration::restart_helper_for_recovery();
        if (result.helper_restarted) {
          result.helper_revert_dispatched =
            display_helper_integration::revert(prefer_golden_if_current_missing) || result.helper_revert_dispatched;
          const bool restore_completed =
            display_helper_integration::wait_for_revert_completion(std::chrono::seconds(30));
          result.physical_display_verified = restore_completed &&
                                             wait_for_physical_display(std::chrono::seconds(2));
        }
      }

      if (!result.physical_display_verified) {
        result.database_restore_applied = restore_windows_display_database();
        if (result.database_restore_applied) {
          result.physical_display_verified = wait_for_physical_display(std::chrono::seconds(5));
        }
      }

      if (!result.physical_display_verified) {
        result.physical_fallback_applied = display_helper_integration::ensure_physical_display_active();
        if (result.physical_fallback_applied) {
          result.physical_display_verified = wait_for_physical_display(std::chrono::seconds(5));
        }
      }
    } else {
      result.physical_display_verified = VDISPLAY::has_active_physical_display();
    }

    if (!enforce_db_restore || result.physical_display_verified || !had_active_virtual_display) {
      result.virtual_displays_removed = VDISPLAY::removeAllVirtualDisplays();
      if (result.virtual_displays_removed) {
        (void) wait_for_virtual_display_teardown(std::chrono::seconds(5));
      }
    } else {
      BOOST_LOG(error) << "Virtual display cleanup: retaining VGD because no physical display could be verified; refusing to leave Windows headless.";
    }

    BOOST_LOG(info) << "Virtual display cleanup: finished (reason=" << reason_text
                    << ", had_active_virtual_display=" << (had_active_virtual_display ? "true" : "false")
                    << ", virtual_displays_removed=" << (result.virtual_displays_removed ? "true" : "false")
                    << ", helper_revert_dispatched=" << (result.helper_revert_dispatched ? "true" : "false")
                    << ", database_restore_applied=" << (result.database_restore_applied ? "true" : "false")
                    << ", physical_display_verified=" << (result.physical_display_verified ? "true" : "false")
                    << ", helper_restarted=" << (result.helper_restarted ? "true" : "false")
                    << ", physical_fallback_applied=" << (result.physical_fallback_applied ? "true" : "false")
                    << ")";
    return result;
  }
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
