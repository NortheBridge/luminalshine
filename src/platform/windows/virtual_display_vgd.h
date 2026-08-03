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
#include <chrono>
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

  /// Number of currently tracked monitor sessions (0 when idle). Cheap;
  /// used by the devnode rebind worker to refuse yanking the device out
  /// from under a session that started concurrently.
  size_t tracked_session_count();

  /// "proto <maj>.<min> build <n>" from the driver handshake, for
  /// diagnostics/web UI.
  std::optional<std::string> driver_version_string();

  /// The raw `driver_build` from the handshake caps (the same <n> as in
  /// driver_version_string), for version-mismatch checks. Opens the
  /// control device when needed.
  std::optional<uint32_t> driver_build();

  /// What the ring-consuming capture backend needs to map a session's
  /// frame ring (see display_vgd.cpp).
  struct RingTargetInfo {
    uint64_t session_id;
    uint32_t ring_slots;
    uint32_t transport_flags;
  };

  /// Resolve the tracked session whose monitor backs `display_name`
  /// (e.g. "\\\\.\\DISPLAY274"). With a single tracked session (the
  /// common per-client case) that session is returned directly;
  /// otherwise the session whose recorded display name matches wins.
  std::optional<RingTargetInfo> ring_target_for_display(const std::string &display_name);

  struct RingTransitionToken {
    uint64_t session_id {0};
    uint32_t ring_slots {0};
    uint32_t generation {0};
  };

  /// Snapshot the sole tracked ring before a planned display modeset.
  std::optional<RingTransitionToken> begin_planned_modeset();

  /// Wait until the planned modeset has either produced and activated a newer
  /// ring generation, or the original generation has remained ACTIVE for the
  /// whole settle window (the apply was a no-op).
  bool wait_for_planned_modeset(
    const RingTransitionToken &before,
    std::chrono::milliseconds timeout
  );

  /// Promote the sole live LuminalVGD monitor from the client refresh to its
  /// create-time 2x mode.  This is intentionally one-way for a stream: the
  /// render-stack detector has a reliable positive edge but no reliable
  /// negative edge.  Stream teardown restores the client's base mode.
  bool promote_frame_generation_refresh();

}  // namespace VDISPLAY::vgd
