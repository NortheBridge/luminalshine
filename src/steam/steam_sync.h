/**
 * @file src/steam/steam_sync.h
 * @brief Steam library auto-sync. Scans every Steam library folder
 *        for installed games (via the appmanifest_*.acf files Steam
 *        keeps next to each install), filters family-shared titles
 *        per user preference, and writes a `steam_apps.json` catalogue
 *        beside the user's `apps.json` for the proc layer to merge
 *        into the displayed app list.
 *
 *        The sync is read-only against Steam's own files. We never
 *        modify anything in the Steam install or library folders;
 *        the only file we write is our own steam_apps.json inside
 *        the LuminalShine config directory.
 *
 *        Launch model: each sync'd entry's `detached` array carries
 *        a single command that opens `steam://rungameid/<appid>`
 *        via cmd.exe's URL-protocol handler. Steam picks the URL
 *        up whether the client is already running or not. End-of-
 *        game detection is intentionally out of scope for now — the
 *        Moonlight user ends the stream manually when they're done,
 *        matching the behaviour of the long-standing hand-curated
 *        "Steam" entry in apps.json.
 */
#pragma once

#include "src/platform/common.h"

#include <memory>
#include <string>

namespace steam::sync {

  /**
   * @brief Run one scan-and-write cycle synchronously. Reads every
   *        appmanifest_*.acf across all library folders, applies the
   *        family-shared filter (config::steam.include_family_shared),
   *        writes the result as `steam_apps.json` next to @p
   *        apps_json_path, and triggers a proc::refresh so the new
   *        entries surface in Moonlight immediately.
   *
   *        Safe to call when `steam_auto_sync` is disabled — in that
   *        case the function returns early without touching disk.
   *        Safe to call when Steam isn't installed — returns early
   *        when steam::paths::steam_install_root() is empty.
   */
  void run_once(const std::string &apps_json_path);

  /**
   * @brief Delete the `steam_apps.json` file next to @p apps_json_path
   *        (if it exists) and refresh proc so the entries disappear
   *        from Moonlight. Wired to the
   *        `/api/state/reset-steam-library-cache` endpoint and to
   *        the toggle-off path in saveConfig.
   */
  void clear_cache(const std::string &apps_json_path);

  /**
   * @brief Start the background sync worker (~30s polling cadence
   *        over each library's `steamapps/` directory; re-runs the
   *        scan only when the directory contents fingerprint
   *        changes). Returns a guard whose destructor stops the
   *        worker. Returns nullptr — meaning no worker, no
   *        background activity — when `steam_auto_sync` is off at
   *        the time of call.
   *
   *        The worker reads `config::steam.auto_sync` and
   *        `config::steam.include_family_shared` live on every tick
   *        so toggling them via /api/config takes effect without a
   *        service restart.
   */
  std::unique_ptr<platf::deinit_t> start_worker(const std::string &apps_json_path);

}  // namespace steam::sync
