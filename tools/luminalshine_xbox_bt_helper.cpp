/**
 * @file tools/luminalshine_xbox_bt_helper.cpp
 * @brief LuminalShine Xbox Bluetooth controller reconnect helper service.
 *
 * Independent Windows service (separate from LuminalShineService) that
 * watches for Bluetooth Xbox Series X/S controller arrival events and
 * briefly plugs a ViGEmBus virtual X360 pad to force Steam's XInput
 * hotplug path to re-enumerate. Steam can otherwise take a substantial
 * period to notice a real BT controller has come back, even though
 * Windows and the Xbox Accessories app see it immediately.
 *
 * Runs in Session 0 as LocalSystem. HID device-interface arrivals fire
 * system-wide and reach a service via SERVICE_CONTROL_DEVICEEVENT with
 * DEVICE_NOTIFY_SERVICE_HANDLE registration. ViGEmBus is a kernel-mode
 * driver, so virtual pads attached from Session 0 are visible to all
 * user-session XInput consumers (including Steam).
 *
 * Crash-isolated from LuminalShineService and with no IPC between the
 * two: a Steam-hotplug regression in this helper cannot affect the
 * streaming host. Disabled by default; users opt in by setting
 * `enabled: true` in luminalshine_xboxcontroller_state.json.
 */

#define WIN32_LEAN_AND_MEAN
// INITGUID must be defined BEFORE any header that pulls in guiddef.h
// (Windows.h does). Without it, DEFINE_GUID() below expands to an
// `extern const GUID` declaration and emits no storage — the
// `.refptr.GUID_DEVINTERFACE_HID_LOCAL` reference at the device-arrival
// filter registration site then fails to link with "undefined symbol"
// on the MSYS2 UCRT64 / clang + lld toolchain used by ci-windows.yml.
// With INITGUID, the same macro lays down the actual `const GUID` =
// {...} initializer in this TU and the link resolves cleanly.
#define INITGUID
#include <Windows.h>
#include <Dbt.h>
#include <ShlObj.h>
// Explicit <KnownFolders.h> because mingw-w64's ShlObj.h doesn't pull
// it in transitively (the Microsoft SDK does, but the MSYS2 UCRT64
// headers we build under don't), and toggling INITGUID flipped a
// guard in the header chain that previously was masking the
// dependency. FOLDERID_ProgramData (used at SHGetKnownFolderPath
// below) is declared by DEFINE_KNOWN_FOLDER in this header; without
// it the compile fails with "use of undeclared identifier".
#include <KnownFolders.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ViGEm/Client.h>
#include <nlohmann/json.hpp>

// GUID_DEVINTERFACE_HID, defined inline so the helper compiles cleanly on
// toolchains where <hidclass.h> isn't directly importable from a service
// build. Value matches the Windows SDK: {4D1E55B2-F16F-11CF-88CB-001111000030}.
DEFINE_GUID(GUID_DEVINTERFACE_HID_LOCAL,
            0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);

namespace {

  constexpr char    kServiceName[]    = "LuminalShineXboxBtHelper";
  constexpr wchar_t kSingletonName[]  = L"Local\\LuminalShineXboxBtHelperSingleton";
  constexpr wchar_t kLogFileName[]    = L"luminalshine_xbox_bt_helper.log";
  constexpr wchar_t kConfigFileName[] = L"luminalshine_xboxcontroller_state.json";

  // Default match list. Microsoft VID 0x045E plus the BLE-mode PIDs that
  // ship on Xbox Series X|S and recent Xbox Wireless controllers, and the
  // older Xbox One Bluetooth PID. Aftermarket pads (e.g. 8BitDo) that
  // exhibit the same Steam-hotplug delay can be added via the JSON file.
  struct match_t {
    uint16_t vid;
    uint16_t pid;
  };

  struct config_t {
    bool                 enabled           = false;
    unsigned             debounce_ms       = 500;
    unsigned             nudge_duration_ms = 250;
    bool                 log_each_nudge    = true;
    std::vector<match_t> match_vid_pid {
      {0x045E, 0x0B13},
      {0x045E, 0x0B20},
      {0x045E, 0x02FD},
    };
  };

  SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
  SERVICE_STATUS        g_service_status        = {};
  HANDLE                g_stop_event            = nullptr;
  HDEVNOTIFY            g_device_notify         = nullptr;
  HANDLE                g_config_watch_handle   = INVALID_HANDLE_VALUE;
  FILE                 *g_log_file              = nullptr;
  std::mutex            g_log_mtx;
  std::mutex            g_config_mtx;
  config_t              g_config;
  std::mutex            g_nudge_mtx;
  std::chrono::steady_clock::time_point g_last_nudge_ts {};

