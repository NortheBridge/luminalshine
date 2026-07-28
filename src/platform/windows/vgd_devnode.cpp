/**
 * @file src/platform/windows/vgd_devnode.cpp
 * @brief Service-owned LuminalVGD devnode + no-reboot driver switchover
 *        (see vgd_devnode.h for the design).
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
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/windows/diag_info.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/vgd_devnode.h"
#include "src/platform/windows/virtual_display_vgd.h"

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
    /// dies, and re-arrives at the next service start). Never closed
    /// explicitly — closing it yanks the virtual display machinery.
    HSWDEVICE g_sw_device = nullptr;

    // SwDeviceCreate/SwDeviceClose are bound dynamically: MinGW toolchains
    // do not reliably ship an import library for swdevice.dll, and a load
    // failure must degrade to "device not created" rather than a missing-
    // DLL process death.
    // The DEVPROPERTY array parameter is typed void* here: we always pass
    // nullptr/0, and MinGW's devpropdef availability varies.
    using SwDeviceCreate_t = HRESULT(WINAPI *)(PCWSTR, PCWSTR, const SW_DEVICE_CREATE_INFO *, ULONG, const void *, SW_DEVICE_CREATE_CALLBACK, PVOID, PHSWDEVICE);
    using SwDeviceClose_t = VOID(WINAPI *)(HSWDEVICE);

    struct swdevice_api_t {
      SwDeviceCreate_t create {nullptr};
      SwDeviceClose_t close {nullptr};
    };

    const swdevice_api_t &swdevice_api() {
      static const swdevice_api_t api = []() {
        swdevice_api_t out {};
        HMODULE mod = LoadLibraryW(L"swdevice.dll");
        if (!mod) {
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
      if (!api.create) {
        BOOST_LOG(warning) << "LuminalVGD devnode: SwDeviceCreate is unavailable on this system.";
        return std::nullopt;
      }
      if (g_sw_device) {
        // A previous create in this process is still alive; close it so
        // the re-create below starts clean (rebind path).
        if (api.close) {
          api.close(g_sw_device);
        }
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
      // exists (driver start continues asynchronously after that).
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
        if (api.close) {
          api.close(device);
        }
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

    /// Full rebind: remove the devnode and recreate it as a software
    /// device — a brand-new device instance runs full driver ranking, so
    /// the newest staged package always wins. Also migrates legacy
    /// persistent (installer-created) devnodes to service ownership.
    bool rebind_via_recreate() {
      if (auto stale = find_device_instance(false); stale) {
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

    void heal_version_mismatch() {
      const auto bundled = bundled_driver_build();
      if (!bundled) {
        return;  // portable install without a bundled package — nothing to compare
      }
      const auto running = VDISPLAY::vgd::driver_build();
      if (!running) {
        return;  // no handshake — diagnostics elsewhere cover this
      }
      if (*running == *bundled) {
        return;
      }
      if (*running > *bundled) {
        // Dev box running a newer script-installed build than the MSI
        // bundle. Never downgrade it automatically.
        BOOST_LOG(info) << "LuminalVGD devnode: running driver build " << *running
                        << " is newer than the bundled build " << *bundled
                        << "; leaving it in place.";
        return;
      }

      BOOST_LOG(warning) << "LuminalVGD devnode: running driver build " << *running
                         << " is older than the bundled build " << *bundled
                         << " — rebinding to the new driver without a reboot.";

      // Quiesce: no sessions exist this early in startup, but be thorough —
      // the device stop must not be vetoed by our own handles.
      VDISPLAY::vgd::remove_all_virtual_displays();
      VDISPLAY::vgd::close_device();

      bool rebound = false;
      if (const auto inf = bundled_inf_path(); inf) {
        // Preferred: in-place update against the now-idle device. This is
        // the same call the MSI makes, but from here nothing holds the
        // device open, so the stop is not vetoed and no reboot is flagged.
        const auto res = update_driver_in_place(*inf);
        if (res.ok && !res.reboot_required) {
          rebound = true;
        } else if (res.ok && res.reboot_required) {
          BOOST_LOG(warning) << "LuminalVGD devnode: in-place rebind still requires a reboot; "
                             << "falling back to devnode recreate.";
        }
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

      const auto now_running = VDISPLAY::vgd::driver_build();
      if (now_running && *now_running == *bundled) {
        BOOST_LOG(info) << "LuminalVGD devnode: driver switched to build " << *now_running
                        << " without a reboot.";
      } else {
        BOOST_LOG(warning) << "LuminalVGD devnode: rebind completed but the driver reports build "
                           << (now_running ? std::to_string(*now_running) : std::string("<no handshake>"))
                           << " instead of the bundled " << *bundled
                           << ". A reboot may still be required.";
      }
    }

    void ensure_and_heal_once() {
      // 1. Make sure a device exists at all.
      auto present = find_device_instance(true);
      if (present) {
        BOOST_LOG(debug) << "LuminalVGD devnode: adopting present device "
                         << platf::to_utf8(*present) << ".";
      } else {
        // No present devnode. Fresh MSI installs stage the driver package
        // only; the phantom of our own previous software device (if any)
        // re-arrives under the same instance id.
        const bool package_staged = bundled_driver_build().has_value() || platf::diag::query_virtual_display_driver_info().version.has_value();
        if (!package_staged) {
          // Nothing bundled and no installed driver either — behave as
          // before this module existed (backend selection reports the
          // driver missing).
          return;
        }
        auto created = create_software_device();
        if (!created) {
          BOOST_LOG(error) << "LuminalVGD devnode: no device present and software-device "
                           << "creation failed; virtual displays will be unavailable.";
          return;
        }
        if (!wait_device_started(*created, std::chrono::milliseconds(20000))) {
          BOOST_LOG(warning) << "LuminalVGD devnode: device created but not started yet; "
                             << "continuing (the driver may finish starting shortly).";
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
