/**
 * @file src/cred_store/file_backend.h
 * @brief Internal-use file-storage helpers for the cred_store layer.
 *
 * Not part of the public cred_store:: API. The Linux libsecret backend
 * delegates to these helpers on hosts where the secret service is
 * unavailable at runtime (no D-Bus session keyring, headless service
 * account, sandboxed Flatpak without portal access). The Windows and
 * macOS backends do not use this layer because their system stores are
 * always available — they only consult the file path for the one-shot
 * legacy-credentials import.
 *
 * Functions accept filesystem paths in the `path` parameter. Behaviour
 * matches the pre-PR-2 file path exactly: atomic writes go through
 * `statefile::atomic_write_json` (temp + fsync + rename + .bak rotation)
 * and reads through `statefile::load_or_recover` (primary then .bak).
 */
#pragma once

#include <string>
#include <string_view>

namespace cred_store::file_backend {

  /**
   * @brief Whether a credential file exists at @p path. Returns true if
   *        either the primary file or its `.bak` recovery sibling is
   *        present, since `load` will succeed in both cases.
   */
  bool exists(std::string_view path);

  /**
   * @brief Load credential JSON from @p path into @p out. @p out is
   *        cleared on failure. Uses `statefile::load_or_recover` so a
   *        primary-missing-but-bak-present state still succeeds.
   */
  bool load(std::string_view path, std::string &out);

  /**
   * @brief Persist @p blob as JSON at @p path with crash-safe atomic
   *        write semantics. Rejects malformed JSON before writing so a
   *        corrupted in-memory blob can't be propagated to disk.
   */
  bool store(std::string_view path, std::string_view blob);

  /**
   * @brief Remove the primary file and its `.bak` sibling at @p path.
   *        Treats "did not exist" as success.
   */
  bool erase(std::string_view path);

}  // namespace cred_store::file_backend
