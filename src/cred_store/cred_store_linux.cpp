/**
 * @file src/cred_store/cred_store_linux.cpp
 * @brief libsecret (Secret Service / GNOME Keyring / KWallet via the
 *        Secret Service D-Bus API) backend for the cred_store layer.
 *
 * Selected by CMake when `pkg_check_modules(libsecret-1)` succeeds
 * during configure. On hosts where libsecret is installed but the
 * Secret Service daemon is unreachable at runtime (no D-Bus session,
 * sandboxed Flatpak without the portal, headless service account
 * without a logged-in user) we silently fall back to the file backend
 * so LuminalShine still has somewhere to persist credentials.
 *
 * Credentials are stored in the user's default keyring as a single
 * entry tagged with the schema name
 * `io.northebridge.LuminalShine.AdminCredentials`. The credential
 * payload is the exact same JSON blob the file backend would write
 * (including the Argon2id record from PR 1).
 *
 * The first invocation also runs the same one-shot import as the
 * Windows backend: if no libsecret entry exists at the schema but a
 * legacy plaintext file is on disk at the configured path, the file's
 * contents are imported into libsecret, the file is renamed with a
 * `.migrated-<UTC>` suffix as a forensic recovery anchor, and
 * `cred_store::exists/load` start returning the libsecret value.
 */
#include "src/cred_store/cred_store.h"

#include "src/config.h"
#include "src/cred_store/file_backend.h"
#include "src/logging.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <libsecret/secret.h>
#include <mutex>
#include <sstream>
#include <fstream>
#include <string>

namespace cred_store {

  namespace {
    constexpr const char *kBackendLibsecret = "libsecret";
    constexpr const char *kBackendFileFallback = "file (libsecret unavailable)";
    constexpr const char *kSecretLabel = "LuminalShine Admin Credentials";

    // Schema name doubles as the lookup key (no attributes — we keep
    // exactly one entry in the keyring under this schema).
    // SECRET_SCHEMA_NONE means libsecret filters by schema name so we
    // don't collide with other services on the same keyring.
    const SecretSchema *credentials_schema() {
      static const SecretSchema schema = {
        "io.northebridge.LuminalShine.AdminCredentials",
        SECRET_SCHEMA_NONE,
        {
          {nullptr, static_cast<SecretSchemaAttributeType>(0)},
        },
      };
      return &schema;
    }

    std::mutex &init_mutex() {
      static std::mutex m;
      return m;
    }

    /// Latched on first successful or failed probe. True means the
    /// secret service is reachable; false means we permanently route
    /// to the file backend for the rest of the process lifetime.
    bool &libsecret_available_flag() {
      static bool available = false;
      return available;
    }

    bool &init_done_flag() {
      static bool done = false;
      return done;
    }

    bool &import_attempted_flag() {
      static bool attempted = false;
      return attempted;
    }

    std::string utf8_timestamp_now() {
      using namespace std::chrono;
      const auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
      std::time_t tt = static_cast<std::time_t>(secs);
      std::tm utc {};
      gmtime_r(&tt, &utc);
      char buf[24] = {};
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
      return buf;
    }

    /// Probe the Secret Service via libsecret. On failure (typically
    /// G_DBUS_ERROR_SERVICE_UNKNOWN or SECRET_ERROR_PROTOCOL), latch
    /// the file-fallback flag and warn once.
    void probe_libsecret_once() {
      std::lock_guard<std::mutex> lk(init_mutex());
      if (init_done_flag()) {
        return;
      }
      init_done_flag() = true;

      GError *err = nullptr;
      SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &err);
      if (!svc || err) {
        BOOST_LOG(warning) << "cred_store(libsecret): secret service unavailable ("
                           << (err ? err->message : "no service handle")
                           << "); falling back to file backend.";
        if (err) {
          g_error_free(err);
        }
        libsecret_available_flag() = false;
        return;
      }
      g_object_unref(svc);
      libsecret_available_flag() = true;
    }

    bool use_file_fallback() {
      probe_libsecret_once();
      return !libsecret_available_flag();
    }

