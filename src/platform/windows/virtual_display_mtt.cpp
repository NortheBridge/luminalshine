/**
 * @file src/platform/windows/virtual_display_mtt.cpp
 * @brief MTT VDD backend implementation.
 *
 * Talks to MikeTheTech's Virtual Display Driver via:
 *   - settings XML at `%ProgramData%\LuminalShine\vdd_settings.xml`
 *     (VDDPATH redirected via `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH`).
 *   - the named pipe `\\.\pipe\MTTVirtualDisplayPipe` (commands: RELOAD_DRIVER,
 *     SETDISPLAYCOUNT, SETGPU, PING).
 *
 * Multi-client model: MTT does not support per-client GUID-tagged displays.
 * Every concurrent Moonlight client shares the same MTT display, configured
 * by whichever connector arrived first. Subsequent connectors get the
 * existing mode and an info-level log explaining the fallback.
 */

#include "src/platform/windows/virtual_display_mtt.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shlobj.h>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")

namespace VDISPLAY::mtt {

  namespace {
    constexpr const wchar_t *kPipeName = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
    constexpr const wchar_t *kRegistryKey = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
    constexpr const wchar_t *kRegistryValueVddPath = L"VDDPATH";
    constexpr const wchar_t *kProgramDataSubdir = L"LuminalShine";
    constexpr const wchar_t *kSettingsFileName = L"vdd_settings.xml";
    /// MTT's hardware ID (root-enumerated). Used to detect installation.
    constexpr const wchar_t *kHardwareIdLower = L"root\\mttvdd";

    struct TrackedDisplay {
      GUID guid {};
      std::string client_uid;
      std::string client_name;
      std::wstring display_name;     ///< e.g. "\\\\.\\DISPLAY7"
      std::string device_id;         ///< e.g. "{abcd1234-...}"
      std::wstring monitor_device_path;
      std::chrono::steady_clock::time_point ready_since {};
    };

    struct State {
      std::mutex mutex;
      std::atomic<bool> initialized {false};
      std::atomic<bool> watchdog_feeding {true};
      std::atomic<bool> ping_thread_running {false};
      std::atomic<bool> ping_thread_stop {false};
      std::thread ping_thread;
      std::function<void()> on_ping_failure;
      std::vector<TrackedDisplay> tracked;  ///< Currently active displays.
      std::wstring program_data_dir;        ///< Resolved %ProgramData%\LuminalShine.
      std::wstring settings_path;           ///< program_data_dir + kSettingsFileName.
    };

    State &state() {
      static State s;
      return s;
    }

    bool guids_equal(const GUID &a, const GUID &b) {
      return std::memcmp(&a, &b, sizeof(GUID)) == 0;
    }

    std::string guid_to_string(const GUID &guid) {
      wchar_t buf[40] {};
      StringFromGUID2(guid, buf, 40);
      // StringFromGUID2 returns "{...}". Lowercase + utf-8 narrow.
      std::wstring w(buf);
      for (auto &c : w) {
        c = (wchar_t) towlower(c);
      }
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (n <= 0) {
        return {};
      }
      std::string out(n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
      return out;
    }

    std::string wide_to_utf8(const std::wstring &w) {
      if (w.empty()) {
        return {};
      }
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (n <= 0) {
        return {};
      }
      std::string out(n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
      return out;
    }

    std::wstring utf8_to_wide(const std::string &s) {
      if (s.empty()) {
        return {};
      }
      const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
      if (n <= 0) {
        return {};
      }
      std::wstring out(n - 1, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
      return out;
    }

    std::wstring resolve_program_data_dir() {
      PWSTR program_data_raw = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &program_data_raw))) {
        if (program_data_raw) {
          CoTaskMemFree(program_data_raw);
        }
        return L"C:\\ProgramData\\" + std::wstring(kProgramDataSubdir);
      }
      std::wstring out(program_data_raw);
      CoTaskMemFree(program_data_raw);
      out += L"\\";
      out += kProgramDataSubdir;
      return out;
    }

