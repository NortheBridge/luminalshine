/**
 * @file src/crash_handler.cpp
 * @brief Implementation of the top-level crash handler.
 */
#include "crash_handler.h"

#include "logging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
  #include <windows.h>
  #include <dbghelp.h>
// clang-format on

  #include "platform/common.h"
#endif

namespace crash_handler {

  std::size_t prune_dumps(const std::filesystem::path &dir, std::size_t keep) {
    namespace fs = std::filesystem;

    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> dumps;
    for (const auto &entry : fs::directory_iterator {dir, ec}) {
      std::error_code entry_ec;
      if (!entry.is_regular_file(entry_ec) || entry.path().extension() != ".dmp") {
        continue;
      }
      const auto written_at = entry.last_write_time(entry_ec);
      if (entry_ec) {
        continue;
      }
      dumps.emplace_back(written_at, entry.path());
    }

    if (dumps.size() <= keep) {
      return 0;
    }

    // Newest first; everything past `keep` goes.
    std::sort(dumps.begin(), dumps.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.first > rhs.first;
    });

    std::size_t removed = 0;
    for (std::size_t i = keep; i < dumps.size(); ++i) {
      std::error_code remove_ec;
      if (std::filesystem::remove(dumps[i].second, remove_ec)) {
        ++removed;
      }
    }
    return removed;
  }

