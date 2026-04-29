/**
 * @file src/platform/windows/virtual_display_mtt.h
 * @brief MTT VDD backend — the IDD-based default.
 *
 * The MTT driver doesn't expose per-call IOCTLs the way SudoVDA does. Its
 * model is:
 *   1. Driver reads `vdd_settings.xml` on init (path comes from
 *      `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH`).
 *   2. Driver listens on `\\.\pipe\MTTVirtualDisplayPipe`.
 *   3. To reconfigure (count of displays, modes, GPU, etc.), the host writes
 *      a new XML and sends `RELOAD_DRIVER` over the pipe.
 *   4. PING/PONG over the pipe is the liveness signal (replaces SudoVDA's
 *      `IOCTL_GET_WATCHDOG`).
 *
 * Multi-client model: MTT cannot create per-client GUID-tagged displays. When
 * multiple Moonlight clients connect, they share whichever display the first
 * connector configured. See `create_display()` for the fallback semantics.
 */
#pragma once

#include "src/platform/windows/virtual_display.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace VDISPLAY::mtt {

  /// One-time process initialization. Verifies the driver is installed,
  /// seeds the registry `VDDPATH` to LuminalShine's ProgramData directory,
  /// and writes a default settings XML if none exists. Returns the driver
  /// status equivalent to `openVDisplayDevice()`.
  DRIVER_STATUS initialize();

  /// Releases any handles held by the backend. Called on process shutdown
  /// or when the watchdog declares the driver dead.
  void shutdown();

  /// Returns true if the MTT VDD device class is enumerated by SetupAPI
  /// (regardless of whether it's currently responsive).
  bool is_driver_installed();

  /// PING/PONG over the named pipe with the supplied timeout. This is the
  /// MTT equivalent of SudoVDA's `IOCTL_GET_WATCHDOG`.
  bool is_responsive(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

  /// Sends a single text command over `\\.\pipe\MTTVirtualDisplayPipe` and
  /// optionally reads the reply. Used internally for RELOAD_DRIVER /
  /// SETDISPLAYCOUNT / SETGPU.
  bool send_pipe_command(const std::wstring &command,
                         std::wstring *reply = nullptr,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

  /// Tells the driver to re-read settings.xml and re-enumerate adapters.
  bool reload_driver();

  /// Selects the GPU MTT renders against, by friendly name. Triggers a
  /// driver reload internally.
  bool set_render_adapter(const std::wstring &adapter_friendly_name);

  /// Picks the GPU with the most dedicated VRAM (matches SudoVDA's behavior).
  bool set_render_adapter_with_most_vram();

  /// Starts the heartbeat thread. The callback fires when N consecutive
  /// PING attempts time out — same contract as
  /// `VDISPLAY::startPingThread(failCb)`.
  bool start_ping_thread(std::function<void()> on_failure);

  /// Stop the heartbeat thread.
  void stop_ping_thread();

  /// Pause/resume the heartbeat. SudoVDA exposes the same toggle so the
  /// host can suppress watchdog alerts during normal teardown.
  void set_watchdog_feeding_enabled(bool enable);

  /// Create (or join) a virtual display with the requested mode.
  ///
  /// MTT's model does not support per-client GUIDs. If a virtual display is
  /// already configured for an active session this call returns the existing
  /// display unmodified; the supplied dimensions/refresh are ignored and an
  /// info-level log is emitted explaining the fallback.
  std::optional<VirtualDisplayCreationResult> create_display(
    const char *client_uid,
    const char *client_name,
    const char *hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz = 0,
    bool framegen_refresh_active = false
  );

  /// Remove the display tracked under `guid`. If this was the only tracked
  /// display, the driver is reconfigured to count=0 and reloaded so the
  /// virtual monitor disappears from Windows enumeration.
  bool remove_display(const GUID &guid);

  /// Removes all tracked virtual displays.
  bool remove_all_displays();

  /// Returns true if `guid` corresponds to a display this backend currently
  /// tracks (matches `VDISPLAY::is_virtual_display_guid_tracked` semantics).
  bool is_guid_tracked(const GUID &guid);

  /// Resolve the Windows device id (`{xxxxxxxx-xxxx-...}`) of an MTT-created
  /// display by friendly client name.
  std::optional<std::string> resolve_device_id_for_client(const std::string &client_name);

  /// Return any active MTT-created display's device id.
  std::optional<std::string> resolve_any_device_id();

  /// Resolve by `\\.\DISPLAYn` device path (MTT side).
  std::optional<std::string> resolve_device_id(const std::wstring &display_name);

  /// Returns true when `output_identifier` matches a display owned by MTT.
  bool is_mtt_output(const std::string &output_identifier);

  /// Enumerate MTT-owned displays (used by main.cpp / nvhttp.cpp display
  /// listings). Mirrors `enumerateSudaVDADisplays` for the MTT path.
  std::vector<SudaVDADisplayInfo> enumerate_displays();

}  // namespace VDISPLAY::mtt
