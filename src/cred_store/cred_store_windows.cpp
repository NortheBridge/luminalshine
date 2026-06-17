/**
 * @file src/cred_store/cred_store_windows.cpp
 * @brief Windows Credential Manager backend for the cred_store
 *        abstraction. Replaces cred_store_file.cpp on Windows builds
 *        via CMake conditional.
 *
 * Credentials are stored as a `CRED_TYPE_GENERIC` entry persisted at
 * `CRED_PERSIST_LOCAL_MACHINE` scope. The credential payload is the
 * exact same JSON blob that the file backend would write to
 * sunshine_credentials.json — including the Argon2id record from PR 1.
 *
 * Under the hood, WCM stores the blob in the per-machine vault under
 * `%SystemRoot%\System32\config\systemprofile\AppData\Local\Microsoft\Credentials\`
 * encrypted with DPAPI under the SYSTEM-account master key. On
 * Windows 11 (and Windows Server 2022+) the master key chain is TPM-
 * bound by default, so the credential blob is unrecoverable by an
 * attacker who removes the drive and mounts it on different hardware.
 *
 * The first invocation also runs a one-shot import: if no credential
 * exists at the requested key in WCM but a plaintext file exists on
 * disk at that path, the file's contents are imported into WCM, the
 * file is renamed with a `.migrated-<UTC>` suffix as a forensic
 * recovery anchor, and `cred_store::exists/load` start returning the
 * WCM-backed value.
 *
 * Keys passed in by the credential layer are normalised to the WCM
 * target name `LuminalShine/AdminCredentials` — see `wcm_target_name`
 * below. We deliberately do NOT key WCM entries by file path because
 * a host where the user has overridden `credentials_file` in
 * sunshine.conf should still resolve to the same vault entry.
 */
#include "src/cred_store/cred_store.h"

#include "src/config.h"
#include "src/cred_store/tpm_seal_windows.h"
#include "src/logging.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <wincred.h>
// clang-format on

namespace cred_store {

  namespace {
    constexpr wchar_t kTargetName[] = L"LuminalShine/AdminCredentials";
    constexpr const char *kBackendName = "windows-credential-manager";

    // Guards the one-shot import path. We need this lock because the
    // file-to-WCM migration and a concurrent save_user_creds could
    // race on first call.
    std::mutex &import_mutex() {
      static std::mutex m;
      return m;
    }

    // Per-process cache of whether the one-shot file -> WCM import
    // has run. Stored as an atomic-bool-like state and consulted from
    // every public entry point so the import is at-most-once per
    // process even on hosts that have neither WCM nor file artifacts.
    bool &import_attempted_flag() {
      static bool attempted = false;
      return attempted;
    }

