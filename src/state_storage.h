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
   * @brief Atomically write a JSON ptree to disk and refresh its backup.
   *
   * Sequence:
   *   1. Serialise to "<path>.tmp" in the same directory as <path>.
   *   2. fsync / FlushFileBuffers the temp file so the new bytes are durable
   *      on disk before the rename commits — this closes the window where a
   *      power loss or Windows servicing reboot could surface a zero-byte
   *      file to the next reader.
   *   3. Rename "<path>.tmp" onto <path>. On NTFS and POSIX the rename is
   *      atomic, so concurrent readers see either the full prior contents
   *      or the full new contents, never a torn write.
   *   4. Mirror the new file to "<path>.bak" via temp+rename so the most
   *      recent good copy is always available for `load_or_recover()` if
   *      the primary file is later corrupted (e.g. by a Windows Insider
   *      Preview flight upgrade).
   *
   * Creates parent directories if needed. The caller is responsible for
   * holding `state_mutex()` if the target file is shared with other
   * writers that take the same lock.
   *
   * @return true on success, false on any I/O failure. Failure is logged.
   */
  bool atomic_write_json(const std::filesystem::path &path, const boost::property_tree::ptree &tree);

  /**
   * @brief Load a JSON ptree from disk, falling back to "<path>.bak" if the
   *        primary file fails to parse.
   *
   * Returns true on a successful read of either file. When the primary file
   * exists but cannot be parsed and the backup parses cleanly, the backup
   * is promoted back to the primary location (via `atomic_write_json`) so
   * subsequent reads succeed without further recovery and the user does not
   * silently lose pairings / credentials.
   *
   * Returns false when neither file is readable. The caller should treat
   * that case the same as "fresh install" — do NOT overwrite the primary
   * file blindly, since doing so destroys whatever bytes are left on disk
   * that an operator could still hand-recover.
   */
  bool load_or_recover(const std::filesystem::path &path, boost::property_tree::ptree &out);

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
