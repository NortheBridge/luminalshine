/**
 * @file src/steam/shortcuts_sync.cpp
 * @brief Implementation of the non-Steam shortcuts auto-sync. Same
 *        polling-worker shape as steam_sync.cpp; the only meaningful
 *        differences are (a) the input file is binary VDF rather
 *        than text, (b) shortcuts.vdf is per-user rather than
 *        per-machine, and (c) launches go directly to the shortcut's
 *        Exe rather than through `steam://rungameid/`.
 */
#include "src/steam/shortcuts_sync.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/state_storage.h"
#include "src/steam/steam_paths.h"
#include "src/steam/vdf_parser.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace steam::sync_shortcuts {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  namespace {

    struct Entry {
      std::string steamid3;
      std::string shortcut_appid;  // local shortcut ID (Steam's grid-ID)
      std::string name;
      std::string exe;
      std::string start_dir;
      std::string launch_options;
      std::string icon;
    };

    /// Strip surrounding double quotes from a path-as-stored. Steam
    /// writes `"C:\\Path\\to\\game.exe"` (literal quotes around the
    /// path) into Exe and StartDir; the quotes are escape characters
    /// in the on-disk format, not part of the path itself.
    std::string unquote(const std::string &s) {
      if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
      }
      return s;
    }

    std::vector<Entry> scan_one_user(const steam::paths::UserdataDir &user) {
      std::vector<Entry> out;
      const fs::path vdf_path = user.path / "config" / "shortcuts.vdf";
      std::error_code ec;
      if (!fs::exists(vdf_path, ec)) {
        return out;
      }
      std::ifstream in(vdf_path, std::ios::binary);
      if (!in) {
        return out;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      const std::string content = ss.str();
      if (content.empty()) {
        return out;
      }
      auto root = steam::vdf::parse_binary(content);
      if (!root || root->is_string()) {
        BOOST_LOG(warning) << "shortcuts_sync: failed to parse "
                           << vdf_path.string();
        return out;
      }

      // Root key is conventionally "shortcuts"; entries inside it are
      // index keys ("0", "1", ...). Each per-shortcut block holds the
      // fields we care about as leaves.
      for (const auto &shortcut : root->children()) {
        if (shortcut.is_string()) {
          continue;
        }
        Entry e;
        e.steamid3 = user.steamid3;
        e.name = shortcut.find_string("AppName");
        if (e.name.empty()) {
          // Some older Steam writers used "appname" lowercase.
          e.name = shortcut.find_string("appname");
        }
        if (e.name.empty()) {
          continue;  // Unusable entry.
        }
        e.exe = unquote(shortcut.find_string("Exe"));
        if (e.exe.empty()) {
          // Legacy field name some Steam versions used.
          e.exe = unquote(shortcut.find_string("exe"));
        }
        if (e.exe.empty()) {
          continue;
        }
        e.start_dir = unquote(shortcut.find_string("StartDir"));
        if (e.start_dir.empty()) {
          e.start_dir = unquote(shortcut.find_string("startdir"));
        }
        e.launch_options = shortcut.find_string("LaunchOptions");
        if (e.launch_options.empty()) {
          e.launch_options = shortcut.find_string("launchoptions");
        }
        e.icon = unquote(shortcut.find_string("icon"));

        // appid is stored as int32 — the binary parser already
        // formats it as a decimal string. Use it as-is (after
        // dropping a leading minus sign — Steam writes negative
        // appids and the decimal-string serialisation handles that).
        e.shortcut_appid = shortcut.find_string("appid");
        if (e.shortcut_appid.empty()) {
          e.shortcut_appid = shortcut.find_string("AppID");
        }

        out.emplace_back(std::move(e));
      }
      return out;
    }

    std::vector<Entry> scan() {
      std::vector<Entry> out;
      if (!steam::paths::steam_install_root()) {
        return out;
      }
      for (const auto &user : steam::paths::userdata_dirs()) {
        auto user_entries = scan_one_user(user);
        std::move(user_entries.begin(), user_entries.end(), std::back_inserter(out));
      }
      // Stable alphabetical sort for predictable Moonlight ordering.
      std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        return std::lexicographical_compare(
          a.name.begin(), a.name.end(),
          b.name.begin(), b.name.end(),
          [](unsigned char x, unsigned char y) {
            return std::tolower(x) < std::tolower(y);
          }
        );
      });
      return out;
    }

    fs::path nonsg_apps_json_path(const std::string &apps_json_path) {
      return fs::path(apps_json_path).parent_path() / "nonsg_apps.json";
    }

    /// Compose the cmd string for a shortcut. Quote the exe path so
    /// spaces don't split the command, then append LaunchOptions
    /// verbatim (Steam already serialises them as a shell-ready
    /// fragment).
    std::string compose_cmd(const Entry &e) {
      std::string out = "\"" + e.exe + "\"";
      if (!e.launch_options.empty()) {
        out += " ";
        out += e.launch_options;
      }
      return out;
    }

    pt::ptree build_tree(const std::vector<Entry> &entries) {
      pt::ptree root;
      pt::ptree apps_array;
      root.put("generated_by", "luminalshine-nonsteam-shortcuts-sync");
      for (const auto &e : entries) {
        pt::ptree app;
        app.put("name", e.name);
        app.put("cmd", compose_cmd(e));
        if (!e.start_dir.empty()) {
          app.put("working-dir", e.start_dir);
        }
        if (!e.icon.empty()) {
          app.put("image-path", e.icon);
        }
        app.put("auto-detach", true);
        app.put("wait-all", true);
        app.put("exit-timeout", 10);
        apps_array.push_back({"", app});
      }
      root.add_child("apps", apps_array);
      return root;
    }

    bool write_tree_atomic(const fs::path &path, const pt::ptree &tree) {
      return statefile::atomic_write_json(path, tree);
    }

    std::uint64_t compute_fingerprint() {
      std::uint64_t fp = 0;
      for (const auto &user : steam::paths::userdata_dirs()) {
        const fs::path vdf_path = user.path / "config" / "shortcuts.vdf";
        std::error_code ec;
        if (!fs::exists(vdf_path, ec)) {
          continue;
        }
        fp += static_cast<std::uint64_t>(
          fs::last_write_time(vdf_path, ec).time_since_epoch().count()
        );
        fp += 0x9e3779b97f4a7c15ULL;
      }
      return fp;
    }

    class Worker: public platf::deinit_t {
    public:
      explicit Worker(std::string apps_json_path):
          _apps_json_path(std::move(apps_json_path)) {
        _thread = std::thread([this]() {
          loop();
        });
      }

      ~Worker() override {
        {
          std::lock_guard<std::mutex> lk(_mtx);
          _stop = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
          _thread.join();
        }
      }

    private:
      void loop() {
        using namespace std::chrono_literals;
        std::uint64_t last_fp = 0;
        std::unique_lock<std::mutex> lk(_mtx);
        while (!_stop) {
          lk.unlock();
          try {
            if (config::steam.nonsteam_shortcuts_auto_sync) {
              const auto fp = compute_fingerprint();
              if (fp != last_fp) {
                run_once(_apps_json_path);
                last_fp = fp;
              }
            }
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "shortcuts_sync worker: " << e.what();
          }
          lk.lock();
          _cv.wait_for(lk, 30s, [&]() {
            return _stop;
          });
        }
      }

      std::string _apps_json_path;
      std::thread _thread;
      std::mutex _mtx;
      std::condition_variable _cv;
      bool _stop = false;
    };

  }  // namespace

  void run_once(const std::string &apps_json_path) {
    if (!config::steam.nonsteam_shortcuts_auto_sync) {
      return;
    }
    if (!steam::paths::steam_install_root()) {
      return;
    }
    auto entries = scan();
    const auto out_path = nonsg_apps_json_path(apps_json_path);
    auto tree = build_tree(entries);

    std::lock_guard<std::mutex> guard(statefile::state_mutex());
    if (!write_tree_atomic(out_path, tree)) {
      BOOST_LOG(warning) << "shortcuts_sync: failed to write " << out_path.string();
      return;
    }
    proc::refresh(apps_json_path);
    BOOST_LOG(info) << "shortcuts_sync: refreshed app list with " << entries.size()
                    << " non-Steam shortcut entries.";
  }

  void clear_cache(const std::string &apps_json_path) {
    const auto out_path = nonsg_apps_json_path(apps_json_path);
    std::lock_guard<std::mutex> guard(statefile::state_mutex());
    std::error_code ec;
    fs::remove(out_path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "shortcuts_sync: clear_cache could not remove "
                         << out_path.string() << ": " << ec.message();
    }
    proc::refresh(apps_json_path);
    BOOST_LOG(info) << "shortcuts_sync: cleared non-Steam shortcuts cache.";
  }

  std::unique_ptr<platf::deinit_t> start_worker(const std::string &apps_json_path) {
    return std::make_unique<Worker>(apps_json_path);
  }

}  // namespace steam::sync_shortcuts
