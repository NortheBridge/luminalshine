/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"

namespace http {

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  int reload_user_creds(const std::string &file);

  /**
   * @brief Clear the in-RAM credential fields (username/password/salt/kdf)
   *        under the credential RAM lock. Use instead of clearing the
   *        config::sunshine fields directly — direct writes race
   *        verify_user_password's snapshot.
   */
  void clear_user_creds_in_ram();

  /**
   * @brief Whether a stored admin credential record exists but is
   *        currently unloadable (e.g. TPM transiently unavailable at
   *        service start). While true, first-user setup must be refused
   *        — accepting new credentials would overwrite the real record —
   *        and the auth-status API should report the locked state
   *        instead of offering setup. Cleared by the background retry
   *        worker once the record loads (or disappears).
   */
  bool user_creds_locked();

  /**
   * @brief Verify a plaintext username + password against the in-memory
   *        credential record loaded by `reload_user_creds`.
   *
   * Dispatches to the appropriate KDF based on `config::sunshine.password_kdf`
   * (Argon2id or legacy SHA-256), compares the computed hash with the
   * stored value, and — on a successful match against a legacy SHA-256
   * record — opportunistically upgrades the on-disk record to Argon2id
   * using the configured Argon2 parameters. The upgrade is best-effort;
   * a write failure does not change the login outcome.
   *
   * Username comparison is case-insensitive (matches existing behaviour
   * at the three login call sites).
   *
   * @return true iff the credentials matched.
   */
  bool verify_user_password(const std::string &username, const std::string &password);

  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  bool configure_curl_tls(CURL *curl);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);
  std::string cookie_escape(const std::string &value);
  std::string cookie_unescape(const std::string &value);

  extern std::string unique_id;
  extern net::net_e origin_web_ui_allowed;

#ifdef _WIN32
  extern std::string shared_virtual_display_guid;
#endif

  // Update origin ACL from current config
  void refresh_origin_acl();

}  // namespace http
