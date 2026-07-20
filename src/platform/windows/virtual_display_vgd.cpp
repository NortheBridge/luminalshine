/**
 * @file src/platform/windows/virtual_display_vgd.cpp
 * @brief LuminalVGD backend implementation over the luminal-vgd-ffi C ABI.
 */

#include "src/platform/windows/virtual_display_vgd.h"

#include "src/logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <luminal_vgd.h>

namespace VDISPLAY::vgd {

  namespace {

    struct TrackedSession {
      uint64_t session_id;
      uint64_t display_id;
      std::string client_name;
      uint32_t ring_slots;
      /// GDI display name ("\\\\.\\DISPLAY274") once the monitor surfaced;
      /// empty while the display is still inactive pre-APPLY.
      std::wstring display_name;
    };

    struct GuidKey {
      uint8_t bytes[16];

      bool operator<(const GuidKey &o) const {
        return std::memcmp(bytes, o.bytes, sizeof(bytes)) < 0;
      }
    };

    GuidKey key_of(const GUID &guid) {
      GuidKey k {};
      std::memcpy(k.bytes, &guid, sizeof(k.bytes));
      return k;
    }

    std::mutex g_mutex;
    VgdDeviceHandle *g_device = nullptr;
    std::optional<VgdCaps> g_caps;
    std::map<GuidKey, TrackedSession> g_sessions;
    std::thread g_ping_thread;
    std::atomic<bool> g_ping_stop {false};
    std::atomic<bool> g_ping_feeding {true};
    std::function<void()> g_ping_fail_cb;

    /// FNV-1a 64 over a byte range — stable display identities from
    /// client UIDs (a returning client reclaims connector + settings).
    uint64_t fnv1a64(const void *data, size_t len) {
      const auto *p = static_cast<const uint8_t *>(data);
      uint64_t h = 1469598103934665603ULL;
      for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
      }
      return h;
    }

