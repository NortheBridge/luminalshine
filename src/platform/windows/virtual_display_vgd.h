/**
 * @file src/platform/windows/virtual_display_vgd.h
 * @brief LuminalVGD backend: per-client virtual monitors via the
 *        luminal-vgd-ffi C ABI (vendored at src/drivers/luminal-display).
 *
 * The public VDISPLAY entry points in virtual_display.cpp dispatch here
 * when virtual_display_backend selects BackendType::LUMINALVGD. Monitor
 * identity follows the driver's retention model: display identity is
 * derived from the client UID (a returning client reclaims its connector
 * and remembered Windows display settings), while each stream gets a
 * fresh session lease fed by the ping thread.
 */
#pragma once

#include <functional>
#include <optional>
#include <string>

#include <winsock2.h>
#include <windows.h>

#include "src/platform/windows/virtual_display.h"

namespace VDISPLAY::vgd {

  /// Cheap installed/reachable probe (open + close). Used by backend
  /// selection; does not keep the device open.
  bool driver_appears_installed();

  DRIVER_STATUS open_device();
  void close_device();
  bool driver_ready();

  bool start_ping_thread(std::function<void()> failCb);
  void set_watchdog_feeding(bool enable);

  std::optional<VirtualDisplayCreationResult> create_virtual_display(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps_millihz,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    bool enable_hdr = false
  );

  /// True when the connected driver advertises HDR10 (caps gate for
  /// requesting HDR monitors and for skipping the SDR topology downgrade).
  bool driver_supports_hdr();

  bool remove_virtual_display(const GUID &guid);
  bool remove_all_virtual_displays();
  bool is_guid_tracked(const GUID &guid);

  /// "proto <maj>.<min> build <n>" from the driver handshake, for
  /// diagnostics/web UI.
  std::optional<std::string> driver_version_string();

  /// What the ring-consuming capture backend needs to map a session's
  /// frame ring (see display_vgd.cpp).
  struct RingTargetInfo {
    uint64_t session_id;
    uint32_t ring_slots;
  };

  /// Resolve the tracked session whose monitor backs `display_name`
  /// (e.g. "\\\\.\\DISPLAY274"). With a single tracked session (the
  /// common per-client case) that session is returned directly;
  /// otherwise the session whose recorded display name matches wins.
  std::optional<RingTargetInfo> ring_target_for_display(const std::string &display_name);

}  // namespace VDISPLAY::vgd
