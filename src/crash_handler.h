/**
 * @file src/crash_handler.h
 * @brief Top-level crash handler: last-chance fatal logging + minidump capture.
 *
 * Motivated by the 2026-07-27 postmortem (POSTMORTEM-2026-07-27.md,
 * "silent host-crash bug"): ~10 abrupt luminalshine.exe terminations
 * between 7/25 and 7/27 left no trace in our own logs — diagnosing them
 * required WER archaeology and admin access to the SYSTEM profile's
 * CrashDumps folder. This module makes every crash self-document:
 *   - an unhandled SEH exception logs a fatal line (code, faulting
 *     address, module, dump path) and writes a stack-only minidump (thread
 *     stacks and module lists, no heap) to `<appdata>/crashdumps/` before
 *     letting WER proceed, and
 *   - std::terminate logs the active exception (if any) and re-raises it
 *     as a distinctive SEH exception so the same dump path applies.
 *
 * The header is cross-platform; the implementation lives in
 * crash_handler.cpp and is compiled on every platform so the API can be
 * called unconditionally (call sites stay short). On non-Windows
 * platforms `init()` is a no-op.
 */
#pragma once

#include <cstddef>
#include <filesystem>

#ifdef _WIN32
// Forward declaration so this header does not pull in <windows.h>.
struct _EXCEPTION_POINTERS;
#endif

namespace crash_handler {

  /**
   * @brief Install the process-wide crash handlers (Windows only).
   *
   * Call once, early in main(), after logging is initialised — the
   * handlers emit through BOOST_LOG(fatal) so a crash before logging is
   * up would only reach WER anyway. Creates `<appdata>/crashdumps/`,
   * restricts it to SYSTEM and Administrators (if that fails, no minidumps
   * are written), prunes it to the newest dumps, prebuilds the dump
   * directory path into a static buffer (so the exception filter itself
   * avoids heap allocation), then installs SetUnhandledExceptionFilter and
   * std::set_terminate.
   */
  void init();

  /**
   * @brief Mark that process exit has begun. Registered with atexit by init().
   *
   * Boost.Log's sinks are torn down during exit; once this is set the crash and
   * terminate handlers keep to stderr / OutputDebugString and the minidump.
   */
  void note_exit_started() noexcept;

  /**
   * @brief Delete the oldest `.dmp` files in @p dir until at most @p keep remain.
   *
   * Ordering is by last-write time (newest kept). Non-`.dmp` entries and
   * subdirectories are ignored; a missing or unreadable directory is a
   * no-op. Called from `init()` on every startup — deliberately *not*
   * from the exception filter, where directory iteration would allocate.
   * Exposed here so the unit tests can exercise it directly.
   *
   * @param dir Directory to prune.
   * @param keep Number of newest dumps to keep.
   * @return Number of files removed.
   */
  std::size_t prune_dumps(const std::filesystem::path &dir, std::size_t keep = 5);

#ifdef _WIN32

  /**
   * @brief Write a minidump of the current process to an open file.
   *
   * Called by the exception filter once it has created the dump file. Nothing
   * runs after the dump call, so GetLastError() still describes a failed write
   * when this returns false. Exposed here so the unit tests can exercise it
   * directly, like prune_dumps.
   *
   * @param file Writable file HANDLE.
   * @param exception_info Exception to record; it must belong to the calling thread.
   * @return True when the dump was written.
   */
  bool write_minidump(void *file, _EXCEPTION_POINTERS *exception_info);

  /**
   * @brief Store the message of the std::terminate exception for the dump.
   *
   * Clears the whole static message buffer, then copies @p message in,
   * truncated and NUL-terminated. Passing nullptr only clears it. Exposed here
   * so the unit tests can exercise the dump writer with and without a message.
   *
   * @param message Message text, or nullptr to clear.
   */
  void note_terminate_message(const char *message) noexcept;

  /**
   * @brief Tell whether @p path is a minidump this handler writes today.
   *
   * Reads the minidump header and accepts the file only if the signature
   * matches and the header flags hold nothing beyond the stack-only dump
   * types (plus the AVX state bit dbghelp adds on its own). Dumps from
   * before the stack-only change, which also hold the memory behind stack
   * pointers, fail the check, as do truncated files and anything unreadable.
   * Used by the crash bundle to decide which dumps may be exported.
   *
   * @param path File to check.
   * @return True for a stack-only dump.
   */
  bool is_stack_only_dump(const std::filesystem::path &path);

#endif

}  // namespace crash_handler
