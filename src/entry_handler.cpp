/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <format>
#include <iostream>
#include <mutex>
#include <thread>

// local includes
#include "config.h"
#include "confighttp.h"
#include "cred_store/cred_store.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "state_storage.h"

#ifdef _WIN32
  #include "cred_store/tpm_seal_windows.h"
  #include <filesystem>
  #include <ShlObj.h>
  #include <KnownFolders.h>
#endif

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

using namespace std::literals;

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    // save_user_creds requires the state lock; this offline `creds` mode
    // runs before any other thread so contention is theoretical, but holding
    // the lock keeps the contract uniform across all call sites.
    {
      std::lock_guard<std::mutex> guard(statefile::state_mutex());
      http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);
    }

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif

  int reset_admin_credentials() {
    // Use the same lock the save/load paths use so a concurrent
    // service-side credential write can't race the uninstaller. This
    // mode runs offline; contention is theoretical.
    std::lock_guard<std::mutex> guard(statefile::state_mutex());

    const std::string key = cred_store::default_key();
    const bool erase_ok = cred_store::erase(key);
    if (!erase_ok) {
      BOOST_LOG(error) << "reset_admin_credentials: cred_store::erase failed for key "
                       << key;
    }

#ifdef _WIN32
    // The TPM-bound wrapping key is shared by all sealed records; deleting
    // it here is the cleanup step for the MSI uninstall path. On non-Windows
    // there is no equivalent persisted key.
    const bool tpm_ok = cred_store::tpm_seal::clear();
    if (!tpm_ok) {
      BOOST_LOG(warning) << "reset_admin_credentials: TPM key delete failed; the key "
                         << "may persist in the Microsoft Platform Crypto Provider "
                         << "and will be regenerated on next credential save.";
    }
#endif

    BOOST_LOG(info) << "reset_admin_credentials: cleared credential record"
                    << (erase_ok ? "" : " (with errors; see log)") << ".";
    return erase_ok ? 0 : 1;
  }

  int reset_session_history() {
    // %ProgramData%\LuminalShine\sessions\<uuid>.json + the sibling
    // session_mon.port discovery file. We do NOT touch the running
    // sidecar's in-memory state — the next service restart will see
    // an empty sessions/ directory and start fresh. If KEEPADMINCRE-
    // DENTIALS-style deletion-during-active-stream becomes a
    // concern later, the right move is to stop the sidecar service
    // around this call.
#ifdef _WIN32
    namespace fs = std::filesystem;
    PWSTR pd = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd)) || !pd) {
      BOOST_LOG(warning) << "reset_session_history: could not resolve %ProgramData%";
      return 1;
    }
    fs::path root = fs::path(pd) / L"LuminalShine";
    CoTaskMemFree(pd);

    bool ok = true;
    std::error_code ec;
    std::size_t removed = 0;
    fs::path sessions = root / L"sessions";
    if (fs::exists(sessions, ec)) {
      for (const auto &entry : fs::directory_iterator(sessions, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        std::error_code rm_ec;
        fs::remove(entry.path(), rm_ec);
        if (rm_ec) {
          BOOST_LOG(warning) << "reset_session_history: failed to remove "
                             << entry.path().string() << ": " << rm_ec.message();
          ok = false;
        } else {
          ++removed;
        }
      }
    }

    fs::path port_file = root / L"session_mon.port";
    std::error_code port_ec;
    fs::remove(port_file, port_ec);

    BOOST_LOG(info) << "reset_session_history: removed " << removed
                    << " session file(s)" << (ok ? "" : " (with errors; see log)") << ".";
    return ok ? 0 : 1;
#else
    return 0;
#endif
  }
}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      // Try the post-26.05.1 service name first, then fall back to the
      // legacy `SunshineService` so this binary can still query/manage
      // the prior service on hosts where the MSI rename hasn't finished
      // (mid-upgrade, manual install, etc.). Both names point at the
      // same logical Windows service — there is never both at once
      // because the MSI install/uninstall pair around `ServiceInstall`
      // tears the old name down before the new one comes up.
      service_handle = OpenServiceA(scm_handle, "LuminalShineService", service_desired_access);
      if (!service_handle) {
        service_handle = OpenServiceA(scm_handle, "SunshineService", service_desired_access);
      }
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << SERVICE_NAME " failed to start: "sv << status.dwWin32ExitCode;
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
