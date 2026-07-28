/**
 * @file src/platform/windows/vgd_devnode.cpp
 * @brief Service-owned LuminalVGD devnode + no-reboot driver switchover
 *        (see vgd_devnode.h for the design).
 *
 * Review-hardened (2026-07-28 adversarial pass):
 *  - The device rebind runs on a detached worker, never on the startup
 *    thread — UpdateDriverForPlugAndPlayDevices is synchronous and
 *    unbounded, and first backend selection happens before the web UI
 *    exists.
 *  - The version check reads the devnode's registry DriverVersion (full
 *    4-field tuple compare) instead of the control-device handshake, so
 *    an idle service never holds the control handle open — a held handle
 *    would veto device stops and reintroduce reboot-required updates for
 *    the running-service (dev sign/test) flow.
 *  - Software-device creation is attempted only when the driver package
 *    is actually STAGED in the DriverStore; a bundled-but-unstaged
 *    package (portable zip, never installed) must not burn a 20 s
 *    device-start wait on every service start.
 *  - When a device exists but its control interface is not up yet,
 *    backend selection is deliberately NOT latched (device_expected()) —
 *    a slow driver start must not disable virtual displays for the
 *    process lifetime.
 */
// platform includes
#include <winsock2.h>
#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>

// MinGW-w64 does not ship swdevice.h; declare the minimal software-device
// ABI locally (layout verified against the Windows SDK swdevice.h — this
// struct and callback signature are a stable public contract).
DECLARE_HANDLE(HSWDEVICE);
using PHSWDEVICE = HSWDEVICE *;
using SW_DEVICE_CREATE_CALLBACK = void(WINAPI *)(HSWDEVICE hSwDevice, HRESULT CreateResult, PVOID pContext, PCWSTR pszDeviceInstanceId);

enum SW_DEVICE_CAPABILITIES {
  SWDeviceCapabilitiesNone = 0,
  SWDeviceCapabilitiesRemovable = 1,
  SWDeviceCapabilitiesSilentInstall = 2,
  SWDeviceCapabilitiesNoDisplayInUI = 4,
  SWDeviceCapabilitiesDriverRequired = 8,
};

typedef struct _SW_DEVICE_CREATE_INFO {
  ULONG cbSize;
  PCWSTR pszInstanceId;
  PCWSTR pszzHardwareIds;  ///< multi-sz
  PCWSTR pszzCompatibleIds;  ///< multi-sz
  const GUID *pContainerId;
  ULONG CapabilityFlags;
  PCWSTR pszDeviceDescription;
  PCWSTR pszDeviceLocation;
  const SECURITY_DESCRIPTOR *pSecurityDescriptor;
} SW_DEVICE_CREATE_INFO;

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/windows/diag_info.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/vgd_devnode.h"
#include "src/platform/windows/virtual_display_vgd.h"
#include "src/tdr_state.h"

namespace platf::vgd_devnode {

  namespace {
    constexpr wchar_t kHardwareId[] = L"root\\luminal_vgd";
    /// Multi-sz: the literal carries an embedded NUL + the implicit
    /// terminator, forming the required double-NUL.
    constexpr wchar_t kHardwareIdMultiSz[] = L"root\\luminal_vgd\0";
    constexpr wchar_t kDeviceDescription[] = L"Luminal Video Graphics Display";
    /// GUID_DEVCLASS_DISPLAY, defined locally to avoid initguid.h
    /// ordering games with the extern from devguid.h.
    constexpr GUID kDisplayClassGuid = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

    /// The software device the service owns for its lifetime (default
    /// SWDeviceLifetimeHandle: the device disappears when the process
    /// dies, and re-arrives at the next service start). Closed only by
    /// the rebind worker, immediately before recreating.
    HSWDEVICE g_sw_device = nullptr;