#ifdef _WIN32

  namespace {
    // 0xE0 (customer-defined error severity) + "LST" — the distinctive code
    // the std::terminate handler raises so the SEH filter below writes a
    // dump for C++-level aborts too. Grep target: a fatal log line with
    // this code means "std::terminate escalation", not a hardware fault.
    constexpr DWORD kTerminateExceptionCode = 0xE04C5354;

    constexpr std::size_t kMaxDumps = 5;

    // Everything the exception filter needs is prebuilt or statically
    // allocated at init() time: by the time the filter runs, the heap may
    // be corrupt and any allocation can deadlock or recurse. The filter
    // only does swprintf into these buffers + CreateFileW + MiniDumpWriteDump.
    constexpr std::size_t kPathCapacity = 1024;
    wchar_t g_dump_dir[kPathCapacity] = {};
    wchar_t g_dump_path[kPathCapacity + 64] = {};
    wchar_t g_module_path[kPathCapacity] = {};
    char g_dump_path_utf8[kPathCapacity * 3 + 192] = {};
    char g_module_path_utf8[kPathCapacity * 3] = {};
    char g_what_buffer[512] = {};

    volatile LONG g_crash_in_progress = 0;
    volatile LONG g_terminate_in_progress = 0;

    /// Narrow a wide path into a static buffer for the log line. No heap.
    const char *to_utf8(const wchar_t *src, char *dst, int dst_size) {
      const int written = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, nullptr, nullptr);
      if (written <= 0) {
        dst[0] = '\0';
      }
      return dst;
    }

    LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *exception_info) {
      // One shot. A second faulting thread — or a fault inside this filter
      // itself (the dump write, or Boost.Log against a corrupt heap) —
      // goes straight through to WER instead of recursing.
      if (InterlockedExchange(&g_crash_in_progress, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
      }
      if (g_dump_dir[0] == L'\0') {
        return EXCEPTION_CONTINUE_SEARCH;
      }

      const DWORD code = exception_info->ExceptionRecord->ExceptionCode;
      void *const fault_address = exception_info->ExceptionRecord->ExceptionAddress;

      g_module_path[0] = L'\0';
      HMODULE fault_module = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(fault_address), &fault_module) && fault_module != nullptr) {
        GetModuleFileNameW(fault_module, g_module_path, static_cast<DWORD>(std::size(g_module_path)));
      }

      SYSTEMTIME now {};
      GetLocalTime(&now);  // local time so the filename lines up with log timestamps
      swprintf(
        g_dump_path,
        std::size(g_dump_path),
        L"%ls\\luminalshine-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
        g_dump_dir,
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId())
      );
      to_utf8(g_dump_path, g_dump_path_utf8, static_cast<int>(std::size(g_dump_path_utf8)));
      to_utf8(g_module_path, g_module_path_utf8, static_cast<int>(std::size(g_module_path_utf8)));

      // The signature log line. Includes the dump path so a user pasting
      // their log has already told us where the dump is. BOOST_LOG here is
      // a calculated risk (it allocates), but the alternative is the exact
      // in-log silence the 2026-07-27 postmortem documents; the dump write
      // below is ordered after the flush so the log line survives even if
      // MiniDumpWriteDump takes the process down.
      BOOST_LOG(fatal) << "CRASH: unhandled exception 0x" << std::hex << code << std::dec
                       << (code == kTerminateExceptionCode ? " (std::terminate escalation)" : "")
                       << " at address " << fault_address
                       << " in module " << (g_module_path_utf8[0] != '\0' ? g_module_path_utf8 : "<unknown>")
                       << "; writing minidump to " << g_dump_path_utf8;
      logging::log_flush();

      DWORD dump_error = ERROR_SUCCESS;
      BOOL dump_written = FALSE;
      const HANDLE dump_file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (dump_file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dump_exception {};
        dump_exception.ThreadId = GetCurrentThreadId();
        dump_exception.ExceptionPointers = exception_info;
        dump_exception.ClientPointers = FALSE;

        const auto dump_type = static_cast<MINIDUMP_TYPE>(
          MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
        );
        dump_written = MiniDumpWriteDump(
          GetCurrentProcess(),
          GetCurrentProcessId(),
          dump_file,
          dump_type,
          &dump_exception,
          nullptr,
          nullptr
        );
        if (!dump_written) {
          dump_error = GetLastError();
        }
        CloseHandle(dump_file);
      } else {
        dump_error = GetLastError();
      }

      if (dump_written) {
        BOOST_LOG(fatal) << "CRASH: minidump written to " << g_dump_path_utf8;
      } else {
        BOOST_LOG(fatal) << "CRASH: minidump write to " << g_dump_path_utf8
                         << " FAILED (error=" << dump_error << "); check WER CrashDumps instead";
      }
      logging::log_flush();

      // Let the process die: hand the exception on so WER still runs and
      // the SYSTEM-profile CrashDumps copy keeps existing as a backstop.
      return EXCEPTION_CONTINUE_SEARCH;
    }

    [[noreturn]] void terminate_handler() {
      // Re-entry (e.g. the logging below itself throwing) falls through to
      // an immediate abort rather than recursing.
      if (InterlockedExchange(&g_terminate_in_progress, 1) != 0) {
        std::abort();
      }

      const char *description = "no active exception";
      if (const auto active = std::current_exception()) {
        try {
          std::rethrow_exception(active);
        } catch (const std::exception &e) {
          // Copy into a static buffer: the caught object dies with this
          // catch block, and everything past here should avoid the heap.
          std::strncpy(g_what_buffer, e.what(), sizeof(g_what_buffer) - 1);
          g_what_buffer[sizeof(g_what_buffer) - 1] = '\0';
          description = g_what_buffer;
        } catch (...) {
          description = "non-standard exception (no what() available)";
        }
      }

      // Teardown-safe channels first (see the early handler in main.cpp:
      // std::terminate can fire during CRT exit-time static destruction,
      // where Boost.Log's sinks are already gone and logging through them
      // corrupts the heap). If BOOST_LOG below misbehaves, this trace has
      // already left the process.
      std::fputs("FATAL: std::terminate: ", stderr);
      std::fputs(description, stderr);
      std::fputc('\n', stderr);
      std::fflush(stderr);
      OutputDebugStringA("FATAL: std::terminate: ");
      OutputDebugStringA(description);
      OutputDebugStringA("\n");

      BOOST_LOG(fatal) << "std::terminate called: " << description
                       << ". Raising exception 0x" << std::hex << kTerminateExceptionCode << std::dec
                       << " to capture a minidump.";
      logging::log_flush();

      // Route through the SEH filter above so the abort produces the same
      // fatal log line + minidump a hardware fault would.
      RaiseException(kTerminateExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);

      // Only reachable if something (a debugger, another filter) swallowed
      // the exception. Fall back to the default abort path.
      std::abort();
    }
  }  // namespace

  void init() {
    std::filesystem::path dump_dir;
    try {
      dump_dir = platf::appdata() / "crashdumps";

      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      if (ec) {
        BOOST_LOG(warning) << "Crash handler: could not create dump directory " << dump_dir.string()
                           << ": " << ec.message() << ". Minidumps will be unavailable (WER still applies).";
        dump_dir.clear();
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Crash handler: could not resolve dump directory: " << e.what()
                         << ". Minidumps will be unavailable (WER still applies).";
      dump_dir.clear();
    }

    if (!dump_dir.empty()) {
      // Prune on startup rather than inside the exception filter, where
      // directory iteration would allocate. A crash therefore leaves at
      // most kMaxDumps + 1 dumps on disk until the next start trims back.
      const auto removed = prune_dumps(dump_dir, kMaxDumps);
      if (removed > 0) {
        BOOST_LOG(debug) << "Crash handler: pruned " << removed << " old minidump(s) from " << dump_dir.string();
      }

      const std::wstring dir_w = dump_dir.wstring();
      if (dir_w.size() < std::size(g_dump_dir)) {
        std::wmemcpy(g_dump_dir, dir_w.c_str(), dir_w.size() + 1);
      } else {
        BOOST_LOG(warning) << "Crash handler: dump directory path is too long ("
                           << dir_w.size() << " chars); minidumps will be unavailable.";
      }
    }

    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::set_terminate(terminate_handler);

    if (g_dump_dir[0] != L'\0') {
      BOOST_LOG(info) << "Crash handler installed; minidumps will be written to " << dump_dir.string();
    }
  }

#else  // !_WIN32

  void init() {
    // Windows-only for now: the silent-crash incidents this exists for
    // (POSTMORTEM-2026-07-27.md) are Windows-host crashes, and only
    // Windows is shipped. Compiled everywhere so call sites stay short.
  }

#endif

}  // namespace crash_handler