    /// UTF-8 → UTF-16 helper. Cheap; returns empty on conversion
    /// failure (rare for ASCII LuminalShine vault names).
    std::wstring to_wide(std::string_view s) {
      if (s.empty()) {
        return {};
      }
      const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
      if (needed <= 0) {
        return {};
      }
      std::wstring out(static_cast<size_t>(needed), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
      return out;
    }

    std::string utf8_timestamp_now() {
      using namespace std::chrono;
      const auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
      std::time_t tt = static_cast<std::time_t>(secs);
      std::tm utc {};
#ifdef _WIN32
      gmtime_s(&utc, &tt);
#else
      gmtime_r(&tt, &utc);
#endif
      char buf[24] = {};
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
      return buf;
    }

    /// One-shot filename migration for the sunshine_credentials.json ->
    /// luminalshine_credentials.json rename. Only fires when the configured
    /// path is the new default; user-overridden paths are left alone. Copies
    /// the legacy file to the new path before maybe_import_file_once probes
    /// the disk, then archives the legacy file as `.deprecated-<UTC>` so an
    /// operator can recover by hand. Failure modes log and leave both files
    /// in place — the next boot retries idempotently.
    void maybe_migrate_legacy_filename(std::string_view key) {
      if (key.empty()) {
        return;
      }
      namespace fs = std::filesystem;
      fs::path new_path(std::string {key});
      if (new_path.filename() != "luminalshine_credentials.json") {
        return;
      }
      fs::path old_path = new_path.parent_path() / "sunshine_credentials.json";
      std::error_code ec;
      if (fs::exists(new_path, ec)) {
        return;
      }
      if (!fs::exists(old_path, ec)) {
        return;
      }

      std::error_code copy_ec;
      fs::copy_file(old_path, new_path, copy_ec);
      if (copy_ec) {
        BOOST_LOG(warning) << "cred_store(wcm): could not migrate "
                           << old_path.string() << " -> "
                           << new_path.string() << ": "
                           << copy_ec.message();
        return;
      }

      fs::path archive = old_path;
      archive += ".deprecated-" + utf8_timestamp_now();
      std::error_code rename_ec;
      fs::rename(old_path, archive, rename_ec);
      if (rename_ec) {
        BOOST_LOG(warning) << "cred_store(wcm): copied legacy credentials to new "
                           << "path but could not archive original ("
                           << old_path.string() << " -> " << archive.string()
                           << "): " << rename_ec.message();
      } else {
        BOOST_LOG(info) << "cred_store(wcm): renamed legacy "
                        << old_path.filename().string() << " to "
                        << new_path.filename().string() << " (original archived as "
                        << archive.filename().string() << ").";
      }
    }

    /// One-shot import: if `key` looks like a filesystem path AND the
    /// file exists AND no WCM entry is present, load the file, store
    /// it in WCM, and rename the file with a `.migrated-<UTC>` suffix
    /// for forensic recovery. Idempotent and safe to call from any
    /// path.
    void maybe_import_file_once(std::string_view key) {
      std::lock_guard<std::mutex> lk(import_mutex());
      if (import_attempted_flag()) {
        return;
      }
      import_attempted_flag() = true;

      // Run the legacy-filename rename first so the probe below finds the
      // file at the configured (new) path.
      maybe_migrate_legacy_filename(key);

      // Skip if WCM already has the entry — nothing to import over.
      PCREDENTIALW existing = nullptr;
      if (CredReadW(kTargetName, CRED_TYPE_GENERIC, 0, &existing)) {
        if (existing) {
          CredFree(existing);
        }
        return;
      }
      const DWORD read_err = GetLastError();
      if (read_err != ERROR_NOT_FOUND) {
        BOOST_LOG(warning) << "cred_store(wcm): unexpected CredRead error "
                           << read_err << " during import probe";
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

      // Read the legacy file via the property tree so a corrupt blob
      // is detected before we go to all the trouble of writing it
      // into WCM.
      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_json(legacy_path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "cred_store(wcm): legacy credentials file at "
                           << legacy_path.string() << " is unparseable ("
                           << e.what() << "); skipping import. The Web UI's "
                           << "create-first-user flow will run on next visit.";
        return;
      }

      std::ostringstream blob;
      try {
        boost::property_tree::write_json(blob, tree);
      } catch (...) {
        return;
      }

      const auto wide = to_wide(blob.str());
      (void) wide;  // CredWrite takes a byte buffer, not a wide string;
                   // we send the UTF-8 bytes directly so the WCM read
                   // path can round-trip the same JSON we'd have written
                   // to disk.

      CREDENTIALW cred {};
      cred.Type = CRED_TYPE_GENERIC;
      cred.TargetName = const_cast<LPWSTR>(kTargetName);
      cred.CredentialBlobSize = static_cast<DWORD>(blob.str().size());
      cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.str().data()));
      cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
      // UserName isn't part of the auth payload — it's just metadata
      // surfaced in the Credential Manager UI. Use a stable label.
      std::wstring username_label = L"LuminalShine";
      cred.UserName = username_label.data();

      if (!CredWriteW(&cred, 0)) {
        BOOST_LOG(error) << "cred_store(wcm): CredWrite during import failed (err="
                         << GetLastError() << "). Legacy file left in place.";
        return;
      }

      // Rename the legacy file as `.migrated-<UTC>` so an operator can
      // verify the import succeeded and recover from a corrupted WCM
      // by hand if necessary.
      fs::path archive_path = legacy_path;
      archive_path += ".migrated-" + utf8_timestamp_now();
      std::error_code rename_ec;
      fs::rename(legacy_path, archive_path, rename_ec);
      if (rename_ec) {
        BOOST_LOG(warning) << "cred_store(wcm): imported credentials into WCM but "
                           << "could not rename source file ("
                           << legacy_path.string() << " -> "
                           << archive_path.string() << "): "
                           << rename_ec.message();
      } else {
        BOOST_LOG(info) << "cred_store(wcm): migrated legacy credentials from "
                        << legacy_path.string() << " into Windows Credential Manager "
                        << "(archived as " << archive_path.string() << ").";
      }
    }
  }  // namespace

  std::string backend_name() {
    if (config::sunshine.tpm_binding && tpm_seal::available()) {
      return std::string {kBackendName} + " + tpm";
    }
    return kBackendName;
  }

  std::string default_key() {
    return config::sunshine.credentials_file;
  }