    /// True once a devnode was adopted or created this process — backend
    /// selection uses it to avoid latching NONE while a driver start is
    /// still in flight.
    std::atomic<bool> g_device_expected {false};

    /// Rebind-worker coordination. The worker is a real (joinable)
    /// thread, not detached: a detached worker racing process exit runs
    /// into destructed statics (vgd g_mutex/g_sessions, Boost.Log) — UB
    /// that the crash handler would then report as a spurious shutdown
    /// crash. shutdown_join() bounds the wait instead.
    std::atomic<bool> g_rebind_in_flight {false};
    std::atomic<bool> g_stop_requested {false};
    std::thread g_rebind_thread;

    // SwDeviceCreate/SwDeviceClose are bound dynamically: MinGW toolchains
    // ship no import library (or header) for them. The exports live in
    // cfgmgr32.dll (already loaded — CM_* is used below), with the
    // api-ms-win-devices-swdevice-l1-1-0 apiset as the documented
    // alternate name. Both functions must resolve or neither is used —
    // the create-timeout path relies on SwDeviceClose's documented
    // guarantee that no callback fires after it returns.
    using SwDeviceCreate_t = HRESULT(WINAPI *)(PCWSTR, PCWSTR, const SW_DEVICE_CREATE_INFO *, ULONG, const void *, SW_DEVICE_CREATE_CALLBACK, PVOID, PHSWDEVICE);
    using SwDeviceClose_t = VOID(WINAPI *)(HSWDEVICE);

    struct swdevice_api_t {
      SwDeviceCreate_t create {nullptr};
      SwDeviceClose_t close {nullptr};

      bool usable() const {
        return create && close;
      }
    };

    const swdevice_api_t &swdevice_api() {
      static const swdevice_api_t api = []() {
        swdevice_api_t out {};
        HMODULE mod = LoadLibraryW(L"cfgmgr32.dll");
        if (!mod || !GetProcAddress(mod, "SwDeviceCreate")) {
          mod = LoadLibraryW(L"api-ms-win-devices-swdevice-l1-1-0.dll");
        }
        if (mod) {
          out.create = reinterpret_cast<SwDeviceCreate_t>(reinterpret_cast<void *>(GetProcAddress(mod, "SwDeviceCreate")));
          out.close = reinterpret_cast<SwDeviceClose_t>(reinterpret_cast<void *>(GetProcAddress(mod, "SwDeviceClose")));
        }
        return out;
      }();
      return api;
    }

    /// newdev!UpdateDriverForPlugAndPlayDevicesW, also bound dynamically
    /// (same MinGW import-library caveat).
    using UpdateDriver_t = BOOL(WINAPI *)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

    UpdateDriver_t update_driver_fn() {
      static const UpdateDriver_t fn = []() -> UpdateDriver_t {
        HMODULE mod = LoadLibraryW(L"newdev.dll");
        if (!mod) {
          return nullptr;
        }
        return reinterpret_cast<UpdateDriver_t>(reinterpret_cast<void *>(GetProcAddress(mod, "UpdateDriverForPlugAndPlayDevicesW")));
      }();
      return fn;
    }

    bool multi_sz_contains(const std::vector<wchar_t> &multi_sz, const wchar_t *needle) {
      const wchar_t *p = multi_sz.data();
      const wchar_t *end = p + multi_sz.size();
      while (p < end && *p) {
        if (_wcsicmp(p, needle) == 0) {
          return true;
        }
        p += wcslen(p) + 1;
      }
      return false;
    }

