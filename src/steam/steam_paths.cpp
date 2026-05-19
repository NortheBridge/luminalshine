/**
 * @file src/steam/steam_paths.cpp
 * @brief Windows-only implementation of Steam path discovery. The
 *        non-Windows stubs return empty values; the auto-sync feature
 *        is gated to Windows by callers so those stubs are only ever
 *        invoked from tests on non-Windows build hosts.
 */
#include "src/steam/steam_paths.h"

#include "src/logging.h"
#include "src/steam/vdf_parser.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
  // clang-format off
  #include <winsock2.h>
  #include <windows.h>
  // clang-format on
#endif

namespace steam::paths {

  namespace fs = std::filesystem;

  namespace {

#ifdef _WIN32
    /// Read a REG_SZ value from the given registry key + value name.
    /// Returns empty string on any failure (key missing, wrong type,
    /// access denied). Uses RegGetValueW with RRF_RT_REG_SZ so the
    /// function fails closed if the value is the wrong type.
    std::optional<std::wstring> read_reg_string(HKEY root, std::wstring_view subkey, std::wstring_view name) {
      DWORD size = 0;
      LSTATUS s = RegGetValueW(
        root,
        std::wstring {subkey}.c_str(),
        std::wstring {name}.c_str(),
        RRF_RT_REG_SZ,
        nullptr,
        nullptr,
        &size
      );
      if (s != ERROR_SUCCESS || size == 0) {
        return std::nullopt;
      }
      std::wstring buf(size / sizeof(wchar_t), L'\0');
      s = RegGetValueW(
        root,
        std::wstring {subkey}.c_str(),
        std::wstring {name}.c_str(),
        RRF_RT_REG_SZ,
        nullptr,
        buf.data(),
        &size
      );
      if (s != ERROR_SUCCESS) {
        return std::nullopt;
      }
      // RegGetValueW always nul-terminates; trim any trailing nulls.
      while (!buf.empty() && buf.back() == L'\0') {
        buf.pop_back();
      }
      return buf;
    }
#endif

    std::string read_file_utf8(const fs::path &p) {
      std::ifstream in(p, std::ios::binary);
      if (!in) {
        return {};
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

  }  // namespace

  std::optional<fs::path> steam_install_root() {
#ifdef _WIN32
    // The launcher writes its own install path to HKCU\Software\Valve\
    // Steam\SteamPath in forward-slash form on every Steam start, so
    // that key is the authoritative source for the current user. Fall
    // back to the per-machine 32-bit Wow6432Node InstallPath key when
    // the per-user key is missing (e.g. service account on first run
    // before the user has launched Steam interactively).
    if (auto p = read_reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) {
      fs::path candidate(*p);
      std::error_code ec;
      if (fs::exists(candidate, ec)) {
        return candidate;
      }
    }
    if (auto p = read_reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) {
      fs::path candidate(*p);
      std::error_code ec;
      if (fs::exists(candidate, ec)) {
        return candidate;
      }
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
  }

  std::vector<LibraryFolder> library_folders() {
    std::vector<LibraryFolder> out;
    const auto root = steam_install_root();
    if (!root) {
      return out;
    }

    // Default library: always the install root's own steamapps/ dir.
    // Only emit it if libraryfolders.vdf doesn't list it (Steam
    // includes the root in the VDF after the user's first library
    // configuration; on a freshly installed Steam it's the only
    // implicit library).
    const fs::path libraryfolders_vdf = *root / "config" / "libraryfolders.vdf";
    std::error_code ec;
    if (!fs::exists(libraryfolders_vdf, ec)) {
      std::error_code ec2;
      if (fs::exists(*root / "steamapps", ec2)) {
        out.push_back({*root, {}});
      }
      return out;
    }

    const auto content = read_file_utf8(libraryfolders_vdf);
    if (content.empty()) {
      return out;
    }
    auto tree = steam::vdf::parse(content);
    if (!tree) {
      BOOST_LOG(warning) << "steam_paths: failed to parse libraryfolders.vdf at "
                         << libraryfolders_vdf.string();
      return out;
    }

    // Schema (post-2021 Steam):
    //   "libraryfolders" {
    //     "0" { "path" "..." "apps" { "440" "1234" ... } }
    //     "1" { ... }
    //   }
    // Pre-2021 Steam used a different layout (no per-folder block,
    // just "0"/"1" mapping straight to a path string). Both are
    // handled.
    for (const auto &child : tree->children()) {
      if (child.is_string()) {
        // Legacy layout: child key is the index, value is the path.
        fs::path p(child.as_string());
        if (fs::exists(p, ec)) {
          out.push_back({std::move(p), {}});
        }
        continue;
      }
      // Modern layout: child is a block; pick up "path" + "apps".
      LibraryFolder folder;
      const auto path_str = child.find_string("path");
      if (path_str.empty()) {
        continue;
      }
      folder.path = fs::path(path_str);
      if (!fs::exists(folder.path, ec)) {
        continue;
      }
      if (const auto *apps_node = child.find("apps"); apps_node && !apps_node->is_string()) {
        for (const auto &app : apps_node->children()) {
          folder.installed_appids.push_back(app.key);
        }
      }
      out.push_back(std::move(folder));
    }

    return out;
  }

  fs::path steamapps_dir(const LibraryFolder &folder) {
    return folder.path / "steamapps";
  }

  std::optional<fs::path> librarycache_dir() {
    const auto root = steam_install_root();
    if (!root) {
      return std::nullopt;
    }
    fs::path cache = *root / "appcache" / "librarycache";
    std::error_code ec;
    if (!fs::exists(cache, ec)) {
      return std::nullopt;
    }
    return cache;
  }

}  // namespace steam::paths