  bool exists(std::string_view key) {
    maybe_import_file_once(key);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(kTargetName, CRED_TYPE_GENERIC, 0, &cred)) {
      return false;
    }
    if (cred) {
      CredFree(cred);
    }
    return true;
  }

  bool load(std::string_view key, std::string &out) {
    out.clear();
    maybe_import_file_once(key);

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(kTargetName, CRED_TYPE_GENERIC, 0, &cred)) {
      const DWORD err = GetLastError();
      if (err != ERROR_NOT_FOUND) {
        BOOST_LOG(warning) << "cred_store(wcm): CredRead failed (err=" << err << ")";
      }
      return false;
    }
    if (!cred) {
      return false;
    }
    std::string raw;
    if (cred->CredentialBlobSize > 0 && cred->CredentialBlob) {
      raw.assign(
        reinterpret_cast<const char *>(cred->CredentialBlob),
        static_cast<size_t>(cred->CredentialBlobSize)
      );
    }
    // CredFree wipes the structure but not necessarily the blob; the
    // caller is expected to SecureZeroMemory the returned string when
    // done (PR 6 lands that discipline on the verification paths).
    CredFree(cred);
    if (raw.empty()) {
      // Self-heal: CredRead returned success with a zero-byte blob, which
      // is broken state — the entry registered but holds nothing usable.
      // Without cleaning up, the next start sees the same broken entry,
      // the web UI shows a login form (because the entry "exists") that
      // can never authenticate, and the user is locked out. Erase it so
      // the next start observes "no entry" → first-time setup → recovery.
      // The same self-heal pattern handles the matching reload_user_creds
      // case in src/httpcommon.cpp where the blob loads non-empty but
      // fails to parse.
      BOOST_LOG(warning) << "cred_store(wcm): credential entry has empty blob; "
                         << "erasing so first-time setup can recover.";
      if (!CredDeleteW(kTargetName, CRED_TYPE_GENERIC, 0)) {
        BOOST_LOG(warning) << "cred_store(wcm): CredDelete during self-heal failed "
                           << "(err=" << GetLastError() << "); manual reset may be "
                           << "needed via the Reset Admin Password shortcut.";
      }
      return false;
    }

    // Transparently unseal if the entry was written by a TPM-binding
    // build. We do this regardless of the current tpm_binding setting
    // — flipping the toggle to "off" should still let the user log in
    // with credentials that were last written under "on", and a
    // subsequent save will rewrite them as plain.
    if (tpm_seal::looks_sealed(raw)) {
      if (tpm_seal::unseal(raw, out)) {
        return !out.empty();
      }

      // One-shot retry after a short pause. The previous failure mode
      // we're absorbing here is a sub-second TPM / Microsoft Platform
      // Crypto Provider driver hiccup where NCryptOpenKey or
      // NCryptDecrypt briefly errors out and then recovers on the very
      // next call (observed during system suspend/resume and right
      // after a Windows credential subsystem service restart). Without
      // this, the diagnostic path below would proceed to roundtrip-
      // probe and possibly self-heal-erase a perfectly recoverable
      // blob just because the first call landed during a 100-ms
      // provider blip. 250ms is well past the observed glitch windows
      // and is paid only on the cold-failure path — the happy path is
      // unaffected.
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (tpm_seal::unseal(raw, out)) {
        BOOST_LOG(info) << "cred_store(wcm): TPM unseal recovered on retry "
                        << "after a brief provider glitch.";
        return !out.empty();
      }

      // Persistent failure. Ask tpm_seal to tell us *why* so we can
      // distinguish "orphaned blob — safe to erase" from "transient
      // TPM unavailability — preserve the blob and let the user use
      // the Reset Admin Password shortcut."
      using tpm_seal::UnsealFailureCause;
      const auto cause = tpm_seal::diagnose_unseal_failure(raw);
      switch (cause) {
        case UnsealFailureCause::KeyMissing: {
          // No persisted wrapping key on this host. The wrapped blob
          // is orphaned — no possible key value can ever decrypt it.
          // Self-heal: erase the blob so the next start observes
          // "no entry" → first-time setup → recovery. The user does
          // not need to use the Reset Admin Password shortcut; the
          // setup screen will appear automatically.
          BOOST_LOG(warning) << "cred_store(wcm): the credential blob is "
                             << "TPM-sealed but no matching wrapping key "
                             << "exists on this host (key was deleted, "
                             << "sysprep'd, or the NCrypt store was reset). "
                             << "The blob is unrecoverable; erasing it so "
                             << "first-time setup can recover. Paired "
                             << "clients and apps are unaffected.";
          if (!CredDeleteW(kTargetName, CRED_TYPE_GENERIC, 0)) {
            BOOST_LOG(warning) << "cred_store(wcm): CredDelete during orphan "
                               << "self-heal failed (err="
                               << GetLastError() << "); manual reset may be "
                               << "needed via the Reset Admin Password shortcut.";
          }
          return false;
        }
        case UnsealFailureCause::BindingMismatch: {
          // Persisted key is live (roundtrip probe succeeded) but the
          // stored blob was wrapped under a different key. This is
          // the "silent key rotation" pathology that the
          // open_or_create_key fix in tpm_seal_windows.cpp now
          // prevents going forward — but pre-fix installs may have
          // already wrapped credentials under a rotated-away key.
          // Self-heal: erase the orphan blob so the user lands at
          // first-time setup.
          BOOST_LOG(warning) << "cred_store(wcm): the credential blob is "
                             << "TPM-sealed under a different wrapping key "
                             << "than the one currently persisted on this "
                             << "host (silent rotation by an earlier build). "
                             << "The blob is unrecoverable; erasing it so "
                             << "first-time setup can recover. Paired "
                             << "clients and apps are unaffected.";
          if (!CredDeleteW(kTargetName, CRED_TYPE_GENERIC, 0)) {
            BOOST_LOG(warning) << "cred_store(wcm): CredDelete during binding-"
                               << "mismatch self-heal failed (err="
                               << GetLastError() << "); manual reset may be "
                               << "needed via the Reset Admin Password shortcut.";
          }
          return false;
        }
        case UnsealFailureCause::KeyTransientlyUnavailable:
        case UnsealFailureCause::NotApplicable:
        default:
          // PRESERVE the blob. The persisted key might come back when
          // the underlying provider clears whatever it's blocked on
          // (BitLocker recovery prompt, suspended TPM, AV scan
          // holding a handle, etc.). Auto-deletion here would
          // permanently destroy a credential that next boot would
          // recover. The user-driven Reset Admin Password shortcut
          // remains the manual escape for the truly-unrecoverable
          // case. (This branch preserves the historical "Do NOT
          // auto-delete on unseal failure" guarantee for everything
          // we can't positively classify as orphan-or-mismatch.)
          BOOST_LOG(error) << "cred_store(wcm): credential entry is TPM-sealed "
                           << "but unseal failed and the wrapping key state "
                           << "could not be confirmed (likely a transient "
                           << "TPM / Microsoft Platform Crypto Provider "
                           << "issue). The credential is being preserved in "
                           << "case the provider recovers on the next start. "
                           << "If the issue persists, use the Reset "
                           << "LuminalShine Admin Password shortcut in the "
                           << "Start Menu to recover (preserves paired "
                           << "clients and apps; only the admin login is reset).";
          return false;
      }
    }
    out = std::move(raw);
    return !out.empty();
  }

  bool store(std::string_view key, std::string_view blob) {
    (void) key;  // WCM target name is fixed; the per-config key is
                 // only consulted for the legacy-file import probe.
    maybe_import_file_once(key);

    // Decide whether to wrap the blob with the TPM before persistence.
    // tpm_binding is default-true on Windows; when the TPM is missing
    // we silently store plaintext (matches the pre-PR-5 behaviour) so
    // a TPM-less host is never bricked by a config default.
    std::string sealed;
    std::string_view to_store = blob;
    if (config::sunshine.tpm_binding && tpm_seal::available()) {
      if (tpm_seal::seal(blob, sealed)) {
        to_store = sealed;
      } else {
        BOOST_LOG(warning) << "cred_store(wcm): TPM seal failed; falling back to "
                           << "unwrapped storage for this write.";
      }
    }

    CREDENTIALW cred {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(kTargetName);
    cred.CredentialBlobSize = static_cast<DWORD>(to_store.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(to_store.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    std::wstring username_label = L"LuminalShine";
    cred.UserName = username_label.data();

    if (!CredWriteW(&cred, 0)) {
      const DWORD err = GetLastError();
      BOOST_LOG(error) << "cred_store(wcm): CredWrite failed (err=" << err << ")";
      return false;
    }
    return true;
  }

  bool erase(std::string_view key) {
    (void) key;
    bool ok = true;
    if (!CredDeleteW(kTargetName, CRED_TYPE_GENERIC, 0)) {
      const DWORD err = GetLastError();
      if (err != ERROR_NOT_FOUND) {
        BOOST_LOG(warning) << "cred_store(wcm): CredDelete failed (err=" << err << ")";
        ok = false;
      }
    } else {
      BOOST_LOG(info) << "cred_store(wcm): credential entry " << "LuminalShine/AdminCredentials"
                      << " removed.";
    }
    // Also delete the TPM-bound wrapping key. A future credential save
    // will regenerate it (cheap on TPM 2.0 hardware) so this gives the
    // user a fully clean slate without breaking subsequent operations.
    if (!tpm_seal::clear()) {
      ok = false;
    }
    return ok;
  }

}  // namespace cred_store