    /// The driver refuses the reserved identity ranges (permanent
    /// 0x7000…, ephemeral 0xE000…); pin the top nibble clear of both and
    /// keep identities nonzero.
    uint64_t display_id_for_client(const char *client_uid) {
      const uint64_t h = client_uid ? fnv1a64(client_uid, std::strlen(client_uid)) : 0;
      uint64_t id = (h & 0x0FFF'FFFF'FFFF'FFFFULL) | 0x4000'0000'0000'0000ULL;
      return id;
    }

    uint64_t fresh_session_id(const GUID &guid) {
      LARGE_INTEGER qpc {};
      QueryPerformanceCounter(&qpc);
      uint64_t h = fnv1a64(&guid, sizeof(guid));
      h ^= static_cast<uint64_t>(qpc.QuadPart) * 0x9E3779B97F4A7C15ULL;
      return h ? h : 1;
    }

    DRIVER_STATUS open_locked() {
      if (g_device) {
        return DRIVER_STATUS::OK;
      }
      g_device = vgd_device_open();
      if (!g_device) {
        return DRIVER_STATUS::FAILED;
      }
      VgdCaps caps {};
      if (vgd_handshake(g_device, &caps) != 0) {
        vgd_device_close(g_device);
        g_device = nullptr;
        return DRIVER_STATUS::FAILED;
      }
      g_caps = caps;
      BOOST_LOG(info) << "LuminalVGD driver ready: proto " << caps.proto_major << '.'
                      << caps.proto_minor << " build " << caps.driver_build
                      << " caps 0x" << std::hex << caps.caps << std::dec
                      << " watchdog " << caps.watchdog_secs << " s";
      return DRIVER_STATUS::OK;
    }

    /// Enumerate active display names whose adapter is our driver.
    std::vector<std::wstring> luminal_display_names() {
      std::vector<std::wstring> out;
      DISPLAY_DEVICEW adapter {};
      adapter.cb = sizeof(adapter);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            std::wcsstr(adapter.DeviceString, L"Luminal Video Graphics Display")) {
          out.emplace_back(adapter.DeviceName);
        }
      }
      return out;
    }

    std::optional<std::wstring> monitor_device_path_of(const std::wstring &display_name) {
      DISPLAY_DEVICEW mon {};
      mon.cb = sizeof(mon);
      if (EnumDisplayDevicesW(display_name.c_str(), 0, &mon, EDD_GET_DEVICE_INTERFACE_NAME)) {
        return std::wstring {mon.DeviceID};
      }
      return std::nullopt;
    }

  }  // namespace

  bool driver_appears_installed() {
    std::lock_guard lk(g_mutex);
    if (g_device) {
      return true;
    }
    VgdDeviceHandle *probe = vgd_device_open();
    if (!probe) {
      return false;
    }
    vgd_device_close(probe);
    return true;
  }

  DRIVER_STATUS open_device() {
    std::lock_guard lk(g_mutex);
    return open_locked();
  }

  void close_device() {
    {
      std::lock_guard lk(g_mutex);
      if (!g_device) {
        return;
      }
    }
    g_ping_stop.store(true);
    if (g_ping_thread.joinable()) {
      g_ping_thread.join();
    }
    std::lock_guard lk(g_mutex);
    vgd_device_close(g_device);
    g_device = nullptr;
    g_caps.reset();
  }

  bool driver_ready() {
    std::lock_guard lk(g_mutex);
    return open_locked() == DRIVER_STATUS::OK;
  }

  bool start_ping_thread(std::function<void()> failCb) {
    std::lock_guard lk(g_mutex);
    if (open_locked() != DRIVER_STATUS::OK) {
      return false;
    }
    if (g_ping_thread.joinable()) {
      return true;  // already running
    }
    g_ping_fail_cb = std::move(failCb);
    g_ping_stop.store(false);
    g_ping_thread = std::thread([] {
      int consecutive_failures = 0;
      while (!g_ping_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!g_ping_feeding.load()) {
          continue;
        }
        std::vector<uint64_t> sessions;
        {
          std::lock_guard lk(g_mutex);
          if (!g_device) {
            break;
          }
          for (auto &[k, s] : g_sessions) {
            sessions.push_back(s.session_id);
          }
        }
        bool any_failed = false;
        for (uint64_t sid : sessions) {
          std::lock_guard lk(g_mutex);
          if (!g_device) {
            break;
          }
          if (vgd_ping(g_device, sid) == VGD_ERR_IO) {
            any_failed = true;
          }
        }
        consecutive_failures = any_failed ? consecutive_failures + 1 : 0;
        if (consecutive_failures >= 3) {
          BOOST_LOG(error) << "LuminalVGD watchdog: ping failed 3x — driver unreachable.";
          consecutive_failures = 0;
          if (g_ping_fail_cb) {
            g_ping_fail_cb();
          }
        }
      }
    });
    return true;
  }

  void set_watchdog_feeding(bool enable) {
    g_ping_feeding.store(enable);
  }

  std::optional<VirtualDisplayCreationResult> create_virtual_display(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps_millihz,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    bool enable_hdr
  ) {
    const auto before = luminal_display_names();

    // Create under the lock; poll for the display WITHOUT it (the ping
    // thread must keep feeding leases while we wait for PnP).
    std::unique_lock lk(g_mutex);
    if (open_locked() != DRIVER_STATUS::OK) {
      return std::nullopt;
    }

    if (auto it = g_sessions.find(key_of(guid)); it != g_sessions.end()) {
      // Same stream GUID re-requested: report the existing display.
      VirtualDisplayCreationResult result {};
      result.reused_existing = true;
      result.ready_since = std::chrono::steady_clock::now();
      result.client_name = it->second.client_name;
      lk.unlock();
      auto names = luminal_display_names();
      if (!names.empty()) {
        result.display_name = names.front();
        result.monitor_device_path = monitor_device_path_of(names.front());
        result.device_id = resolveVirtualDisplayDeviceId(names.front());
        std::lock_guard relock(g_mutex);
        if (auto again = g_sessions.find(key_of(guid)); again != g_sessions.end()) {
          again->second.display_name = names.front();
        }
      }
      return result;
    }

    VgdCreateRequest req {};
    req.session_id = fresh_session_id(guid);
    req.display_id = display_id_for_client(s_client_uid);
    req.adapter_luid = 0;        // driver default (largest VRAM)
    req.lease_timeout_ms = 0;    // driver default; ping thread feeds it
    // HDR10 when the client asked for it and the driver advertises the cap;
    // otherwise SDR-8. The driver's EDID grows the CTA-861.3 HDR block and
    // Windows offers advanced color on the monitor.
    const bool hdr = enable_hdr && g_caps && (g_caps->caps & VGD_CAP_HDR10);
    // Proto wire encoding (SudoVDA-ported): Sdr8=8, Sdr10=10, Hdr10=110,
    // Hdr12=112 — HDR depths carry a leading "1"; plain 10 with hdr=1 is
    // rejected as BAD_BIT_DEPTH.
    req.bit_depth = hdr ? 110 : 8;
    req.hdr = hdr ? 1 : 0;
    if (enable_hdr && !hdr) {
      BOOST_LOG(info) << "LuminalVGD: client requested HDR but the installed driver lacks HDR10 caps; creating SDR monitor.";
    }
    req.flags = 0;
    req.mode_count = 1;
    // Callers pass millihertz (nvhttp/webrtc normalize Hz → mHz before the
    // call); the spec below must NOT rescale it again.
    req.modes[0] = VgdModeSpec {width, height, fps_millihz};
    if (framegen_refresh_active && base_fps_millihz != 0 && base_fps_millihz != fps_millihz) {
      // Advertise the base rate too so the OS can drop out of the
      // frame-generation-doubled mode without a monitor cycle.
      req.modes[1] = VgdModeSpec {width, height, base_fps_millihz};
      req.mode_count = 2;
    }
    if (s_client_name) {
      const size_t n = std::min<size_t>(std::strlen(s_client_name), 31);
      for (size_t i = 0; i < n; ++i) {
        req.friendly_name[i] = static_cast<uint16_t>(s_client_name[i]);
      }
    }

    VgdCreateReply reply {};
    const int io = vgd_create_monitor(g_device, &req, &reply);
    if (io != 0 || reply.result != 0) {
      BOOST_LOG(error) << "LuminalVGD CREATE_MONITOR failed: io=" << io
                       << " result=" << reply.result;
      return std::nullopt;
    }
    g_sessions[key_of(guid)] = TrackedSession {
      req.session_id,
      reply.display_id,
      s_client_name ? s_client_name : "",
      reply.ring_slots,
      {},
    };
    BOOST_LOG(info) << "LuminalVGD monitor created: session 0x" << std::hex << req.session_id
                    << " display 0x" << reply.display_id << std::dec << " connector "
                    << reply.connector_index << ' ' << width << 'x' << height << '@' << fps_millihz << "mHz";
    lk.unlock();

    // The monitor arrives asynchronously; wait briefly for the OS to
    // surface the new display so callers get a usable display name.
    VirtualDisplayCreationResult result {};
    result.reused_existing = false;
    result.ready_since = std::chrono::steady_clock::now();
    result.client_name = s_client_name ? std::optional<std::string> {s_client_name} : std::nullopt;
    for (int attempt = 0; attempt < 50; ++attempt) {  // ≤5 s
      auto now = luminal_display_names();
      for (auto &name : now) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
          result.display_name = name;
          result.monitor_device_path = monitor_device_path_of(name);
          // Resolve the libdisplaydevice device id so the display-helper
          // topology layer can target the new monitor directly.
          result.device_id = resolveVirtualDisplayDeviceId(name);
          {
            // Record the GDI name so the ring capture backend can map
            // this display back to its session.
            std::lock_guard relock(g_mutex);
            if (auto it = g_sessions.find(key_of(guid)); it != g_sessions.end()) {
              it->second.display_name = name;
            }
          }
          return result;
        }
      }
      // The monitor arrives inactive (it only attaches to the desktop once
      // the display helper applies the topology), so the attached-display
      // poll above may never see it. The client-name resolver enumerates
      // inactive devices too — a resolved device id is enough for the
      // topology layer to activate the display.
      if (s_client_name) {
        if (auto id = resolveVirtualDisplayDeviceIdForClient(s_client_name)) {
          result.device_id = std::move(id);
          return result;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_LOG(warning) << "LuminalVGD monitor created but no new display surfaced within 5 s.";
    return result;
  }

  bool remove_virtual_display(const GUID &guid) {
    std::lock_guard lk(g_mutex);
    auto it = g_sessions.find(key_of(guid));
    if (it == g_sessions.end()) {
      return false;
    }
    bool ok = false;
    if (g_device) {
      ok = vgd_destroy_monitor(g_device, it->second.session_id) == 0;
    }
    g_sessions.erase(it);
    return ok;
  }

  bool remove_all_virtual_displays() {
    std::lock_guard lk(g_mutex);
    bool all_ok = true;
    for (auto &[k, s] : g_sessions) {
      if (!g_device || vgd_destroy_monitor(g_device, s.session_id) != 0) {
        all_ok = false;
      }
    }
    g_sessions.clear();
    return all_ok;
  }

  bool is_guid_tracked(const GUID &guid) {
    std::lock_guard lk(g_mutex);
    return g_sessions.contains(key_of(guid));
  }

  std::optional<std::string> driver_version_string() {
    std::lock_guard lk(g_mutex);
    if (open_locked() != DRIVER_STATUS::OK || !g_caps) {
      return std::nullopt;
    }
    return "proto " + std::to_string(g_caps->proto_major) + '.' +
           std::to_string(g_caps->proto_minor) + " build " +
           std::to_string(g_caps->driver_build);
  }

  bool driver_supports_hdr() {
    std::lock_guard lk(g_mutex);
    return g_caps && (g_caps->caps & VGD_CAP_HDR10);
  }

  std::optional<RingTargetInfo> ring_target_for_display(const std::string &display_name) {
    std::wstring wanted(display_name.begin(), display_name.end());
    std::lock_guard lk(g_mutex);
    if (g_sessions.empty()) {
      return std::nullopt;
    }
    if (g_sessions.size() == 1) {
      const auto &s = g_sessions.begin()->second;
      return RingTargetInfo {s.session_id, s.ring_slots};
    }
    for (const auto &[k, s] : g_sessions) {
      if (!s.display_name.empty() && s.display_name == wanted) {
        return RingTargetInfo {s.session_id, s.ring_slots};
      }
    }
    BOOST_LOG(warning) << "LuminalVGD: no tracked session matches display '"
                       << display_name << "' (" << g_sessions.size() << " sessions).";
    return std::nullopt;
  }

}  // namespace VDISPLAY::vgd
