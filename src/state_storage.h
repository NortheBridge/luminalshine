#pragma once

#include <boost/property_tree/ptree_fwd.hpp>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace statefile {

  const std::string &sunshine_state_path();

  const std::string &luminalshine_state_path();

  std::mutex &state_mutex();

  void migrate_recent_state_keys();

  /**
   * @brief Atomically write a JSON ptree to disk.
   *
   * Writes to "<path>.tmp" and then renames it onto <path>. On NTFS and
   * POSIX filesystems the rename is atomic, so a concurrent reader (or a
   * crash mid-write) will see either the full prior file contents or the
   * full new contents — never a partial / truncated file. Creates parent
   * directories if needed.
   *
   * The caller is responsible for holding `state_mutex()` if the target
   * file is shared with other writers that take the same lock.
   *
   * @return true on success, false on any I/O failure. Failure is logged.
   */
  bool atomic_write_json(const std::filesystem::path &path, const boost::property_tree::ptree &tree);

  /**
   * @brief Persist the snapshot exclusion device list to luminalshine_state.json.
   * @param devices List of device IDs to exclude from display snapshots.
   *
   * This is called when config is saved/applied so that the display helper
   * can read the exclusion list directly without depending on IPC from Sunshine.
   */
  void save_snapshot_exclude_devices(const std::vector<std::string> &devices);

  /**
   * @brief Load the snapshot exclusion device list from luminalshine_state.json.
   * @return The list of device IDs to exclude, or an empty vector if not found.
   */
  std::vector<std::string> load_snapshot_exclude_devices();

}  // namespace statefile