    bool ensure_directory(const std::wstring &dir) {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(dir), ec);
      return !ec;
    }

    bool write_registry_vdd_path(const std::wstring &path) {
      HKEY key = nullptr;
      DWORD disposition = 0;
      LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryKey, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &key, &disposition);
      if (rc != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "MTT VDD: failed to open/create registry key (rc=" << rc << ")";
        return false;
      }
      // Driver expects a trailing backslash on the directory path.
      std::wstring with_slash = path;
      if (with_slash.empty() || with_slash.back() != L'\\') {
        with_slash.push_back(L'\\');
      }
      const auto bytes = static_cast<DWORD>((with_slash.size() + 1) * sizeof(wchar_t));
      rc = RegSetValueExW(key, kRegistryValueVddPath, 0, REG_SZ,
                          reinterpret_cast<const BYTE *>(with_slash.c_str()), bytes);
      RegCloseKey(key);
      if (rc != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "MTT VDD: failed to write VDDPATH registry value (rc=" << rc << ")";
        return false;
      }
      return true;
    }

    /// Minimal settings XML writer. We don't need a full XML library — only
    /// the count and resolutions list change at runtime.
    bool write_settings_xml(const std::wstring &path,
                            int monitor_count,
                            int width, int height, int refresh_rate,
                            const std::wstring &gpu_friendly_name) {
      std::wstringstream ss;
      ss << L"<?xml version='1.0' encoding='utf-8'?>\r\n"
         << L"<!-- Generated by LuminalShine. Do not edit by hand; the file is\r\n"
         << L"     overwritten each time a streaming session begins. -->\r\n"
         << L"<vdd_settings>\r\n"
         << L"    <monitors>\r\n"
         << L"        <count>" << monitor_count << L"</count>\r\n"
         << L"    </monitors>\r\n"
         << L"    <gpu>\r\n"
         << L"        <friendlyname>" << (gpu_friendly_name.empty() ? L"default" : gpu_friendly_name) << L"</friendlyname>\r\n"
         << L"    </gpu>\r\n"
         << L"    <global>\r\n"
         << L"        <g_refresh_rate>" << refresh_rate << L"</g_refresh_rate>\r\n"
         << L"    </global>\r\n"
         << L"    <resolutions>\r\n"
         << L"        <resolution>\r\n"
         << L"            <width>" << width << L"</width>\r\n"
         << L"            <height>" << height << L"</height>\r\n"
         << L"            <refresh_rate>" << refresh_rate << L"</refresh_rate>\r\n"
         << L"        </resolution>\r\n"
         << L"    </resolutions>\r\n"
         << L"    <options>\r\n"
         << L"        <CustomEdid>false</CustomEdid>\r\n"
         << L"        <PreventSpoof>false</PreventSpoof>\r\n"
         << L"        <EdidCeaOverride>false</EdidCeaOverride>\r\n"
         << L"        <HardwareCursor>true</HardwareCursor>\r\n"
         << L"        <SDR10bit>false</SDR10bit>\r\n"
         << L"        <HDRPlus>false</HDRPlus>\r\n"
         << L"        <logging>false</logging>\r\n"
         << L"        <debuglogging>false</debuglogging>\r\n"
         << L"    </options>\r\n"
         << L"</vdd_settings>\r\n";

      const std::wstring xml_w = ss.str();
      std::string xml_utf8 = wide_to_utf8(xml_w);

      std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
      if (!out) {
        BOOST_LOG(warning) << "MTT VDD: failed to open settings XML for write at " << wide_to_utf8(path);
        return false;
      }
      out.write(xml_utf8.data(), static_cast<std::streamsize>(xml_utf8.size()));
      out.close();
      return out.good();
    }

    /// Snapshot the current set of `\\.\DISPLAYn` device names (for diff after reload).
    std::set<std::wstring> snapshot_active_displays() {
      std::set<std::wstring> out;
      DISPLAY_DEVICEW dev {};
      dev.cb = sizeof(dev);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
          out.emplace(dev.DeviceName);
        }
      }
      return out;
    }

    /// Resolve a `\\.\DISPLAYn` device name to a Windows display device id.
    std::optional<std::string> resolve_device_id_from_display_name(const std::wstring &display_name) {
      DISPLAY_DEVICEW monitor {};
      monitor.cb = sizeof(monitor);
      if (!EnumDisplayDevicesW(display_name.c_str(), 0, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) {
        return std::nullopt;
      }
      // DeviceID will be the monitor's interface path; for the "device id" we
      // want the GUID portion that nvhttp/display_helper expect.
      std::wstring id(monitor.DeviceID);
      // Try to find a "{...}" GUID in the path.
      const auto open = id.find(L'{');
      const auto close = id.find(L'}');
      if (open != std::wstring::npos && close != std::wstring::npos && close > open) {
        return wide_to_utf8(id.substr(open, close - open + 1));
      }
      return wide_to_utf8(id);
    }

    /// Returns true if a display monitor (NOT adapter) is attached to the MTT
    /// adapter. Used by enumerate_displays() and resolve_device_id().
    bool is_mtt_owned_display(const std::wstring &display_name) {
      DISPLAY_DEVICEW dev {};
      dev.cb = sizeof(dev);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (display_name != dev.DeviceName) {
          continue;
        }
        // The adapter's DeviceString contains the friendly name; MTT's adapter
        // identifies as "Virtual Display Driver" or similar; safer to also
        // check DeviceID which contains the hardware ID for the adapter.
        std::wstring did(dev.DeviceID);
        std::wstring ds(dev.DeviceString);
        for (auto &c : did) {
          c = (wchar_t) towlower(c);
        }
        for (auto &c : ds) {
          c = (wchar_t) towlower(c);
        }
        if (did.find(L"mttvdd") != std::wstring::npos ||
            ds.find(L"virtual display driver") != std::wstring::npos ||
            ds.find(L"mikethetech") != std::wstring::npos) {
          return true;
        }
        return false;
      }
      return false;
    }

    /// Find newly-appeared displays after a driver reload.
    std::vector<std::wstring> diff_new_displays(const std::set<std::wstring> &before) {
      std::vector<std::wstring> added;
      auto after = snapshot_active_displays();
      for (const auto &name : after) {
        if (!before.count(name) && is_mtt_owned_display(name)) {
          added.push_back(name);
        }
      }
      return added;
    }
  }  // namespace

  bool is_driver_installed() {
    HDEVINFO info_set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
                                             DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (info_set == INVALID_HANDLE_VALUE) {
      return false;
    }
    SP_DEVINFO_DATA devinfo {};
    devinfo.cbSize = sizeof(devinfo);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(info_set, i, &devinfo); ++i) {
      wchar_t hwid[512] {};
      if (!SetupDiGetDeviceRegistryPropertyW(info_set, &devinfo, SPDRP_HARDWAREID, nullptr,
                                             reinterpret_cast<PBYTE>(hwid), sizeof(hwid),
                                             nullptr)) {
        continue;
      }
      std::wstring hw(hwid);
      for (auto &c : hw) {
        c = (wchar_t) towlower(c);
      }
      if (hw.find(L"mttvdd") != std::wstring::npos) {
        found = true;
        break;
      }
    }
    SetupDiDestroyDeviceInfoList(info_set);
    return found;
  }

  bool send_pipe_command(const std::wstring &command, std::wstring *reply,
                         std::chrono::milliseconds timeout) {
    // CreateFile wants WAIT_OBJECT_0 semantics; we use WaitNamedPipe to
    // honor `timeout` for the connect step.
    const DWORD wait_ms = timeout.count() <= 0 ? 100 : static_cast<DWORD>(timeout.count());
    if (!WaitNamedPipeW(kPipeName, wait_ms)) {
      return false;
    }
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD written = 0;
    const DWORD to_write = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    BOOL ok = WriteFile(pipe, command.c_str(), to_write, &written, nullptr);
    if (ok && reply) {
      wchar_t buf[256] {};
      DWORD read_bytes = 0;
      if (ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &read_bytes, nullptr) && read_bytes > 0) {
        reply->assign(buf, read_bytes / sizeof(wchar_t));
      }
    }
    CloseHandle(pipe);
    return ok != FALSE;
  }

  bool reload_driver() {
    return send_pipe_command(L"RELOAD_DRIVER");
  }

  bool is_responsive(std::chrono::milliseconds timeout) {
    std::wstring reply;
    if (!send_pipe_command(L"PING", &reply, timeout)) {
      return false;
    }
    return reply.find(L"PONG") != std::wstring::npos;
  }

  bool set_render_adapter(const std::wstring &adapter_friendly_name) {
    if (adapter_friendly_name.empty()) {
      return false;
    }
    std::wstring command = L"SETGPU \"";
    command += adapter_friendly_name;
    command += L"\"";
    return send_pipe_command(command);
  }

  bool set_render_adapter_with_most_vram() {
    // We delegate VRAM probing to DXGI in `virtual_display.cpp` (existing
    // logic); on this side we just pass the friendly name. Until the wider
    // refactor lands, fall back to a no-op that lets the driver pick its
    // default adapter.
    return send_pipe_command(L"GETALLGPUS");
  }

  bool start_ping_thread(std::function<void()> on_failure) {
    auto &s = state();
    if (s.ping_thread_running.load(std::memory_order_acquire)) {
      return true;
    }
    s.on_ping_failure = std::move(on_failure);
    s.ping_thread_stop.store(false, std::memory_order_release);
    s.ping_thread_running.store(true, std::memory_order_release);
    s.ping_thread = std::thread([] {
      auto &s = state();
      int consecutive_failures = 0;
      // Match SudoVDA cadence: ~5s ping, fail after 3 consecutive misses.
      while (!s.ping_thread_stop.load(std::memory_order_acquire)) {
        if (s.watchdog_feeding.load(std::memory_order_acquire)) {
          if (is_responsive(std::chrono::milliseconds(1500))) {
            consecutive_failures = 0;
          } else {
            ++consecutive_failures;
            if (consecutive_failures >= 3) {
              BOOST_LOG(warning) << "MTT VDD: PING failed 3x; declaring driver unresponsive.";
              auto cb = s.on_ping_failure;
              if (cb) {
                cb();
              }
              break;
            }
          }
        }
        for (int i = 0; i < 50 && !s.ping_thread_stop.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
      s.ping_thread_running.store(false, std::memory_order_release);
    });
    return true;
  }

  void stop_ping_thread() {
    auto &s = state();
    s.ping_thread_stop.store(true, std::memory_order_release);
    if (s.ping_thread.joinable()) {
      s.ping_thread.join();
    }
    s.ping_thread_running.store(false, std::memory_order_release);
  }

  void set_watchdog_feeding_enabled(bool enable) {
    state().watchdog_feeding.store(enable, std::memory_order_release);
  }

  DRIVER_STATUS initialize() {
    auto &s = state();
    {
      std::lock_guard lk(s.mutex);
      if (s.initialized.load(std::memory_order_acquire)) {
        return DRIVER_STATUS::OK;
      }
      s.program_data_dir = resolve_program_data_dir();
      s.settings_path = s.program_data_dir + L"\\" + kSettingsFileName;
      ensure_directory(s.program_data_dir);
      // Point the driver at our managed settings location.
      write_registry_vdd_path(s.program_data_dir);

      // Seed a default settings file if none exists.
      if (!std::filesystem::exists(std::filesystem::path(s.settings_path))) {
        write_settings_xml(s.settings_path, 0, 1920, 1080, 60, L"default");
      }
    }

    if (!is_driver_installed()) {
      BOOST_LOG(warning) << "MTT VDD: driver is not installed.";
      return DRIVER_STATUS::FAILED;
    }

    // Liveness probe; if the pipe isn't up yet, the driver might still be
    // initializing — give it a short retry window.
    bool alive = false;
    for (int i = 0; i < 10 && !alive; ++i) {
      alive = is_responsive(std::chrono::milliseconds(500));
      if (!alive) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
    if (!alive) {
      BOOST_LOG(warning) << "MTT VDD: PING timed out after 10 attempts; driver may not be running.";
      return DRIVER_STATUS::WATCHDOG_FAILED;
    }

    state().initialized.store(true, std::memory_order_release);
    BOOST_LOG(info) << "MTT VDD: backend initialized.";
    return DRIVER_STATUS::OK;
  }

  void shutdown() {
    auto &s = state();
    stop_ping_thread();
    {
      std::lock_guard lk(s.mutex);
      s.tracked.clear();
      s.initialized.store(false, std::memory_order_release);
    }
  }

  std::optional<VirtualDisplayCreationResult> create_display(
    const char *client_uid,
    const char *client_name,
    const char *hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active
  ) {
    (void) hdr_profile;
    (void) base_fps_millihz;
    (void) framegen_refresh_active;
    auto &s = state();
    {
      std::lock_guard lk(s.mutex);
      // If a display is already configured, return the existing one. MTT
      // can't realistically reconfigure mid-session without disrupting other
      // active clients, so we silently share. Log so behavior is auditable.
      if (!s.tracked.empty()) {
        const auto &existing = s.tracked.front();
        BOOST_LOG(info) << "MTT VDD: per-client virtual display unsupported by this backend; "
                        << "client '" << (client_name ? client_name : "") << "' will share the "
                        << "existing virtual display (device_id=" << existing.device_id << ").";
        VirtualDisplayCreationResult shared {};
        shared.display_name = existing.display_name;
        shared.device_id = existing.device_id;
        shared.client_name = client_name ? std::optional<std::string>(client_name) : std::nullopt;
        shared.monitor_device_path = existing.monitor_device_path;
        shared.reused_existing = true;
        shared.ready_since = existing.ready_since;
        return shared;
      }
    }

    // Snapshot present displays so we can detect the new one after reload.
    auto before = snapshot_active_displays();

    // Decide refresh rate: MTT wants integer Hz. fps is mHz/1000 in
    // SudoVDA's contract; mirror that here.
    const int refresh_hz = static_cast<int>(std::max<uint32_t>(1, fps / 1000U));
    if (!write_settings_xml(s.settings_path, 1,
                            static_cast<int>(width), static_cast<int>(height),
                            refresh_hz, L"default")) {
      BOOST_LOG(warning) << "MTT VDD: failed to write settings XML.";
      return std::nullopt;
    }

    if (!reload_driver()) {
      BOOST_LOG(warning) << "MTT VDD: RELOAD_DRIVER pipe command failed.";
      return std::nullopt;
    }

    // Wait up to 5s for the new display to enumerate.
    std::wstring new_display;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      auto added = diff_new_displays(before);
      if (!added.empty()) {
        new_display = added.front();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (new_display.empty()) {
      BOOST_LOG(warning) << "MTT VDD: new display did not appear within 5s of RELOAD_DRIVER.";
      return std::nullopt;
    }

    auto device_id = resolve_device_id_from_display_name(new_display);
    if (!device_id) {
      BOOST_LOG(warning) << "MTT VDD: could not resolve device_id for "
                         << wide_to_utf8(new_display);
      return std::nullopt;
    }

    TrackedDisplay td {};
    td.guid = guid;
    td.client_uid = client_uid ? client_uid : std::string {};
    td.client_name = client_name ? client_name : std::string {};
    td.display_name = new_display;
    td.device_id = *device_id;
    td.ready_since = std::chrono::steady_clock::now();

    {
      std::lock_guard lk(s.mutex);
      s.tracked.push_back(td);
    }

    VirtualDisplayCreationResult out {};
    out.display_name = new_display;
    out.device_id = *device_id;
    out.client_name = client_name ? std::optional<std::string>(client_name) : std::nullopt;
    out.reused_existing = false;
    out.ready_since = td.ready_since;

    BOOST_LOG(info) << "MTT VDD: created virtual display " << wide_to_utf8(new_display)
                    << " (" << *device_id << ") for client '"
                    << (client_name ? client_name : "") << "'.";
    return out;
  }

  bool remove_display(const GUID &guid) {
    auto &s = state();
    bool was_last = false;
    {
      std::lock_guard lk(s.mutex);
      auto it = std::remove_if(s.tracked.begin(), s.tracked.end(),
                               [&](const TrackedDisplay &t) { return guids_equal(t.guid, guid); });
      if (it == s.tracked.end()) {
        return false;
      }
      s.tracked.erase(it, s.tracked.end());
      was_last = s.tracked.empty();
    }

    if (was_last) {
      // Set count=0 and reload so the virtual display goes away. The driver
      // accepts count>=1 only as a positive count; for count=0 we set the
      // XML and reload, which collapses the adapter to no monitors.
      if (!write_settings_xml(s.settings_path, 0, 1920, 1080, 60, L"default")) {
        BOOST_LOG(warning) << "MTT VDD: failed to write count=0 settings during remove.";
      }
      reload_driver();
    }
    return true;
  }

  bool remove_all_displays() {
    auto &s = state();
    {
      std::lock_guard lk(s.mutex);
      s.tracked.clear();
    }
    write_settings_xml(s.settings_path, 0, 1920, 1080, 60, L"default");
    return reload_driver();
  }

  bool is_guid_tracked(const GUID &guid) {
    auto &s = state();
    std::lock_guard lk(s.mutex);
    for (const auto &t : s.tracked) {
      if (guids_equal(t.guid, guid)) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::string> resolve_device_id_for_client(const std::string &client_name) {
    auto &s = state();
    std::lock_guard lk(s.mutex);
    for (const auto &t : s.tracked) {
      if (t.client_name == client_name) {
        return t.device_id;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> resolve_any_device_id() {
    auto &s = state();
    std::lock_guard lk(s.mutex);
    if (s.tracked.empty()) {
      return std::nullopt;
    }
    return s.tracked.front().device_id;
  }

  std::optional<std::string> resolve_device_id(const std::wstring &display_name) {
    if (!is_mtt_owned_display(display_name)) {
      return std::nullopt;
    }
    return resolve_device_id_from_display_name(display_name);
  }

  bool is_mtt_output(const std::string &output_identifier) {
    if (output_identifier.empty()) {
      return false;
    }
    auto &s = state();
    std::lock_guard lk(s.mutex);
    for (const auto &t : s.tracked) {
      if (t.device_id == output_identifier) {
        return true;
      }
      if (wide_to_utf8(t.display_name) == output_identifier) {
        return true;
      }
    }
    return false;
  }

  std::vector<SudaVDADisplayInfo> enumerate_displays() {
    std::vector<SudaVDADisplayInfo> out;
    DISPLAY_DEVICEW dev {};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
      if (!is_mtt_owned_display(dev.DeviceName)) {
        continue;
      }
      SudaVDADisplayInfo info {};
      info.device_name = dev.DeviceName;
      info.friendly_name = dev.DeviceString;
      info.is_active = (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
      // Best-effort dimensions from the current mode.
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(dev.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
        info.width = static_cast<int>(mode.dmPelsWidth);
        info.height = static_cast<int>(mode.dmPelsHeight);
      }
      out.push_back(std::move(info));
    }
    return out;
  }

}  // namespace VDISPLAY::mtt
