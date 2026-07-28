/**
 * @file src/platform/windows/vgd_recovery.cpp
 * @brief LuminalVGD diagnostics + manual recovery (see vgd_recovery.h).
 */
// standard includes
#include <chrono>
#include <mutex>
#include <thread>

// platform includes
#include <winsock2.h>
#include <windows.h>

#include <cfgmgr32.h>

// local includes
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/virtual_display.h"
#include "src/platform/windows/virtual_display_backend.h"
#include "src/platform/windows/virtual_display_vgd.h"
#include "src/platform/windows/vgd_recovery.h"

// The device-node helpers live in virtual_display.cpp's anonymous
// namespace; these thin forwarders are declared alongside the SudoVDA
// ones (see VDISPLAY::recovery_bridge there).
namespace VDISPLAY::recovery_bridge {
  std::optional<std::wstring> luminal_vgd_instance_id();
  bool device_pnp_restart(const std::wstring &id);
  bool device_is_disabled(const std::wstring &id);
  std::string device_collect_hardware_ids(const std::wstring &id);
}  // namespace VDISPLAY::recovery_bridge

namespace platf::vgd_recovery {

  namespace {
    std::mutex g_mutex;
    long long g_last_recovery_at {0};
    recovery_level_t g_last_recovery_level {recovery_level_t::none};
    std::string g_last_recovery_message;

    /// Time for the device to settle after a PnP cycle before we decide
    /// whether the handshake came back.
    constexpr auto kPnpSettleDelay = std::chrono::milliseconds(1500);

    void record_last_recovery(recovery_level_t level, const std::string &message) {
      const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      std::lock_guard<std::mutex> lk(g_mutex);
      g_last_recovery_at = static_cast<long long>(now);
      g_last_recovery_level = level;
      g_last_recovery_message = message;
    }

    /// Close and reopen the control device, re-running the handshake.
    bool recycle_control_handle() {
      VDISPLAY::vgd::close_device();
      return VDISPLAY::vgd::driver_ready();
    }
  }  // namespace

  diagnostic_t collect_diagnostic() {
    diagnostic_t d {};

    {
      std::lock_guard<std::mutex> lk(g_mutex);
      d.last_recovery_at = g_last_recovery_at;
      d.last_recovery_level = g_last_recovery_level;
      d.last_recovery_message = g_last_recovery_message;
    }

    // Driver-side truth first: whether the control device answers is what
    // actually determines if virtual displays work. The PnP node can look
    // healthy while the control ACL refuses us (the 2026-07 outage), and
    // it can look odd while streaming works fine.
    d.driver_reachable = VDISPLAY::vgd::driver_appears_installed();
    if (d.driver_reachable) {
      d.handshake_ok = VDISPLAY::vgd::driver_ready();
      if (auto version = VDISPLAY::vgd::driver_version_string(); version) {
        d.driver_version = *version;
      }
    } else {
      d.last_error = GetLastError();
    }

    auto instance = VDISPLAY::recovery_bridge::luminal_vgd_instance_id();
    if (!instance) {
      d.status_string = d.handshake_ok
        ? "Driver responding, but no root\\luminal_vgd device node was found"
        : "Not present";
      return d;
    }

    d.device_present = true;
    d.instance_id = platf::to_utf8(*instance);
    d.hardware_ids = VDISPLAY::recovery_bridge::device_collect_hardware_ids(*instance);

    DEVINST dev_inst = 0;
    CONFIGRET cr = CM_Locate_DevNodeW(
      &dev_inst,
      const_cast<DEVINSTID_W>(instance->c_str()),
      CM_LOCATE_DEVNODE_NORMAL
    );
    if (cr != CR_SUCCESS) {
      cr = CM_Locate_DevNodeW(
        &dev_inst,
        const_cast<DEVINSTID_W>(instance->c_str()),
        CM_LOCATE_DEVNODE_PHANTOM
      );
    }
    if (cr == CR_SUCCESS) {
      ULONG status = 0, problem = 0;
      if (CM_Get_DevNode_Status(&status, &problem, dev_inst, 0) == CR_SUCCESS) {
        d.problem_code = problem;
        if (status & DN_HAS_PROBLEM) {
          if (problem == CM_PROB_DISABLED) {
            d.status_string = "Disabled (CM_PROB_DISABLED, code 22). "
                              "Use \"Restart Virtual Display Driver\" to re-enable.";
          } else {
            d.status_string = "Has problem (CM_PROB code " + std::to_string(problem) + ")";
          }
        } else if (status & DN_STARTED) {
          d.status_string = d.handshake_ok
            ? "Healthy (DN_STARTED, driver handshake OK)"
            : "Started, but the driver did not answer a handshake";
        } else {
          d.status_string = "Present but not started";
        }
      } else {
        d.status_string = "Present (CM_Get_DevNode_Status failed)";
      }
    } else {
      d.status_string = "Present (CM_Locate_DevNodeW failed, cr=" + std::to_string(cr) + ")";
    }

    return d;
  }

