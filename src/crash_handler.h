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
 *     address, module, dump path) and writes a minidump to
 *     `<appdata>/crashdumps/` before letting WER proceed, and
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

namespace crash_handler {

  /**
   * @brief Install the process-wide crash handlers (Windows only).
   *
   * Call once, early in main(), after logging is initialised — the
   * handlers emit through BOOST_LOG(fatal) so a crash before logging is
   * up would only reach WER anyway. Creates `<appdata>/crashdumps/`,
   * prunes it to the newest dumps, prebuilds the dump directory path
   * into a static buffer (so the exception filter itself avoids heap
   * allocation), then installs SetUnhandledExceptionFilter and
   * std::set_terminate.
   */
  void init();

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

}  // namespace crash_handler
