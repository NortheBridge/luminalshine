/**
 * @file src/steam/steam_sync.cpp
 * @brief Implementation of the Steam library auto-sync. Scans Steam's
 *        own appmanifest files, filters per the user's toggles,
 *        writes a catalogue file the proc layer (PR 1) already knows
 *        how to merge into the app list.
 */
#include "src/steam/steam_sync.h"

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

namespace steam::sync {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  namespace {

    struct Entry {
      std::string appid;
      std::string name;
      std::string installdir;
      std::string image_path;
      bool family_shared = false;
    };

    /// Read one appmanifest_<appid>.acf and produce an Entry on
    /// success. Returns nullopt for manifests that look like
    /// downloading-but-not-installed entries (StateFlags without the
    /// 0x4 "FullyInstalled" bit) so we don't surface games the user
    /// can't actually launch.
    std::optional<Entry> parse_manifest(const fs::path &manifest_path) {
      std::ifstream in(manifest_path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      const std::string content = ss.str();
      if (content.empty()) {
        return std::nullopt;
      }
      auto root = steam::vdf::parse(content);
      if (!root) {
        return std::nullopt;
      }
      Entry e;
      e.appid = root->find_string("appid");
      if (e.appid.empty()) {
        return std::nullopt;
      }
      e.name = root->find_string("name");
      if (e.name.empty()) {
        return std::nullopt;
      }
      e.installdir = root->find_string("installdir");

      // StateFlags is a bitmask; bit 0x4 = FullyInstalled. Anything
      // without that bit set is either currently downloading,
      // pending update, or update-required-but-cant-launch; skip.
      try {
        const auto flags_str = root->find_string("StateFlags", "0");
        const auto flags = std::stoul(flags_str);
        constexpr unsigned int kFullyInstalled = 0x4;
        if ((flags & kFullyInstalled) == 0) {
          return std::nullopt;
        }
      } catch (...) {
        return std::nullopt;
      }

      // Family-shared games carry a non-empty SharedDepots block
      // listing borrowed depots. Owned games either lack the block
      // entirely or carry it empty.
      if (const auto *shared = root->find("SharedDepots"); shared && !shared->is_string()) {
        e.family_shared = !shared->children().empty();
      }
      return e;
    }

    /// Attach the cached artwork path (if Steam has downloaded it).
    /// Steam's librarycache stores per-game art at the
    /// `<appid>_library_600x900.jpg` filename. If the user has never
    /// opened a game's Library page in Steam the file may be absent
    /// — the entry then ships with an empty image-path and renders
    /// with the default LuminalShine app tile, same as a hand-added
    /// app with no image-path.
    void attach_image(Entry &e, const std::optional<fs::path> &librarycache) {
      if (!librarycache) {
        return;
      }
      // Try the 600x900 portrait tile first (Steam's primary library
      // art format since 2019); fall back to the legacy header.jpg
      // shape if portrait is missing.
      const fs::path portrait = *librarycache / (e.appid + "_library_600x900.jpg");
      std::error_code ec;
      if (fs::exists(portrait, ec)) {
        e.image_path = portrait.string();
        return;
      }
      const fs::path header = *librarycache / (e.appid + "_header.jpg");
      if (fs::exists(header, ec)) {
        e.image_path = header.string();
      }
    }

    std::vector<Entry> scan() {
      std::vector<Entry> out;
      const auto folders = steam::paths::library_folders();
      if (folders.empty()) {
        return out;
      }
      const auto cache = steam::paths::librarycache_dir();
      std::set<std::string> seen_appids;  // de-dup across libraries
      for (const auto &folder : folders) {
        const fs::path apps_dir = steam::paths::steamapps_dir(folder);
        std::error_code ec;
        if (!fs::exists(apps_dir, ec) || !fs::is_directory(apps_dir, ec)) {
          continue;
        }
        for (const auto &entry : fs::directory_iterator(apps_dir, ec)) {
          if (ec) {
            break;
          }
          if (!entry.is_regular_file()) {
            continue;
          }
          const auto fn = entry.path().filename().string();
          if (fn.rfind("appmanifest_", 0) != 0 || entry.path().extension() != ".acf") {
            continue;
          }
          auto parsed = parse_manifest(entry.path());
          if (!parsed) {
            continue;
          }
          if (!seen_appids.insert(parsed->appid).second) {
            continue;
          }
          attach_image(*parsed, cache);
          out.emplace_back(std::move(*parsed));
        }
      }
      // Stable alphabetical sort so the Moonlight order doesn't
      // depend on filesystem enumeration order. Users can browse
      // 200+ games more easily this way than by install timestamp.
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

    fs::path steam_apps_json_path(const std::string &apps_json_path) {
      return fs::path(apps_json_path).parent_path() / "steam_apps.json";
    }

    /// Build the steam_apps.json file body. Each entry uses the same
    /// shape as the hand-curated "Steam Big Picture" entry in the
    /// default apps.json template: `cmd` set to the steam:// URL,
    /// `auto-detach` true so the launcher process returning quickly
    /// doesn't terminate the session, and `wait-all` true so any
    /// child processes the Steam launcher may have spawned for the
    /// game are accounted for. Steam (not LuminalShine) owns the
    /// game lifetime; the user ends the Moonlight stream manually
    /// when they're done playing.
    pt::ptree build_tree(const std::vector<Entry> &entries, bool include_family_shared) {
      pt::ptree root;
      pt::ptree apps_array;
      root.put("generated_by", "luminalshine-steam-sync");
      for (const auto &e : entries) {
        if (!include_family_shared && e.family_shared) {
          continue;
        }
        pt::ptree app;
        app.put("name", e.name);
        // image-path is optional; only emit when we found cached art
        // (saves the proc parser from doing an extra existence check).
        if (!e.image_path.empty()) {
          app.put("image-path", e.image_path);
        }
        app.put("cmd", "steam://rungameid/" + e.appid);
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

    /// Fingerprint of "what does the manifest set look like" — a sum
    /// of last-write-times plus file count per library folder. Used
    /// by the polling worker to skip the expensive scan+write+refresh
    /// chain when nothing has changed.
    std::uint64_t compute_fingerprint() {
      std::uint64_t fp = 0;
      const auto folders = steam::paths::library_folders();
      for (const auto &folder : folders) {
        const fs::path apps_dir = steam::paths::steamapps_dir(folder);
        std::error_code ec;
        if (!fs::exists(apps_dir, ec)) {
          continue;
        }
        for (const auto &entry : fs::directory_iterator(apps_dir, ec)) {
          if (ec) {
            break;
          }
          if (!entry.is_regular_file()) {
            continue;
          }
          const auto fn = entry.path().filename().string();
          if (fn.rfind("appmanifest_", 0) != 0 || entry.path().extension() != ".acf") {
            continue;
          }
          fp += static_cast<std::uint64_t>(
            entry.last_write_time().time_since_epoch().count()
          );
          fp += 0x9e3779b97f4a7c15ULL;  // mix per-file salt
        }
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
        bool last_include_family_shared = config::steam.include_family_shared;

        // First tick runs immediately so a freshly toggled-on
        // auto-sync surfaces games right away rather than waiting
        // 30 seconds.
        std::unique_lock<std::mutex> lk(_mtx);
        while (!_stop) {
          lk.unlock();
          try {
            if (config::steam.auto_sync) {
              const auto fp = compute_fingerprint();
              const bool family_pref_changed =
                config::steam.include_family_shared != last_include_family_shared;
              if (fp != last_fp || family_pref_changed) {
                run_once(_apps_json_path);
                last_fp = fp;
                last_include_family_shared = config::steam.include_family_shared;
              }
            }
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "steam_sync worker: " << e.what();
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
    if (!config::steam.auto_sync) {
      return;
    }
    if (!steam::paths::steam_install_root()) {
      return;  // Steam not installed
    }
    auto entries = scan();
    const auto out_path = steam_apps_json_path(apps_json_path);
    auto tree = build_tree(entries, config::steam.include_family_shared);

    // Hold the state mutex so we don't race a concurrent apps.json
    // save through the web UI (which also calls proc::refresh).
    std::lock_guard<std::mutex> guard(statefile::state_mutex());
    if (!write_tree_atomic(out_path, tree)) {
      BOOST_LOG(warning) << "steam_sync: failed to write " << out_path.string();
      return;
    }
    proc::refresh(apps_json_path);
    BOOST_LOG(info) << "steam_sync: refreshed app list with " << entries.size()
                    << " Steam entries (family-shared "
                    << (config::steam.include_family_shared ? "included" : "excluded")
                    << ").";
  }

  void clear_cache(const std::string &apps_json_path) {
    const auto out_path = steam_apps_json_path(apps_json_path);
    std::lock_guard<std::mutex> guard(statefile::state_mutex());
    std::error_code ec;
    fs::remove(out_path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "steam_sync: clear_cache could not remove "
                         << out_path.string() << ": " << ec.message();
    }
    proc::refresh(apps_json_path);
    BOOST_LOG(info) << "steam_sync: cleared Steam library cache.";
  }

  std::unique_ptr<platf::deinit_t> start_worker(const std::string &apps_json_path) {
    return std::make_unique<Worker>(apps_json_path);
  }

}  // namespace steam::sync
