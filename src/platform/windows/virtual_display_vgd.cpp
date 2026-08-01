/**
 * @file src/platform/windows/virtual_display_vgd.cpp
 * @brief LuminalVGD backend implementation over the luminal-vgd-ffi C ABI.
 */

#include "src/platform/windows/virtual_display_vgd.h"

#include "src/platform/windows/vgd_devnode.h"

#include "src/config.h"
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
      /// The client UID this monitor was built for. Recorded so the reuse
      /// branch can prove the re-requested stream GUID belongs to the same
      /// client before handing back a monitor built to someone else's mode.
      std::string client_uid;
      /// libdisplaydevice device id, recorded whenever the monitor was
      /// resolved while still INACTIVE (so `display_name` is necessarily
      /// empty). It is the only handle on such a session until the display
      /// helper attaches it, and `ring_target_for_display` needs one to
      /// tell two pre-APPLY sessions apart.
      std::string device_id;
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

    /// The EDID serial the driver stamps for a display identity.
    ///
    /// MUST stay in step with `luminal-vgd-core`'s
    /// `identity::serial_from_display_id` (the 32-bit fold below); it is
    /// duplicated rather than plumbed through the FFI because
    /// `VgdCreateReply` carries no serial field and reply structs in this
    /// proto can never grow. If the driver ever changes the derivation,
    /// identity resolution silently degrades to the client-name arm — it
    /// does not break, but the LuminalVGD backend loses its only
    /// name-length-independent way to find an inactive monitor.
    uint32_t edid_serial_for_display_id(uint64_t display_id) {
      return static_cast<uint32_t>(display_id ^ (display_id >> 32));
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
        // Win32 error 2/259: driver not installed (interface absent);
        // error 5: the driver's SYSTEM/Administrators ACL refused this
        // process — the 2026-07 streaming outage looked exactly like this.
        BOOST_LOG(error) << "LuminalVGD control device open failed (Win32 error "
                         << vgd_last_error() << ").";
        return DRIVER_STATUS::FAILED;
      }
      VgdCaps caps {};
      if (const int hs = vgd_handshake(g_device, &caps); hs != 0) {
        BOOST_LOG(error) << "LuminalVGD handshake failed: code " << hs
                         << " (Win32 error " << vgd_last_error() << ").";
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
    if (platf::vgd_devnode::rebind_in_flight()) {
      // A background driver rebind is replacing the devnode right now; a
      // session created into it would be yanked by the recreate fallback
      // seconds later. Fail fast — the caller's virtual_display_failed
      // fallback handles this session, and the next attempt lands on the
      // new driver.
      BOOST_LOG(warning) << "Virtual display creation refused: a LuminalVGD driver rebind "
                            "is in progress; retry shortly.";
      return std::nullopt;
    }
    const auto before = luminal_display_names();

    // Create under the lock; poll for the display WITHOUT it (the ping
    // thread must keep feeding leases while we wait for PnP).
    std::unique_lock lk(g_mutex);
    if (open_locked() != DRIVER_STATUS::OK) {
      return std::nullopt;
    }

    if (auto it = g_sessions.find(key_of(guid)); it != g_sessions.end()) {
      // Same stream GUID re-requested. Only hand back the existing monitor if
      // it was actually built for this client and still exists: the tracked
      // entry carries the mode, HDR depth and friendly name the OTHER client
      // asked for, so reusing it across identities silently streams the wrong
      // resolution/refresh/HDR state with nothing in the log to say why.
      const std::string requested_uid = s_client_uid ? s_client_uid : "";
      const bool same_client = it->second.client_uid == requested_uid;
      const bool have_name = !it->second.display_name.empty();
      bool still_present = false;
      if (same_client && have_name) {
        const auto present = luminal_display_names();
        still_present = std::find(present.begin(), present.end(), it->second.display_name) != present.end();
      }

      if (same_client && have_name && still_present) {
        VirtualDisplayCreationResult result {};
        result.reused_existing = true;
        result.ready_since = std::chrono::steady_clock::now();
        result.client_name = it->second.client_name;
        // Use the display THIS session recorded, not names.front(): the latter
        // is an arbitrary pick from every attached LuminalVGD adapter in
        // EnumDisplayDevicesW order, which is only correct when exactly one
        // virtual monitor exists.
        // Copy everything needed off the entry BEFORE unlocking: once the lock
        // is dropped another thread may erase it, and `it` dangles.
        const auto tracked_name = it->second.display_name;
        const auto tracked_session_id = it->second.session_id;
        lk.unlock();
        result.display_name = tracked_name;
        result.monitor_device_path = monitor_device_path_of(tracked_name);
        result.device_id = resolveVirtualDisplayDeviceId(tracked_name);
        BOOST_LOG(info) << "LuminalVGD: reusing existing monitor for the same client (session 0x"
                        << std::hex << tracked_session_id << std::dec << ").";
        return result;
      }

      // Identity mismatch or the monitor is gone. Drop the stale tracking and
      // fall through to create a monitor at THIS client's mode. Never refuse:
      // returning nullopt here would leave an exclusive-layout host with no
      // output at all.
      BOOST_LOG(warning) << "LuminalVGD: stream GUID collision on an existing monitor — tracked client_uid='"
                         << it->second.client_uid << "' vs requested '" << requested_uid
                         << "'" << (have_name ? "" : ", tracked display never surfaced")
                         << ((same_client && have_name && !still_present) ? ", tracked display no longer present" : "")
                         << ". Discarding the stale session and creating a monitor for this client.";
      if (g_device) {
        vgd_destroy_monitor(g_device, it->second.session_id);
      }
      g_sessions.erase(it);
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
    if (hdr) {
      // EDID peak luminance from config (proto 0.4 additive field). 0 on
      // the wire means "driver default (993 nits)"; a slider value of 0
      // is sent as 1 so it clamps to the CTA floor (~51 nits) instead of
      // silently meaning "default". Pre-0.4 drivers (build ≤14) ignore
      // the tail bytes entirely.
      const int nits = config::video.vgd_hdr_peak_nits;
      req.max_nits = static_cast<uint32_t>(std::max(nits, 1));
      if (g_caps && g_caps->proto_minor < 4 && nits != 800) {
        BOOST_LOG(info) << "LuminalVGD: vgd_hdr_peak_nits=" << nits
                        << " requires driver build 15+ (proto 0.4); the installed driver ("
                        << "proto " << g_caps->proto_major << '.' << g_caps->proto_minor
                        << ") uses its built-in 993 nits.";
      }
    }
    req.flags = 0;
    req.mode_count = 1;
    // Callers pass millihertz (nvhttp/webrtc normalize Hz → mHz before the
    // call); the spec below must NOT rescale it again.
    req.modes[0] = VgdModeSpec {width, height, fps_millihz};
    // Advertise the base rate alongside the requested one whenever they differ,
    // NOT only when a per-app frame-generation flag happened to be set.
    //
    // A monitor's mode list is fixed for its lifetime: IddCx has no DDI, in 1.10
    // or 1.11, that replaces a monitor description on an arrived monitor, and
    // the only way to change the advertised set is destroy + recreate — the
    // monitor cycle that broadcasts DBT_DEVNODES_CHANGED and kills GTA V
    // Enhanced. So whatever is advertised here is all this session will ever
    // have. Anything not in this list cannot be reached later at any price.
    //
    // That makes the guard actively harmful in the case that matters. A client
    // streams "Desktop" — which has no per-app flag — and then launches a
    // frame-generation game inside the session. With the guard, mode_count is 1
    // and the doubled rate was never advertised, so it can never be selected.
    // Worse, the failure is silent: the host's apply path already requests 2x
    // base on every session (virtual_double_refresh defaults true and
    // virtual_display_mode defaults per_client), and libdisplaydevice snaps an
    // unadvertised request to the nearest supported mode and reports success.
    // The log says the refresh changed; nothing changed.
    //
    // Advertising the superset up front costs two mode-list entries and removes
    // both problems, with no display event at game launch because nothing has to
    // change. A virtual display idling at the higher rate is free.
    if (base_fps_millihz != 0 && base_fps_millihz != fps_millihz) {
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
      s_client_uid ? s_client_uid : "",
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
    // 50 passes, but NOT 5 s: each pass also runs two full display
    // enumerations, so the measured wall clock is ~7 s. Keep the number
    // quoted in the failure message below in step with that.
    for (int attempt = 0; attempt < 50; ++attempt) {
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
      // poll above may never see it. Both resolvers below enumerate
      // inactive devices too — a resolved device id is enough for the
      // topology layer to activate the display.
      //
      // IDENTITY FIRST, LABEL SECOND. The driver stamps this monitor's
      // EDID serial from the very display id we asked for, so the serial
      // match is exact and says nothing about what the client called
      // itself. The label arm cannot see any client whose name outgrows
      // the 13-byte EDID product-name descriptor — which is most of them
      // ('LG C2 83" OLED webOS', 'XBOXONE Series X') — so leaving it as
      // the only fallback meant an arrived-but-inactive monitor was
      // reported missing and then destroyed, on a loop, for those clients.
      // reply.display_id, not req.display_id: the driver's answer is what
      // it actually stamped the EDID from, and it is free to hand back an
      // identity other than the one asked for.
      auto resolved = resolveVirtualDisplayDeviceIdForEdidSerial(edid_serial_for_display_id(reply.display_id));
      if (!resolved && s_client_name) {
        resolved = resolveVirtualDisplayDeviceIdForClient(s_client_name);
      }
      if (resolved) {
        result.device_id = resolved;
        // Record it: the display has no GDI name yet (that arrives with
        // the topology apply), so this id is the only thing that can tell
        // this session apart from another pre-APPLY one later.
        std::lock_guard relock(g_mutex);
        if (auto it = g_sessions.find(key_of(guid)); it != g_sessions.end()) {
          it->second.device_id = *resolved;
        }
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Nothing surfaced: Windows accepted CREATE_MONITOR but never
    // enumerated the display — the signature of a dead display stack.
    // Returning "success" here used to leave the driver session alive
    // with no display behind it; each client retry then stacked another
    // zombie ("no tracked session matches display (2 sessions)" in the
    // 2026-07-27 incident), and the driver's watchdog couldn't reap them
    // because our ping thread kept the leases fed. Destroy what we
    // created and report failure so the caller can fall back cleanly.
    BOOST_LOG(error) << "LuminalVGD monitor created but no new display surfaced within ~7 s; "
                        "destroying the orphaned driver session (session 0x"
                     << std::hex << req.session_id << std::dec
                     << "). If this repeats, the Windows display stack is likely down — "
                        "check Troubleshooting.";
    {
      std::lock_guard relock(g_mutex);
      if (auto it = g_sessions.find(key_of(guid)); it != g_sessions.end()) {
        if (g_device) {
          vgd_destroy_monitor(g_device, it->second.session_id);
        }
        g_sessions.erase(it);
      }
    }
    return std::nullopt;
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

  std::optional<uint32_t> driver_build() {
    std::lock_guard lk(g_mutex);
    if (open_locked() != DRIVER_STATUS::OK || !g_caps) {
      return std::nullopt;
    }
    return g_caps->driver_build;
  }

  size_t tracked_session_count() {
    std::lock_guard lk(g_mutex);
    return g_sessions.size();
  }

  bool driver_supports_hdr() {
    std::lock_guard lk(g_mutex);
    return g_caps && (g_caps->caps & VGD_CAP_HDR10);
  }

  std::optional<RingTargetInfo> ring_target_for_display(const std::string &display_name) {
    std::wstring wanted(display_name.begin(), display_name.end());
    std::unique_lock lk(g_mutex);
    if (g_sessions.empty()) {
      return std::nullopt;
    }
    if (g_sessions.size() == 1) {
      auto &s = g_sessions.begin()->second;
      if (s.display_name.empty()) {
        // The monitor may have activated after create()'s bounded poll
        // gave up (activation completes at display-APPLY time); nothing
        // else ever backfills the tracked name, so adopt it here.
        auto names = luminal_display_names();
        if (names.size() == 1) {
          s.display_name = names.front();
        }
      }
      if (s.display_name.empty()) {
        // Never hand out the ring for a monitor the OS never activated:
        // downstream capture init would bind an arbitrary mid-modeset
        // physical output against this ring and spin a reinit storm
        // whose overlapping NvEnc create/destroy cycles have corrupted
        // the NVIDIA driver heap before (0xc0000374 fail-fast). Refuse
        // here so platf::display falls back to WGC/DDA or the session
        // fails cleanly.
        BOOST_LOG(warning) << "LuminalVGD: sole session 0x" << std::hex << s.session_id << std::dec
                           << " has no active GDI display (monitor never activated); "
                           << "refusing ring capture.";
        return std::nullopt;
      }
      if (!wanted.empty() && s.display_name != wanted) {
        // Silent until now, and indistinguishable in a log from "no
        // session at all". Worth a line: during topology churn the caller
        // can ask for a name the sole session does not (yet) own, and
        // knowing WHICH name was asked for is the difference between a
        // benign mid-modeset miss and a genuinely mistracked display.
        // Narrowed the same way `wanted` was widened; GDI names are ASCII.
        const std::string tracked(s.display_name.begin(), s.display_name.end());
        BOOST_LOG(debug) << "LuminalVGD: sole session 0x" << std::hex << s.session_id << std::dec
                         << " tracks display '" << tracked << "', not '" << display_name
                         << "'; declining ring capture for it.";
        return std::nullopt;
      }
      return RingTargetInfo {s.session_id, s.ring_slots};
    }
    for (const auto &[k, s] : g_sessions) {
      if (!s.display_name.empty() && s.display_name == wanted) {
        return RingTargetInfo {s.session_id, s.ring_slots};
      }
    }
    // No name matched. A session whose monitor was resolved while still
    // INACTIVE has no GDI name to match with — create()'s poll found it by
    // identity, and the name only exists once the helper attaches it — so
    // with two or more sessions the loop above can never match it and ring
    // capture was refused for a display that is now perfectly usable.
    // Map the requested GDI name back to its device id and match on that,
    // then backfill the name so the cheap comparison works from here on.
    const bool have_pre_apply_session = std::any_of(
      g_sessions.begin(),
      g_sessions.end(),
      [](const auto &kv) {
        return kv.second.display_name.empty() && !kv.second.device_id.empty();
      }
    );
    if (!wanted.empty() && have_pre_apply_session) {
      // Resolving a device id is a display-config query with its own
      // internal retries — on a wedged stack it takes seconds. It must not
      // run under g_mutex, which create/destroy/ping all need; that is the
      // same "never hold a lock a teardown path needs across an OS call"
      // rule the ring-lock convoy taught us. Drop it, query, retake, and
      // re-derive everything from the map as it is afterwards.
      //
      // STRICT resolver, deliberately. `resolveVirtualDisplayDeviceId`
      // falls back to "any active virtual display" and then "any virtual
      // display", so it practically never reports nothing — and an
      // INACTIVE display enumerates with an empty GDI name, so the name it
      // is given here routinely matches nothing and the fallback fires.
      // Pairing that arbitrary id with a session would adopt a display
      // name onto the wrong session, hand a client another client's ring,
      // and poison the tracked name permanently (nothing ever clears it,
      // so that session would lose ring capture for good).
      lk.unlock();
      auto wanted_device_id = resolveVirtualDisplayDeviceIdExact(wanted);
      lk.lock();
      if (wanted_device_id) {
        for (auto &[k, s] : g_sessions) {
          if (!s.display_name.empty() || s.device_id.empty() || s.device_id != *wanted_device_id) {
            continue;
          }
          s.display_name = wanted;
          BOOST_LOG(info) << "LuminalVGD: adopted display '" << display_name << "' for session 0x"
                          << std::hex << s.session_id << std::dec
                          << " by device id (monitor activated after creation).";
          return RingTargetInfo {s.session_id, s.ring_slots};
        }
      }
    }
    BOOST_LOG(warning) << "LuminalVGD: no tracked session matches display '"
                       << display_name << "' (" << g_sessions.size() << " sessions).";
    return std::nullopt;
  }

}  // namespace VDISPLAY::vgd