  // ------------------------------------------------------------------- logging

  void log_line(const char *level, const std::string &msg) {
    std::lock_guard<std::mutex> guard(g_log_mtx);
    if (!g_log_file) {
      return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(
      g_log_file,
      "%04u-%02u-%02uT%02u:%02u:%02u.%03u %s %s\n",
      st.wYear, st.wMonth, st.wDay,
      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      level, msg.c_str());
    std::fflush(g_log_file);
  }

  void log_info(const std::string &m) {
    log_line("INFO ", m);
  }

  void log_warn(const std::string &m) {
    log_line("WARN ", m);
  }

  void log_err(const std::string &m) {
    log_line("ERROR", m);
  }

  std::string narrow(const std::wstring &w) {
    if (w.empty()) {
      return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), out.data(), n, nullptr, nullptr);
    return out;
  }

  std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
  }

  std::string vid_pid_str(uint16_t vid, uint16_t pid) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "VID_%04X&PID_%04X", vid, pid);
    return buf;
  }

  // ------------------------------------------------------------- path lookup

  std::filesystem::path program_data_path() {
    PWSTR pd = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &pd)) || !pd) {
      return {};
    }
    std::filesystem::path p {pd};
    CoTaskMemFree(pd);
    return p;
  }

  std::filesystem::path config_dir_path() {
    auto root = program_data_path();
    if (root.empty()) {
      return {};
    }
    return root / L"LuminalShine" / L"config";
  }

  std::filesystem::path config_file_path() {
    auto dir = config_dir_path();
    if (dir.empty()) {
      return {};
    }
    return dir / kConfigFileName;
  }

  std::filesystem::path log_file_path() {
    // %ProgramData%\LuminalShine\logs\luminalshine_xbox_bt_helper.log so
    // diagnostics sit alongside the rest of LuminalShine's logs instead of
    // polluting C:\Windows\Temp. Falls back to %TEMP% only if the known-
    // folder lookup fails, which would be a very unusual host state.
    auto root = program_data_path();
    if (!root.empty()) {
      return root / L"LuminalShine" / L"logs" / kLogFileName;
    }
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(_countof(tmp), tmp) == 0) {
      return {};
    }
    return std::filesystem::path {tmp} / kLogFileName;
  }

  // ------------------------------------------------------------ config I/O

  uint16_t parse_hex_or_int_field(const nlohmann::json &v) {
    if (v.is_number_integer()) {
      return static_cast<uint16_t>(v.get<int>());
    }
    if (v.is_string()) {
      try {
        return static_cast<uint16_t>(std::stoul(v.get<std::string>(), nullptr, 0));
      } catch (...) {
        return 0;
      }
    }
    return 0;
  }

  bool load_config(config_t &out) {
    auto path = config_file_path();
    if (path.empty()) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      // No file -> service idles with built-in defaults (enabled=false).
      out = config_t {};
      return true;
    }
    try {
      std::ifstream in(path);
      nlohmann::json j;
      in >> j;
      config_t cfg;
      cfg.enabled           = j.value("enabled", false);
      cfg.debounce_ms       = j.value("debounce_ms", 500u);
      cfg.nudge_duration_ms = j.value("nudge_duration_ms", 250u);
      cfg.log_each_nudge    = j.value("log_each_nudge", true);
      cfg.match_vid_pid.clear();
      if (j.contains("match_vid_pid") && j["match_vid_pid"].is_array()) {
        for (const auto &entry : j["match_vid_pid"]) {
          uint16_t vid = entry.contains("vid") ? parse_hex_or_int_field(entry["vid"]) : 0;
          uint16_t pid = entry.contains("pid") ? parse_hex_or_int_field(entry["pid"]) : 0;
          if (vid && pid) {
            cfg.match_vid_pid.push_back({vid, pid});
          }
        }
      }
      // An empty match list would silently disable the helper without the
      // user realizing why. Fall back to the built-in defaults instead.
      if (cfg.match_vid_pid.empty()) {
        cfg.match_vid_pid = config_t {}.match_vid_pid;
      }
      out = std::move(cfg);
      return true;
    } catch (const std::exception &e) {
      log_warn("config: failed to parse " + narrow(path.wstring()) + " (" + e.what()
               + "); keeping previously loaded config.");
      return false;
    }
  }

  void write_default_config_if_missing() {
    auto path = config_file_path();
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    nlohmann::json j;
    j["enabled"]           = false;
    j["debounce_ms"]       = 500;
    j["nudge_duration_ms"] = 250;
    j["log_each_nudge"]    = true;
    j["match_vid_pid"]     = nlohmann::json::array({
      {{"vid", "0x045E"}, {"pid", "0x0B13"}},
      {{"vid", "0x045E"}, {"pid", "0x0B20"}},
      {{"vid", "0x045E"}, {"pid", "0x02FD"}},
    });
    try {
      std::ofstream out(path);
      out << j.dump(2);
    } catch (...) {
      // Best-effort: the service still runs with built-in defaults when the
      // file is missing.
    }
  }

  // -------------------------------------------------- device-path parsing

  std::wstring upper_copy(std::wstring s) {
    for (auto &c : s) {
      c = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
    }
    return s;
  }

  // Extracts the 4-hex-digit value following one of several possible
  // prefixes that Windows uses in HID device paths. The HID class driver
  // typically reports `VID_xxxx&PID_yyyy` for both USB and Bluetooth
  // controllers, but some Windows versions / BLE service paths produce
  // `VID&xxxx` / `PID&yyyy` instead. Try both.
  uint16_t extract_hex_after(const std::wstring &up, std::initializer_list<const wchar_t *> prefixes) {
    for (auto prefix : prefixes) {
      auto pos = up.find(prefix);
      if (pos == std::wstring::npos) {
        continue;
      }
      pos += std::wcslen(prefix);
      if (pos + 4 > up.size()) {
        continue;
      }
      try {
        return static_cast<uint16_t>(std::stoul(up.substr(pos, 4), nullptr, 16));
      } catch (...) {
        continue;
      }
    }
    return 0;
  }

  std::pair<uint16_t, uint16_t> parse_vid_pid(const std::wstring &device_path) {
    auto up = upper_copy(device_path);
    uint16_t vid = extract_hex_after(up, {L"VID_", L"VID&"});
    uint16_t pid = extract_hex_after(up, {L"PID_", L"PID&"});
    return {vid, pid};
  }

  bool matches_configured_controller(uint16_t vid, uint16_t pid) {
    std::lock_guard<std::mutex> guard(g_config_mtx);
    for (const auto &m : g_config.match_vid_pid) {
      if (m.vid == vid && m.pid == pid) {
        return true;
      }
    }
    return false;
  }

  // --------------------------------------------------------- ViGEm nudge

  // Plugs a virtual X360 pad, waits nudge_duration_ms, unplugs. Steam's
  // XInput hotplug path scans all real slots whenever any XInput device
  // appears; the virtual pad's appearance is enough to trigger that scan,
  // which then picks up the real Bluetooth controller. Runs on a detached
  // thread because the SCM control handler must return promptly.
  void perform_nudge() {
    unsigned hold_ms;
    bool     log_each;
    {
      std::lock_guard<std::mutex> guard(g_config_mtx);
      hold_ms  = g_config.nudge_duration_ms;
      log_each = g_config.log_each_nudge;
    }

    PVIGEM_CLIENT client = vigem_alloc();
    if (!client) {
      log_err("nudge: vigem_alloc returned null");
      return;
    }

    VIGEM_ERROR status = vigem_connect(client);
    if (!VIGEM_SUCCESS(status)) {
      log_warn("nudge: vigem_connect failed (" + hex8(static_cast<uint32_t>(status))
               + "); ViGEmBus is likely not installed.");
      vigem_free(client);
      return;
    }

    PVIGEM_TARGET target = vigem_target_x360_alloc();
    if (!target) {
      log_err("nudge: vigem_target_x360_alloc returned null");
      vigem_disconnect(client);
      vigem_free(client);
      return;
    }

    status = vigem_target_add(client, target);
    if (!VIGEM_SUCCESS(status)) {
      log_warn("nudge: vigem_target_add failed (" + hex8(static_cast<uint32_t>(status)) + ")");
      vigem_target_free(target);
      vigem_disconnect(client);
      vigem_free(client);
      return;
    }

    if (log_each) {
      log_info("nudge: virtual X360 attached for " + std::to_string(hold_ms) + " ms");
    }

    Sleep(hold_ms);

    status = vigem_target_remove(client, target);
    if (!VIGEM_SUCCESS(status)) {
      log_warn("nudge: vigem_target_remove failed (" + hex8(static_cast<uint32_t>(status)) + ")");
    }
    vigem_target_free(target);
    vigem_disconnect(client);
    vigem_free(client);

    if (log_each) {
      log_info("nudge: complete");
    }
  }

  void maybe_nudge_debounced() {
    unsigned debounce_ms;
    {
      std::lock_guard<std::mutex> g(g_config_mtx);
      if (!g_config.enabled) {
        return;
      }
      debounce_ms = g_config.debounce_ms;
    }
    {
      std::lock_guard<std::mutex> g(g_nudge_mtx);
      auto now  = std::chrono::steady_clock::now();
      auto last = g_last_nudge_ts;
      if (last.time_since_epoch().count() != 0 &&
          (now - last) < std::chrono::milliseconds(debounce_ms)) {
        return;
      }
      g_last_nudge_ts = now;
    }
    std::thread([] {
      perform_nudge();
    }).detach();
  }

  // ----------------------------------------------------- config watcher

  // Watches the config directory for file changes and reloads g_config on
  // each one. ReadDirectoryChangesW is cheaper and lower-latency than
  // polling; the wakeup at service stop happens via closing the directory
  // handle, which causes ReadDirectoryChangesW to return FALSE immediately.
  void config_watcher_thread() {
    auto dir = config_dir_path();
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    g_config_watch_handle = CreateFileW(
      dir.wstring().c_str(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
    if (g_config_watch_handle == INVALID_HANDLE_VALUE) {
      log_warn("config watcher: failed to open " + narrow(dir.wstring())
               + " (last error " + std::to_string(GetLastError()) + ")");
      return;
    }

    std::vector<unsigned char> buf(4096);

    for (;;) {
      DWORD bytes = 0;
      BOOL ok = ReadDirectoryChangesW(
        g_config_watch_handle,
        buf.data(),
        static_cast<DWORD>(buf.size()),
        FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME
          | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
        &bytes,
        nullptr,
        nullptr);
      if (!ok) {
        break;  // handle closed during shutdown, or unrecoverable error
      }
      config_t fresh;
      if (load_config(fresh)) {
        bool enabled_now;
        {
          std::lock_guard<std::mutex> g(g_config_mtx);
          g_config    = std::move(fresh);
          enabled_now = g_config.enabled;
        }
        log_info(std::string("config: reloaded (enabled=") + (enabled_now ? "true" : "false") + ")");
      }
    }
  }

  // -------------------------------------------------------- SCM plumbing

  DWORD WINAPI HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID /*lpContext*/) {
    switch (dwControl) {
      case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;

      case SERVICE_CONTROL_STOP:
      case SERVICE_CONTROL_PRESHUTDOWN:
        g_service_status.dwCurrentState     = SERVICE_STOP_PENDING;
        g_service_status.dwControlsAccepted = 0;
        g_service_status.dwWaitHint         = 5000;
        SetServiceStatus(g_service_status_handle, &g_service_status);
        if (g_stop_event) {
          SetEvent(g_stop_event);
        }
        return NO_ERROR;

      case SERVICE_CONTROL_DEVICEEVENT:
        if (dwEventType == DBT_DEVICEARRIVAL && lpEventData) {
          auto *hdr = reinterpret_cast<DEV_BROADCAST_HDR *>(lpEventData);
          if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            auto *di = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W *>(lpEventData);
            // dbcc_name is variable-length and null-terminated; the struct
            // declares it as [1] but the kernel writes the full path inline
            // through dbcc_size. Constructing from the pointer is safe.
            std::wstring path(di->dbcc_name);
            auto [vid, pid] = parse_vid_pid(path);
            if (vid && pid && matches_configured_controller(vid, pid)) {
              log_info("device arrival: " + vid_pid_str(vid, pid) + " -- scheduling nudge");
              maybe_nudge_debounced();
            }
          }
        }
        return NO_ERROR;

      default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
  }

  void register_device_notifications() {
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = GUID_DEVINTERFACE_HID_LOCAL;

    g_device_notify = RegisterDeviceNotificationW(
      g_service_status_handle,
      &filter,
      DEVICE_NOTIFY_SERVICE_HANDLE);
    if (!g_device_notify) {
      log_err("RegisterDeviceNotificationW failed; HID arrival events will not be received (last error "
              + std::to_string(GetLastError()) + ")");
    } else {
      log_info("Registered for GUID_DEVINTERFACE_HID arrival notifications");
    }
  }

  void unregister_device_notifications() {
    if (g_device_notify) {
      UnregisterDeviceNotification(g_device_notify);
      g_device_notify = nullptr;
    }
  }

  void open_log_file() {
    auto p = log_file_path();
    if (p.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    // Truncate on each service start. The log is a tail used for diagnosing
    // a misbehaving nudge path, not a permanent record -- a fresh log per
    // start is preferable to an unbounded file in ProgramData. If long-term
    // retention is wanted later, switch to append + rotation.
    FILE *f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"w") == 0) {
      std::lock_guard<std::mutex> guard(g_log_mtx);
      g_log_file = f;
    }
  }

  void close_log_file() {
    std::lock_guard<std::mutex> guard(g_log_mtx);
    if (g_log_file) {
      std::fclose(g_log_file);
      g_log_file = nullptr;
    }
  }

  VOID WINAPI ServiceMain(DWORD /*dwArgc*/, LPSTR * /*lpszArgv*/) {
    g_service_status_handle = RegisterServiceCtrlHandlerExA(kServiceName, HandlerEx, nullptr);
    if (!g_service_status_handle) {
      ExitProcess(GetLastError());
    }

    g_service_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwServiceSpecificExitCode = 0;
    g_service_status.dwWin32ExitCode           = NO_ERROR;
    g_service_status.dwWaitHint                = 5000;
    g_service_status.dwControlsAccepted        = 0;
    g_service_status.dwCheckPoint              = 0;
    g_service_status.dwCurrentState            = SERVICE_START_PENDING;
    SetServiceStatus(g_service_status_handle, &g_service_status);

    open_log_file();
    log_info("LuminalShine Xbox Bluetooth Helper starting");

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
      g_service_status.dwWin32ExitCode = GetLastError();
      g_service_status.dwCurrentState  = SERVICE_STOPPED;
      SetServiceStatus(g_service_status_handle, &g_service_status);
      close_log_file();
      return;
    }

    write_default_config_if_missing();
    {
      config_t fresh;
      if (load_config(fresh)) {
        std::lock_guard<std::mutex> g(g_config_mtx);
        g_config = std::move(fresh);
      }
    }
    {
      std::lock_guard<std::mutex> g(g_config_mtx);
      log_info(std::string("config loaded (enabled=") + (g_config.enabled ? "true" : "false")
               + ", debounce_ms=" + std::to_string(g_config.debounce_ms)
               + ", nudge_duration_ms=" + std::to_string(g_config.nudge_duration_ms)
               + ", patterns=" + std::to_string(g_config.match_vid_pid.size()) + ")");
    }

    register_device_notifications();

    std::thread watcher(config_watcher_thread);

    g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
    g_service_status.dwCurrentState     = SERVICE_RUNNING;
    SetServiceStatus(g_service_status_handle, &g_service_status);

    WaitForSingleObject(g_stop_event, INFINITE);

    log_info("LuminalShine Xbox Bluetooth Helper stopping");
    unregister_device_notifications();

    // Closing the directory handle unblocks the watcher thread's
    // ReadDirectoryChangesW call so it can exit promptly.
    if (g_config_watch_handle != INVALID_HANDLE_VALUE) {
      HANDLE h               = g_config_watch_handle;
      g_config_watch_handle  = INVALID_HANDLE_VALUE;
      CloseHandle(h);
    }
    if (watcher.joinable()) {
      watcher.join();
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    g_service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_service_status_handle, &g_service_status);
    close_log_file();
  }

}  // namespace

int main(int /*argc*/, char * /*argv*/[]) {
  // Singleton guard: SCM occasionally tries to start a service while a
  // previous instance is still draining. Bail with a recognizable exit
  // code so an external launcher (or logs) can distinguish a singleton
  // race from a real failure.
  HANDLE singleton = CreateMutexW(nullptr, FALSE, kSingletonName);
  if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 3;
  }

  // Services start with CWD == %SystemRoot%\System32. Re-anchor to the
  // install root (one level up from \tools\) so any future relative-path
  // use stays predictable. Mirrors sunshinesvc.cpp:399-409.
  wchar_t module_path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
    for (int i = 0; i < 2; ++i) {
      wchar_t *last = std::wcsrchr(module_path, L'\\');
      if (last) {
        *last = 0;
      }
    }
    SetCurrentDirectoryW(module_path);
  }

  static SERVICE_TABLE_ENTRYA service_table[] = {
    {const_cast<LPSTR>(kServiceName), ServiceMain},
    {nullptr,                         nullptr    },
  };
  if (!StartServiceCtrlDispatcherA(service_table)) {
    return static_cast<int>(GetLastError());
  }
  return 0;
}
