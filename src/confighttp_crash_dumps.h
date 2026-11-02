/**
 * @file src/confighttp_crash_dumps.h
 * @brief Crash dump discovery for the Web UI crash bundle (Windows-only).
 */
#pragma once

#ifdef _WIN32

  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <string>
  #include <vector>

namespace confighttp {

  /**
   * @brief One crash dump offered to the crash bundle.
   */
  struct CrashDumpInfo {
    std::string process;
    std::filesystem::path path;
    std::filesystem::file_time_type write_time;
    std::uint64_t size;
  };

  /**
   * @brief Find the newest recent crash dump of each LuminalShine process.
   *
   * The directories are passed in, rather than resolved here, so the unit
   * tests can point the search at scratch folders; the Web UI handlers use a
   * wrapper that supplies the real ones. Exposed here so the unit tests can
   * exercise it directly, like `crash_handler::prune_dumps`.
   *
   * @param crash_handler_dirs Folders written by the crash handler.
   * @param local_dump_dirs Folders Windows writes LocalDumps files to.
   * @param wer_report_dirs Folders holding Windows Error Reporting report folders.
   * @param max_age Oldest dump to consider, measured from its last write time.
   * @return At most one dump per process, newest first.
   */
  std::vector<CrashDumpInfo> find_recent_crash_dumps(
    const std::vector<std::filesystem::path> &crash_handler_dirs,
    const std::vector<std::filesystem::path> &local_dump_dirs,
    const std::vector<std::filesystem::path> &wer_report_dirs,
    std::chrono::hours max_age
  );

}  // namespace confighttp

#endif  // _WIN32