  recovery_result_t manual_restart(bool allow_pnp_cycle) {
    recovery_result_t result {};

    if (!VDISPLAY::is_luminalvgd_active()) {
      result.message = "LuminalVGD is not the active virtual display backend; "
                       "nothing to restart.";
      BOOST_LOG(warning) << "Virtual display restart requested, but " << result.message;
      record_last_recovery(recovery_level_t::none, result.message);
      return result;
    }

    if (auto instance = VDISPLAY::recovery_bridge::luminal_vgd_instance_id(); instance) {
      result.instance_id = platf::to_utf8(*instance);
    }

    // Level 1 — drop every monitor this process owns, then recycle the
    // control handle so the next create starts from a fresh handshake.
    // This is the level that clears the states we can actually cause:
    // leaked sessions, a stale device binding after a driver update.
    BOOST_LOG(info) << "LuminalVGD recovery: level 1 (destroy tracked sessions + "
                       "recycle control handle). Active streaming sessions using a "
                       "virtual display will end.";
    VDISPLAY::vgd::remove_all_virtual_displays();
    if (recycle_control_handle()) {
      result.success = true;
      result.level = recovery_level_t::session_reset;
      result.message = "Virtual display sessions were destroyed and the driver "
                       "connection was re-established.";
      if (auto version = VDISPLAY::vgd::driver_version_string(); version) {
        result.message += " Driver reports " + *version + ".";
      }
      BOOST_LOG(info) << "LuminalVGD recovery succeeded at level 1: " << result.message;
      record_last_recovery(result.level, result.message);
      return result;
    }

    BOOST_LOG(warning) << "LuminalVGD recovery: control handle did not come back after "
                          "a session reset.";

    if (!allow_pnp_cycle) {
      result.message = "Virtual display sessions were destroyed, but the driver did not "
                       "respond to a handshake afterwards.";
      record_last_recovery(recovery_level_t::session_reset, result.message);
      return result;
    }

    // Level 2 — PnP disable/enable of the device node. Heavier: it tears
    // down the device for every process, so it only runs once the lighter
    // path has demonstrably failed.
    auto instance = VDISPLAY::recovery_bridge::luminal_vgd_instance_id();
    if (!instance) {
      result.message = "The LuminalVGD device (root\\luminal_vgd) is not present in the "
                       "PnP tree. The driver may have been uninstalled — run "
                       "\"Reconfigure LuminalShine\" from the Start Menu to reinstall it.";
      BOOST_LOG(error) << "LuminalVGD recovery failed: " << result.message;
      record_last_recovery(recovery_level_t::session_reset, result.message);
      return result;
    }

    BOOST_LOG(info) << "LuminalVGD recovery: level 2 (PnP disable+enable) on instance "
                    << result.instance_id;
    const bool cycled = VDISPLAY::recovery_bridge::device_pnp_restart(*instance);
    std::this_thread::sleep_for(kPnpSettleDelay);
    result.level = recovery_level_t::pnp_restart;

    if (!cycled) {
      if (VDISPLAY::recovery_bridge::device_is_disabled(*instance)) {
        result.message = "The PnP restart left the LuminalVGD device disabled. "
                         "Re-enable it in Device Manager, or reboot the machine.";
      } else {
        result.message = "The PnP disable+enable cycle failed; the driver may be "
                         "unresponsive. A reboot may be required.";
      }
      BOOST_LOG(error) << "LuminalVGD recovery failed: " << result.message;
      record_last_recovery(result.level, result.message);
      return result;
    }

    if (recycle_control_handle()) {
      result.success = true;
      result.message = "The LuminalVGD device was restarted and the driver is "
                       "responding again.";
      BOOST_LOG(info) << "LuminalVGD recovery succeeded at level 2.";
    } else {
      result.message = "The device was restarted, but the driver still does not answer "
                       "a handshake. A reboot may be required.";
      BOOST_LOG(error) << "LuminalVGD recovery: " << result.message;
    }
    record_last_recovery(result.level, result.message);
    return result;
  }

}  // namespace platf::vgd_recovery
