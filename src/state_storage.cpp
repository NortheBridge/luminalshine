#include "state_storage.h"

#include "config.h"
#include "logging.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace statefile {
  namespace {
    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;

    std::once_flag migration_once;

    pt::ptree &ensure_root(pt::ptree &tree) {
      auto it = tree.find("root");
      if (it == tree.not_found()) {
        auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
        return inserted->second;
      }
      return it->second;
    }

    bool load_tree_if_exists(const fs::path &path, pt::ptree &out) {
      // Delegate to the public recovery-aware loader so the migration and the
      // snapshot-exclusion readers also benefit from the .bak fallback. The
      // public helper handles existence checks, parse failures, and
      // restoration in one shot.
      out.clear();
      return load_or_recover(path, out);
    }

    void write_tree(const fs::path &path, const pt::ptree &tree) {
      // Route through the atomic helper so callers in this file get the same
      // crash-safety guarantee as external callers.
      atomic_write_json(path, tree);
    }

    // Force the file's bytes (and Windows metadata) to physical storage. Closes
    // the window where a power loss or Windows servicing reboot between the
    // write and the next read could surface a zero-byte file. Returns true on
    // success; logs and returns false on failure. The caller decides whether
    // to abort or proceed — in our case we abort and clean up the temp file.
    bool fsync_path(const fs::path &path) {
#ifdef _WIN32
      HANDLE h = ::CreateFileW(
        path.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      if (h == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "statefile: fsync open failed for "sv << path.string()
                           << " (err="sv << ::GetLastError() << ")"sv;
        return false;
      }
      const BOOL ok = ::FlushFileBuffers(h);
      const DWORD err = ok ? 0 : ::GetLastError();
      ::CloseHandle(h);
      if (!ok) {
        BOOST_LOG(warning) << "statefile: FlushFileBuffers failed for "sv << path.string()
                           << " (err="sv << err << ")"sv;
        return false;
      }
      return true;
#else
      int fd = ::open(path.string().c_str(), O_RDONLY);
      if (fd < 0) {
        BOOST_LOG(warning) << "statefile: fsync open failed for "sv << path.string();
        return false;
      }
      const int rc = ::fsync(fd);
      ::close(fd);
      if (rc != 0) {
        BOOST_LOG(warning) << "statefile: fsync failed for "sv << path.string();
        return false;
      }
      return true;
#endif
    }

    // Mirror the freshly-committed primary file to "<path>.bak". Uses the same
    // temp+rename pattern so the backup itself is never observed in a torn
    // state, even if interrupted. Failures here are non-fatal — the primary
    // file is already durable on disk; the backup is a recovery aid.
    void update_backup(const fs::path &path) {
      fs::path bak_path = path;
      bak_path += ".bak";
      fs::path bak_temp = path;
      bak_temp += ".bak.tmp";

      std::error_code copy_ec;
      fs::copy_file(path, bak_temp, fs::copy_options::overwrite_existing, copy_ec);
      if (copy_ec) {
        BOOST_LOG(warning) << "statefile: backup copy "sv << path.string() << " -> "sv
                           << bak_temp.string() << " failed: "sv << copy_ec.message();
        return;
      }

      // Best-effort durability for the backup, then atomic swap.
      (void) fsync_path(bak_temp);

      std::error_code mv_ec;
      fs::rename(bak_temp, bak_path, mv_ec);
      if (mv_ec) {
        BOOST_LOG(warning) << "statefile: backup rename "sv << bak_temp.string() << " -> "sv
                           << bak_path.string() << " failed: "sv << mv_ec.message();
        std::error_code rm_ec;
        fs::remove(bak_temp, rm_ec);
      }
    }
  }  // namespace

  bool atomic_write_json(const fs::path &path, const pt::ptree &tree) {
    if (path.empty()) {
      BOOST_LOG(error) << "statefile: atomic_write_json called with empty path"sv;
      return false;
    }

    // Create parent directory if missing.
    auto dir = path;
    dir.remove_filename();
    if (!dir.empty() && !fs::exists(dir)) {
      std::error_code mk_ec;
      fs::create_directories(dir, mk_ec);
      if (mk_ec) {
        BOOST_LOG(error) << "statefile: failed to create dir "sv << dir.string() << ": "sv << mk_ec.message();
        return false;
      }
    }

    // Sibling temp file in the same directory keeps the eventual rename on a
    // single filesystem volume — required for the OS-level rename to be
    // atomic. On Windows this maps to MoveFileExW(MOVEFILE_REPLACE_EXISTING).
    fs::path temp_path = path;
    temp_path += ".tmp";

    try {
      pt::write_json(temp_path.string(), tree);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "statefile: failed to write temp file "sv << temp_path.string() << ": "sv << e.what();
      std::error_code rm_ec;
      fs::remove(temp_path, rm_ec);
      return false;
    }

    // Flush the temp file's bytes to physical storage before the rename so a
    // sudden power loss or Windows servicing reboot can never expose a
    // post-rename file that is zero-length on disk.
    (void) fsync_path(temp_path);

    std::error_code mv_ec;
    fs::rename(temp_path, path, mv_ec);
    if (mv_ec) {
      BOOST_LOG(error) << "statefile: atomic rename "sv << temp_path.string() << " -> "sv << path.string()
                       << " failed: "sv << mv_ec.message();
      std::error_code rm_ec;
      fs::remove(temp_path, rm_ec);
      return false;
    }

    // Refresh the on-disk backup. Done after the primary commits so the .bak
    // always reflects a known-good past version.
    update_backup(path);
    return true;
  }

  bool load_or_recover(const fs::path &path, pt::ptree &out) {
    if (path.empty()) {
      return false;
    }

    std::error_code ec;
    const bool primary_exists = fs::exists(path, ec);
    if (primary_exists) {
      try {
        pt::read_json(path.string(), out);
        return true;
      } catch (const std::exception &e) {
        // Distinguish "file is empty / zero bytes" — the canonical failure
        // mode after an interrupted Windows servicing reboot — from a parse
        // error mid-document, but treat both the same: try the backup.
        BOOST_LOG(warning) << "statefile: failed to read "sv << path.string()
                           << " ("sv << e.what() << "); attempting recovery from .bak"sv;
        out.clear();
      }
    }

    fs::path bak_path = path;
    bak_path += ".bak";
    if (!fs::exists(bak_path, ec)) {
      return false;
    }

    try {
      pt::read_json(bak_path.string(), out);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "statefile: backup "sv << bak_path.string()
                       << " also unreadable ("sv << e.what() << "); cannot recover"sv;
      out.clear();
      return false;
    }

    BOOST_LOG(warning) << "statefile: recovered "sv << path.string() << " from backup "sv
                       << bak_path.string() << "; restoring primary"sv;
    // Promote the backup back to the primary location so subsequent reads
    // succeed without further recovery. Uses atomic_write_json so the
    // restoration itself is crash-safe and refreshes the .bak in turn.
    (void) atomic_write_json(path, out);
    return true;
  }

  std::mutex &state_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  const std::string &sunshine_state_path() {
    return config::nvhttp.file_state;
  }

  const std::string &luminalshine_state_path() {
    if (!config::nvhttp.luminalshine_file_state.empty()) {
      return config::nvhttp.luminalshine_file_state;
    }
    return config::nvhttp.file_state;
  }

  void migrate_recent_state_keys() {
    std::call_once(migration_once, [] {
      const fs::path old_path = sunshine_state_path();
      const fs::path new_path = luminalshine_state_path();

      if (old_path.empty() || new_path.empty() || old_path == new_path) {
        return;
      }

      std::lock_guard<std::mutex> guard(state_mutex());

      pt::ptree old_tree;
      const bool old_loaded = load_tree_if_exists(old_path, old_tree);

      pt::ptree new_tree;
      const bool new_loaded = load_tree_if_exists(new_path, new_tree);
      (void) new_loaded;

      bool old_modified = false;
      bool new_modified = false;

      if (old_loaded) {
        auto old_root_it = old_tree.find("root");
        if (old_root_it != old_tree.not_found()) {
          auto &old_root = old_root_it->second;

          auto move_child = [&](const std::string &key) {
            auto child_it = old_root.find(key);
            if (child_it == old_root.not_found()) {
              return;
            }
            auto &new_root = ensure_root(new_tree);
            if (new_root.find(key) == new_root.not_found()) {
              new_root.put_child(key, child_it->second);
              new_modified = true;
            }
            old_root.erase(old_root.to_iterator(child_it));
            old_modified = true;
          };

          move_child("api_tokens");
          move_child("session_tokens");

          if (auto last = old_root.get_optional<std::string>("last_notified_version")) {
            auto &new_root = ensure_root(new_tree);
            if (!new_root.get_optional<std::string>("last_notified_version")) {
              new_root.put("last_notified_version", *last);
              new_modified = true;
            }
            old_root.erase("last_notified_version");
            old_modified = true;
          }
        }
      }

      if (new_modified) {
        write_tree(new_path, new_tree);
      }
      if (old_modified) {
        write_tree(old_path, old_tree);
      }
    });
  }

  void save_snapshot_exclude_devices(const std::vector<std::string> &devices) {
    migrate_recent_state_keys();
    const auto &path_str = luminalshine_state_path();
    if (path_str.empty()) {
      BOOST_LOG(warning) << "statefile: cannot save snapshot exclusions - luminalshine state path is empty";
      return;
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    (void) load_tree_if_exists(path, root);

    // Build the exclusion list as a property tree array
    pt::ptree exclusions_pt;
    for (const auto &device_id : devices) {
      if (!device_id.empty()) {
        pt::ptree item;
        item.put_value(device_id);
        exclusions_pt.push_back({"", item});
      }
    }

    auto &root_node = ensure_root(root);
    root_node.put_child("snapshot_exclude_devices", exclusions_pt);

    write_tree(path, root);
    BOOST_LOG(info) << "statefile: persisted " << devices.size() << " snapshot exclusion device(s) to luminalshine state";
  }

  std::vector<std::string> load_snapshot_exclude_devices() {
    migrate_recent_state_keys();
    const auto &path_str = luminalshine_state_path();
    if (path_str.empty()) {
      return {};
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (!load_tree_if_exists(path, root)) {
      return {};
    }

    std::vector<std::string> devices;
    try {
      auto root_node_opt = root.get_child_optional("root");
      if (!root_node_opt) {
        return {};
      }
      auto exclusions_opt = root_node_opt->get_child_optional("snapshot_exclude_devices");
      if (!exclusions_opt) {
        return {};
      }
      for (const auto &item : *exclusions_opt) {
        const auto device_id = item.second.get_value<std::string>("");
        if (!device_id.empty()) {
          devices.push_back(device_id);
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "statefile: failed to parse snapshot exclusions: " << e.what();
    }
    return devices;
  }

}  // namespace statefile