    /// Find a `root\luminal_vgd` devnode by hardware id. Display-class
    /// only (fast, and both the persistent ROOT and our SWD devices carry
    /// that class once installed). `present_only` false includes phantoms.
    std::optional<std::wstring> find_device_instance(bool present_only) {
      GUID display_class = kDisplayClassGuid;
      HDEVINFO set = SetupDiGetClassDevsW(&display_class, nullptr, nullptr, present_only ? DIGCF_PRESENT : 0);
      if (set == INVALID_HANDLE_VALUE) {
        return std::nullopt;
      }
      auto close_set = std::unique_ptr<void, decltype(&SetupDiDestroyDeviceInfoList)>(set, &SetupDiDestroyDeviceInfoList);

      SP_DEVINFO_DATA data {};
      data.cbSize = sizeof(data);
      for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &data); ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, nullptr, 0, &needed);
        if (needed == 0) {
          continue;
        }
        std::vector<wchar_t> ids(needed / sizeof(wchar_t) + 2, L'\0');
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids.data()), needed, nullptr)) {
          continue;
        }
        if (!multi_sz_contains(ids, kHardwareId)) {
          continue;
        }
        wchar_t instance[MAX_DEVICE_ID_LEN] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &data, instance, _countof(instance), nullptr)) {
          return std::wstring(instance);
        }
      }
      return std::nullopt;
    }

    /// Is the LuminalVGD driver package actually staged in the
    /// DriverStore? A bundled-but-unstaged package (portable zip, no
    /// installer ever ran) must NOT trigger software-device creation:
    /// with SWDeviceCapabilitiesDriverRequired the device would never
    /// start and every service start would burn the full started-wait.
    bool driver_store_has_package() {
      wchar_t win_dir[MAX_PATH] = {};
      if (GetSystemWindowsDirectoryW(win_dir, _countof(win_dir)) == 0) {
        return false;
      }
      const std::filesystem::path repo = std::filesystem::path(win_dir) / L"System32" / L"DriverStore" / L"FileRepository";
      std::error_code ec;
      for (std::filesystem::directory_iterator it(repo, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        std::error_code dir_ec;
        if (!it->is_directory(dir_ec)) {
          continue;
        }
        const auto name = it->path().filename().wstring();
        if (_wcsnicmp(name.c_str(), L"luminalvgd.inf_", 15) == 0) {
          return true;
        }
      }
      return false;
    }

    bool device_started(const std::wstring &instance) {
      DEVINST dev_inst = 0;
      if (CM_Locate_DevNodeW(&dev_inst, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return false;
      }
      ULONG status = 0, problem = 0;
      if (CM_Get_DevNode_Status(&status, &problem, dev_inst, 0) != CR_SUCCESS) {
        return false;
      }
      return (status & DN_STARTED) && !(status & DN_HAS_PROBLEM);
    }

    /// Wait for the devnode to reach DN_STARTED. Driver install on a
    /// fresh device runs asynchronously after SwDeviceCreate's callback.
    bool wait_device_started(const std::wstring &instance, std::chrono::milliseconds budget) {
      const auto deadline = std::chrono::steady_clock::now() + budget;
      while (std::chrono::steady_clock::now() < deadline) {
        if (device_started(instance)) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      return device_started(instance);
    }

    struct sw_create_ctx_t {
      HANDLE event {nullptr};
      HRESULT result {E_PENDING};
      std::wstring instance;
      std::mutex mutex;
    };

    void WINAPI sw_create_callback(HSWDEVICE, HRESULT create_result, PVOID context, PCWSTR instance_id) {
      auto *ctx = static_cast<sw_create_ctx_t *>(context);
      {
        std::lock_guard lk(ctx->mutex);
        ctx->result = create_result;
        if (instance_id) {
          ctx->instance = instance_id;
        }
      }
      SetEvent(ctx->event);
    }

    /// Create (or re-arrive) the service-owned software device. Returns
    /// the device instance id on success.
    std::optional<std::wstring> create_software_device() {
      const auto &api = swdevice_api();
      if (!api.usable()) {
        BOOST_LOG(warning) << "LuminalVGD devnode: the software-device API is unavailable on this system.";
        return std::nullopt;
      }
      if (g_sw_device) {
        // A previous create in this process is still alive; close it so
        // the re-create below starts clean (rebind path).
        api.close(g_sw_device);
        g_sw_device = nullptr;
      }

      sw_create_ctx_t ctx;
      ctx.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!ctx.event) {
        return std::nullopt;
      }
      auto close_event = std::unique_ptr<void, decltype(&CloseHandle)>(ctx.event, &CloseHandle);

      SW_DEVICE_CREATE_INFO create_info {};
      create_info.cbSize = sizeof(create_info);
      create_info.pszInstanceId = L"VirtualDisplay0";
      create_info.pszzHardwareIds = kHardwareIdMultiSz;
      create_info.pszDeviceDescription = kDeviceDescription;
      create_info.CapabilityFlags = static_cast<ULONG>(SWDeviceCapabilitiesSilentInstall) | static_cast<ULONG>(SWDeviceCapabilitiesDriverRequired);

      HSWDEVICE device = nullptr;
      const HRESULT hr = api.create(L"LuminalVGD", L"HTREE\\ROOT\\0", &create_info, 0, nullptr, &sw_create_callback, &ctx, &device);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "LuminalVGD devnode: SwDeviceCreate failed (hr=0x"
                         << std::hex << hr << std::dec << ").";
        return std::nullopt;
      }

      // 15 s: driver matching + UMDF host start on a cold DriverStore can
      // take several seconds; the callback fires once the device instance
      // exists (driver start continues asynchronously after that). The
      // timeout path is safe: SwDeviceClose documents that no callback
      // fires after it returns, and ctx is still in scope at that point
      // (api.usable() above guarantees close resolved).
      const DWORD wait = WaitForSingleObject(ctx.event, 15000);
      HRESULT create_result = E_PENDING;
      std::wstring instance;
      {
        std::lock_guard lk(ctx.mutex);
        create_result = ctx.result;
        instance = ctx.instance;
      }
      if (wait != WAIT_OBJECT_0 || FAILED(create_result) || instance.empty()) {
        BOOST_LOG(error) << "LuminalVGD devnode: software device creation did not complete "
                         << "(wait=" << wait << ", result=0x" << std::hex << create_result
                         << std::dec << ").";
        api.close(device);
        return std::nullopt;
      }

      g_sw_device = device;  // held for the life of the process, by design
      BOOST_LOG(info) << "LuminalVGD devnode: software device created ("
                      << platf::to_utf8(instance) << ").";
      return instance;
    }

    /// Remove a devnode (present or phantom) so the next create re-ranks
    /// drivers from scratch. SetupDi DIF_REMOVE handles the stop.
    bool uninstall_device_instance(const std::wstring &instance) {
      HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
      if (set == INVALID_HANDLE_VALUE) {
        return false;
      }
      auto close_set = std::unique_ptr<void, decltype(&SetupDiDestroyDeviceInfoList)>(set, &SetupDiDestroyDeviceInfoList);
      SP_DEVINFO_DATA data {};
      data.cbSize = sizeof(data);
      if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &data)) {
        return false;
      }
      if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &data)) {
        BOOST_LOG(warning) << "LuminalVGD devnode: DIF_REMOVE failed for "
                           << platf::to_utf8(instance) << " (error "
                           << GetLastError() << ").";
        return false;
      }
      return true;
    }

    std::optional<std::filesystem::path> bundled_inf_path() {
      wchar_t exe_path[MAX_PATH] = {};
      if (GetModuleFileNameW(nullptr, exe_path, _countof(exe_path)) == 0) {
        return std::nullopt;
      }
      std::filesystem::path inf = std::filesystem::path(exe_path).parent_path() / L"drivers" / L"luminalvgd" / L"driver-package" / L"luminalvgd.inf";
      std::error_code ec;
      if (!std::filesystem::exists(inf, ec)) {
        return std::nullopt;
      }
      return inf;
    }

    struct update_result_t {
      bool ok {false};
      bool reboot_required {false};
    };

    update_result_t update_driver_in_place(const std::filesystem::path &inf) {
      update_result_t out {};
      const auto fn = update_driver_fn();
      if (!fn) {
        BOOST_LOG(warning) << "LuminalVGD devnode: newdev.dll unavailable; cannot rebind in place.";
        return out;
      }
      BOOL reboot = FALSE;
      constexpr DWORD INSTALLFLAG_FORCE_ = 0x1;
      if (!fn(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE_, &reboot)) {
        BOOST_LOG(warning) << "LuminalVGD devnode: UpdateDriverForPlugAndPlayDevices failed (error "
                           << GetLastError() << ").";
        return out;
      }
      out.ok = true;
      out.reboot_required = (reboot != FALSE);
      return out;
    }

    /// Full rebind: close our software-device handle FIRST (a live
    /// SWDeviceLifetimeHandle pins the device across a PnP remove),
    /// remove EVERY matching devnode (present or phantom — a dev box can
    /// have both a legacy persistent node and our SWD phantom), then
    /// recreate — a brand-new device instance runs full driver ranking,
    /// so the newest staged package always wins.
    bool rebind_via_recreate() {
      const auto &api = swdevice_api();
      if (g_sw_device && api.usable()) {
        api.close(g_sw_device);
        g_sw_device = nullptr;
      }
      for (int i = 0; i < 4; ++i) {
        auto stale = find_device_instance(false);
        if (!stale) {
          break;
        }
        BOOST_LOG(info) << "LuminalVGD devnode: removing " << platf::to_utf8(*stale)
                        << " for a clean rebind.";
        if (!uninstall_device_instance(*stale)) {
          return false;
        }
      }
      auto created = create_software_device();
      if (!created) {
        return false;
      }
      return wait_device_started(*created, std::chrono::milliseconds(20000));
    }

    /// "x.y.z.w" -> {x,y,z,w}. DriverVer versions are four numeric fields.
    std::optional<std::array<std::uint32_t, 4>> parse_version_tuple(const std::string &version) {
      std::array<std::uint32_t, 4> out {};
      std::istringstream in(version);
      for (int i = 0; i < 4; ++i) {
        std::uint32_t field = 0;
        if (!(in >> field)) {
          return std::nullopt;
        }
        out[i] = field;
        if (i < 3) {
          char dot = 0;
          if (!(in >> dot) || dot != '.') {
            return std::nullopt;
          }
        }
      }
      return out;
    }

    std::string tuple_to_string(const std::array<std::uint32_t, 4> &v) {
      return std::to_string(v[0]) + '.' + std::to_string(v[1]) + '.' + std::to_string(v[2]) + '.' + std::to_string(v[3]);
    }

    /// The rebind itself, on a detached worker: UpdateDriver is
    /// synchronous with no timeout, and first backend selection runs on
    /// the startup thread before the web UI exists — a wedged PnP stack
    /// must not take the whole service down with it.
    void rebind_worker(std::array<std::uint32_t, 4> bundled) {
      struct flight_guard_t {
        ~flight_guard_t() {
          g_rebind_in_flight.store(false, std::memory_order_release);
        }
      } flight_guard;

      if (g_stop_requested.load(std::memory_order_acquire)) {
        return;
      }
      if (VDISPLAY::vgd::tracked_session_count() > 0) {
        // A client connected between the startup decision and this
        // worker running. Do not yank the device mid-session; the next
        // service restart repeats the heal.
        BOOST_LOG(warning) << "LuminalVGD devnode: skipping the driver rebind — a virtual "
                              "display session started first. The update completes at the "
                              "next service restart.";
        return;
      }
      // Quiesce our own holds so the device stop cannot be vetoed by us.
      VDISPLAY::vgd::remove_all_virtual_displays();
      VDISPLAY::vgd::close_device();

      bool rebound = false;
      if (const auto inf = bundled_inf_path(); inf) {
        // Preferred: in-place update against the now-idle device — same
        // call the MSI makes, but nothing holds the device open here.
        const auto res = update_driver_in_place(*inf);
        if (res.ok && !res.reboot_required) {
          rebound = true;
        } else if (res.ok && res.reboot_required) {
          BOOST_LOG(warning) << "LuminalVGD devnode: in-place rebind still requires a reboot; "
                             << "falling back to devnode recreate.";
        }
      }
      if (g_stop_requested.load(std::memory_order_acquire)) {
        BOOST_LOG(warning) << "LuminalVGD devnode: rebind interrupted by shutdown; the update "
                              "completes at the next service start.";
        return;
      }
      if (!rebound) {
        rebound = rebind_via_recreate();
      }
      if (!rebound) {
        BOOST_LOG(error) << "LuminalVGD devnode: could not rebind to the bundled driver. "
                         << "The previous driver keeps running; a reboot will complete "
                         << "the update.";
        return;
      }

      // The device restarts asynchronously after either rebind flavor —
      // give it a bounded window before judging the outcome.
      if (auto instance = find_device_instance(true); instance) {
        wait_device_started(*instance, std::chrono::milliseconds(10000));
      }

      const auto installed = platf::diag::query_virtual_display_driver_info().version;
      const auto installed_tuple = installed ? parse_version_tuple(*installed) : std::nullopt;
      if (installed_tuple && *installed_tuple == bundled) {
        BOOST_LOG(info) << "LuminalVGD devnode: driver switched to " << *installed
                        << " without a reboot.";
        // One handshake proves the new driver actually runs, then the
        // handle is dropped — an idle service must not pin the device.
        if (VDISPLAY::vgd::driver_ready()) {
          if (auto v = VDISPLAY::vgd::driver_version_string(); v) {
            BOOST_LOG(info) << "LuminalVGD devnode: post-rebind handshake OK (" << *v << ").";
          }
        }
        VDISPLAY::vgd::close_device();
      } else {
        BOOST_LOG(warning) << "LuminalVGD devnode: rebind completed but the installed driver "
                           << "reports " << (installed ? *installed : std::string("<unknown>"))
                           << " instead of the bundled " << tuple_to_string(bundled)
                           << ". A reboot may still be required.";
      }
    }

    void heal_version_mismatch() {
      const auto bundled_pkg = platf::diag::query_bundled_vgd_package();
      if (!bundled_pkg.present || !bundled_pkg.version) {
        return;  // portable install without a bundled package — nothing to compare
      }
      const auto bundled = parse_version_tuple(*bundled_pkg.version);
      const auto installed_str = platf::diag::query_virtual_display_driver_info().version;
      const auto installed = installed_str ? parse_version_tuple(*installed_str) : std::nullopt;
      if (!bundled || !installed) {
        return;
      }
      if (*installed == *bundled) {
        return;
      }
      if (*installed > *bundled) {
        // Dev box running a newer script-installed build than the MSI
        // bundle. Never downgrade it automatically. Full-tuple compare,
        // so a future versioning-scheme change stays honest as long as
        // the leading fields grow.
        BOOST_LOG(info) << "LuminalVGD devnode: installed driver " << *installed_str
                        << " is newer than the bundled " << *bundled_pkg.version
                        << "; leaving it in place.";
        return;
      }
      if (tdr::stack_down()) {
        // A driver install against a wedged WDDM stack can hang
        // indefinitely; the wedge needs a reboot anyway, which also
        // completes the update the boring way.
        BOOST_LOG(warning) << "LuminalVGD devnode: bundled driver " << *bundled_pkg.version
                           << " is newer than installed " << *installed_str
                           << ", but the display stack is down — deferring the rebind "
                           << "to the post-reboot start.";
        return;
      }

      BOOST_LOG(warning) << "LuminalVGD devnode: installed driver " << *installed_str
                         << " is older than the bundled " << *bundled_pkg.version
                         << " — rebinding to the new driver without a reboot (background).";
      g_rebind_in_flight.store(true, std::memory_order_release);
      g_rebind_thread = std::thread(&rebind_worker, *bundled);
    }

    void ensure_and_heal_once() {
      // 1. Make sure a device exists at all.
      auto present = find_device_instance(true);
      if (present) {
        g_device_expected.store(true, std::memory_order_release);
        BOOST_LOG(debug) << "LuminalVGD devnode: adopting present device "
                         << platf::to_utf8(*present) << ".";
      } else {
        if (!driver_store_has_package()) {
          // Nothing staged in the DriverStore: creating a DriverRequired
          // software device could never start it. Portable users run the
          // install script once, exactly as before this module existed.
          if (bundled_driver_build()) {
            BOOST_LOG(warning) << "LuminalVGD devnode: a driver package is bundled but not "
                                  "staged in the DriverStore; run drivers\\luminalvgd\\"
                                  "install.ps1 (or the installer) once to stage it.";
          }
          return;
        }
        auto created = create_software_device();
        if (!created) {
          BOOST_LOG(error) << "LuminalVGD devnode: no device present and software-device "
                           << "creation failed; virtual displays will be unavailable.";
          return;
        }
        g_device_expected.store(true, std::memory_order_release);
        if (!wait_device_started(*created, std::chrono::milliseconds(20000))) {
          // Not fatal: backend selection sees device_expected() and
          // keeps re-probing instead of latching NONE.
          BOOST_LOG(warning) << "LuminalVGD devnode: device created but not started yet; "
                             << "backend selection will keep probing for it.";
        }
      }

      // 2. Heal a stale binding (upgrade staged a newer package).
      heal_version_mismatch();
    }
  }  // namespace

  std::optional<std::uint32_t> bundled_driver_build() {
    const auto pkg = platf::diag::query_bundled_vgd_package();
    if (!pkg.present || !pkg.version) {
      return std::nullopt;
    }
    // DriverVer versions are four numeric fields, "x.y.z.BUILD".
    const std::string &v = *pkg.version;
    const auto last_dot = v.find_last_of('.');
    if (last_dot == std::string::npos || last_dot + 1 >= v.size()) {
      return std::nullopt;
    }
    try {
      return static_cast<std::uint32_t>(std::stoul(v.substr(last_dot + 1)));
    } catch (...) {
      return std::nullopt;
    }
  }

  bool device_expected() {
    return g_device_expected.load(std::memory_order_acquire);
  }

  bool rebind_in_flight() {
    return g_rebind_in_flight.load(std::memory_order_acquire);
  }

  void shutdown_join() {
    g_stop_requested.store(true, std::memory_order_release);
    if (!g_rebind_thread.joinable()) {
      return;
    }
    // Bounded: std::thread has no timed join, so poll the in-flight flag
    // (cleared by the worker's scope guard just before it returns). 2 s
    // keeps us well inside the 10 s force-shutdown watchdog. A worker
    // parked inside the unbounded UpdateDriver call is detached with a
    // warning — the residual exposure is only that one blocking API
    // slice, and the update completes at the next service start.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_rebind_in_flight.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_rebind_in_flight.load(std::memory_order_acquire)) {
      g_rebind_thread.join();
      return;
    }
    BOOST_LOG(warning) << "LuminalVGD devnode: shutting down with the driver rebind still in "
                          "flight; detaching the worker. The update completes at the next "
                          "service start.";
    g_rebind_thread.detach();
  }

  void startup_ensure_and_heal() {
    static std::once_flag once;
    std::call_once(once, []() {
      try {
        ensure_and_heal_once();
      } catch (...) {
        BOOST_LOG(error) << "LuminalVGD devnode: startup ensure/heal threw; continuing without it.";
      }
    });
  }

}  // namespace platf::vgd_devnode