    /// One-shot migration: if libsecret is reachable AND no entry exists
    /// yet AND a legacy plaintext file is present at @p key, copy the
    /// file's contents into libsecret and rename the source with a
    /// `.migrated-<UTC>` suffix. Idempotent.
    void maybe_import_file_once(std::string_view key) {
      std::lock_guard<std::mutex> lk(init_mutex());
      if (import_attempted_flag()) {
        return;
      }
      import_attempted_flag() = true;
      if (!libsecret_available_flag()) {
        return;
      }

      GError *err = nullptr;
      gchar *existing = secret_password_lookup_sync(credentials_schema(), nullptr, &err, NULL);
      if (existing) {
        secret_password_free(existing);
        if (err) {
          g_error_free(err);
        }
        return;
      }
      if (err) {
        BOOST_LOG(warning) << "cred_store(libsecret): import probe lookup failed: "
                           << err->message;
        g_error_free(err);
        return;
      }

      if (key.empty()) {
        return;
      }
      namespace fs = std::filesystem;
      fs::path legacy_path(std::string {key});
      std::error_code ec;
      if (!fs::exists(legacy_path, ec)) {
        return;
      }

      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_json(legacy_path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "cred_store(libsecret): legacy credentials file at "
                           << legacy_path.string() << " is unparseable ("
                           << e.what() << "); skipping import.";
        return;
      }

      std::ostringstream blob;
      try {
        boost::property_tree::write_json(blob, tree);
      } catch (...) {
        return;
      }

      GError *store_err = nullptr;
      gboolean ok = secret_password_store_sync(
        credentials_schema(),
        SECRET_COLLECTION_DEFAULT,
        kSecretLabel,
        blob.str().c_str(),
        nullptr,
        &store_err,
        NULL
      );
      if (!ok || store_err) {
        BOOST_LOG(error) << "cred_store(libsecret): import store failed: "
                         << (store_err ? store_err->message : "unknown");
        if (store_err) {
          g_error_free(store_err);
        }
        return;
      }

      fs::path archive_path = legacy_path;
      archive_path += ".migrated-" + utf8_timestamp_now();
      std::error_code rename_ec;
      fs::rename(legacy_path, archive_path, rename_ec);
      if (rename_ec) {
        BOOST_LOG(warning) << "cred_store(libsecret): imported into keyring but "
                           << "could not rename source file ("
                           << legacy_path.string() << " -> "
                           << archive_path.string() << "): "
                           << rename_ec.message();
      } else {
        BOOST_LOG(info) << "cred_store(libsecret): migrated legacy credentials from "
                        << legacy_path.string() << " into the Secret Service keyring "
                        << "(archived as " << archive_path.string() << ").";
      }
    }
  }  // namespace

  std::string backend_name() {
    return use_file_fallback() ? kBackendFileFallback : kBackendLibsecret;
  }

  std::string default_key() {
    return config::sunshine.credentials_file;
  }

  bool exists(std::string_view key) {
    if (use_file_fallback()) {
      return file_backend::exists(key);
    }
    maybe_import_file_once(key);

    GError *err = nullptr;
    gchar *value = secret_password_lookup_sync(credentials_schema(), nullptr, &err, NULL);
    if (err) {
      BOOST_LOG(warning) << "cred_store(libsecret): exists lookup failed: " << err->message;
      g_error_free(err);
      return false;
    }
    if (!value) {
      return false;
    }
    secret_password_free(value);
    return true;
  }

  bool load(std::string_view key, std::string &out) {
    out.clear();
    if (use_file_fallback()) {
      return file_backend::load(key, out);
    }
    maybe_import_file_once(key);

    GError *err = nullptr;
    gchar *value = secret_password_lookup_sync(credentials_schema(), nullptr, &err, NULL);
    if (err) {
      BOOST_LOG(warning) << "cred_store(libsecret): load failed: " << err->message;
      g_error_free(err);
      return false;
    }
    if (!value) {
      return false;
    }
    out.assign(value);
    secret_password_free(value);
    return !out.empty();
  }

  bool store(std::string_view key, std::string_view blob) {
    if (use_file_fallback()) {
      return file_backend::store(key, blob);
    }
    maybe_import_file_once(key);

    std::string payload {blob};
    GError *err = nullptr;
    gboolean ok = secret_password_store_sync(
      credentials_schema(),
      SECRET_COLLECTION_DEFAULT,
      kSecretLabel,
      payload.c_str(),
      nullptr,
      &err,
      NULL
    );
    if (!ok || err) {
      BOOST_LOG(error) << "cred_store(libsecret): store failed: "
                       << (err ? err->message : "unknown");
      if (err) {
        g_error_free(err);
      }
      return false;
    }
    return true;
  }

  bool erase(std::string_view key) {
    if (use_file_fallback()) {
      return file_backend::erase(key);
    }
    GError *err = nullptr;
    gboolean removed = secret_password_clear_sync(credentials_schema(), nullptr, &err, NULL);
    if (err) {
      BOOST_LOG(warning) << "cred_store(libsecret): erase failed: " << err->message;
      g_error_free(err);
      return false;
    }
    if (removed) {
      BOOST_LOG(info) << "cred_store(libsecret): credential entry removed.";
    }
    return true;
  }

  probe_result probe(std::string_view key) {
    if (!exists(key)) {
      return probe_result::absent;
    }
    std::string blob;
    const bool loadable = load(key, blob) && !blob.empty();
    return loadable ? probe_result::present_loadable : probe_result::present_unloadable;
  }

  bool quarantine(std::string_view key, std::string_view blob) {
    // Legacy platform: preserve the suspect bytes next to the configured
    // credentials file. Raw write on purpose — the blob may not be JSON.
    std::ofstream out(std::string(key) + ".quarantine", std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    return out.good();
  }

}  // namespace cred_store
