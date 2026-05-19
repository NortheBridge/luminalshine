/**
 * @file src/steam/steam_paths.h
 * @brief Discovery of Steam install paths and library folders.
 *
 * Windows-focused: LuminalShine is a Windows-only application, so this
 * module reads the Steam install root from the user's registry
 * (HKCU\Software\Valve\Steam\SteamPath) with a fallback to the legacy
 * HKLM\SOFTWARE\WOW6432Node\Valve\Steam install location. From there
 * it reads `<root>/config/libraryfolders.vdf` to enumerate every
 * library folder on the host — multiple drives, SteamLibrary on D:\,
 * external drives, etc.
 *
 * Functions return empty vectors / nullopt on any failure (Steam not
 * installed, registry inaccessible, VDF malformed). The auto-sync
 * caller treats absence as "nothing to sync" rather than an error.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace steam::paths {

  /**
   * @brief Per-library-folder summary as published by Steam's own
   *        libraryfolders.vdf. `path` is the library root (the parent
   *        of the `steamapps/` subdirectory); `installed_appids`
   *        carries the list of appids the library claims to hold,
   *        useful as a cross-check against the actual appmanifest_*
   *        files on disk.
   */
  struct LibraryFolder {
    std::filesystem::path path;
    std::vector<std::string> installed_appids;
  };

  /**
   * @brief Discover Steam's install root, e.g.
   *        `C:\Program Files (x86)\Steam`. Returns nullopt when the
   *        registry keys are absent or the resolved path doesn't
   *        exist on disk.
   */
  std::optional<std::filesystem::path> steam_install_root();

  /**
   * @brief Enumerate every Steam library folder configured on the
   *        host by reading `<root>/config/libraryfolders.vdf`. Returns
   *        empty on parse failure or when the file is missing (which
   *        also means Steam has never been launched on the host).
   *
   *        The default library — the one under the Steam install
   *        root — is always included if present, even when
   *        libraryfolders.vdf hasn't been written yet.
   */
  std::vector<LibraryFolder> library_folders();

  /**
   * @brief Path to a single library folder's `steamapps/` directory.
   *        Convenience for the watcher in PR 2.
   */
  std::filesystem::path steamapps_dir(const LibraryFolder &folder);

  /**
   * @brief Path to the per-machine librarycache directory holding
   *        Steam's downloaded artwork tiles for installed games.
   *        Returns nullopt when the Steam install root isn't known.
   */
  std::optional<std::filesystem::path> librarycache_dir();

  /**
   * @brief One per-user Steam profile directory under
   *        `<steam_root>/userdata/<steamid3>/`. The Steam ID is a
   *        decimal string (the SteamID3 32-bit account number).
   *        Used by the non-Steam shortcuts sync (PR 3) to locate
   *        each user's `config/shortcuts.vdf` and `config/grid/`
   *        artwork directory.
   */
  struct UserdataDir {
    std::string steamid3;
    std::filesystem::path path;
  };

  /**
   * @brief Enumerate every `<root>/userdata/<steamid3>/` subdirectory
   *        on the host. Returns empty when Steam isn't installed or
   *        no user has signed in (the `userdata/` dir doesn't exist
   *        until first Steam login). Excludes the special "0/" and
   *        "anonymous" subdirectories Steam uses for offline profiles.
   */
  std::vector<UserdataDir> userdata_dirs();

}  // namespace steam::paths
