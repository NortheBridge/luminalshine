/**
 * @file src/cred_store/cred_store.h
 * @brief Storage backend interface for admin credentials.
 *
 * Abstracts "where the credential blob lives" so the verifier and the
 * save path don't have to care about whether the bytes are in a JSON
 * file, the Windows Credential Manager vault, GNOME's libsecret, or
 * the macOS Keychain. The credential blob itself is opaque to this
 * layer — it's whatever JSON shape `save_user_creds` decides to write
 * (see PR 1: versioned records with Argon2id parameters).
 *
 * Backend selection is one-shot at process start. Each platform picks
 * its system secret store; an unconfigured / unavailable system store
 * silently falls back to the file backend so LuminalShine still works
 * on a stripped-down host.
 *
 * Keys are opaque strings. The file backend treats them as filesystem
 * paths (preserving existing behaviour). Other backends use them as
 * vault target names (e.g. "LuminalShine/AdminCredentials" on Windows
 * Credential Manager). A `default_key()` helper resolves the canonical
 * value for the active backend so call sites don't have to know.
 */
#pragma once

#include <string>
#include <string_view>

namespace cred_store {

  /**
   * @return Stable diagnostic identifier of the active backend
   *         ("file", "windows-credential-manager", "libsecret",
   *         "macos-keychain"). Surfaced via /api/health/cred-store.
   */
  std::string backend_name();

  /**
   * @return Canonical key for the admin-credentials record on the
   *         active backend. File backend returns
   *         `config::sunshine.credentials_file`; other backends
   *         return a stable target name like
   *         "LuminalShine/AdminCredentials".
   */
  std::string default_key();

  /**
   * @brief Whether a credential record exists at @p key.
   *        Does NOT decrypt the blob; only checks presence.
   *        Returns false on any backend error (no record / no access /
   *        backend down).
   */
  bool exists(std::string_view key);

  /**
   * @brief Load the credential blob at @p key into @p out.
   *        @p out is overwritten on success and cleared on failure.
   *        Caller is responsible for wiping @p out with SecureZeroMemory
   *        (PR 6) once finished — the backend does not retain any copy.
   * @return true on success.
   */
  bool load(std::string_view key, std::string &out);

  /**
   * @brief Persist @p blob at @p key. Atomic with respect to crash /
   *        power loss on the file backend (temp + fsync + rename) and
   *        atomic by API contract on the system-store backends.
   *        Overwrites any existing value at @p key.
   * @return true on success.
   */
  bool store(std::string_view key, std::string_view blob);

  /**
   * @brief Remove the credential record at @p key. Used by the
   *        "Reset Admin Credentials" Troubleshooting flow (PR 3) and
   *        by the MSI uninstall custom action (PR 6).
   * @return true on success or "did not exist"; false on hard errors.
   */
  bool erase(std::string_view key);

}  // namespace cred_store
