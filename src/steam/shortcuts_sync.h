/**
 * @file src/steam/shortcuts_sync.h
 * @brief Non-Steam shortcuts auto-sync. Scans every Steam user's
 *        `<userdata>/<steamid3>/config/shortcuts.vdf` (binary VDF)
 *        for the "non-Steam games" they have added to their Steam
 *        library, and writes a `nonsg_apps.json` catalogue beside
 *        apps.json for the proc layer to merge into the display
 *        list. The merge ordering enforced in PR 1 places these
 *        entries last — after Desktop, Steam, user-added apps.json
 *        entries, and Steam Games — so they don't crowd the user's
 *        hand-curated apps.
 *
 *        Each shortcut becomes a direct-launch entry: the Exe path
 *        (with LaunchOptions appended) goes into `cmd`; StartDir
 *        becomes the working directory; the icon path from the
 *        shortcut is used when present. We deliberately don't route
 *        through `steam://rungameid/<unique_appid>` because that
 *        requires regenerating Steam's grid-ID hash and the user
 *        derives no overlay/input benefit for a Moonlight launch
 *        that runs unattended.
 */
#pragma once

#include "src/platform/common.h"

#include <memory>
#include <string>

namespace steam::sync_shortcuts {

  /**
   * @brief Run one scan-and-write cycle synchronously. Gated by
   *        `config::steam.nonsteam_shortcuts_auto_sync`; no-ops when
   *        the toggle is off or when Steam isn't installed.
   */
  void run_once(const std::string &apps_json_path);

  /**
   * @brief Delete the `nonsg_apps.json` file next to apps_json_path
   *        and refresh proc so the entries disappear from Moonlight.
   *        Wired to the
   *        `/api/state/reset-nonsteam-shortcuts-cache` endpoint.
   */
  void clear_cache(const std::string &apps_json_path);

  /**
   * @brief Start the background worker (~30s polling cadence,
   *        re-scans only when the per-user shortcuts.vdf fingerprint
   *        changes). The worker reads the live toggle each tick so
   *        flipping it via /api/config takes effect without a service
   *        restart.
   */
  std::unique_ptr<platf::deinit_t> start_worker(const std::string &apps_json_path);

}  // namespace steam::sync_shortcuts
