/**
 * @file src/integrity.h
 * @brief Sign/verify/quarantine helpers for Tier A config files.
 *
 * Tier A = files where tampering yields code execution or auth bypass
 * under the SYSTEM service identity (apps.json, sunshine.conf, the TLS
 * CA material). Each Tier A write goes through `write_signed`, which
 * emits a sidecar `<path>.sig` containing the hex HMAC-SHA256 of the
 * file contents under a per-host key (TPM-sealed when available,
 * DPAPI-LocalMachine otherwise). Each Tier A load goes through
 * `verify`; mismatches route to `quarantine` which renames the
 * suspect file with a `.tamper-<UTC>` suffix so the caller can fall
 * back to defaults without losing the forensic artifact.
 *
 * This layer is orthogonal to the DACL/MIC hardening in
 * platf::harden_config_directory — the DACL stops the typical local
 * non-admin attacker; the HMAC catches the residual case where a
 * future regression in the DACL repair leaves the file briefly
 * writable.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace integrity {

  enum class verify_status {
    ok,             ///< Content matches sidecar HMAC.
    not_protected,  ///< Sidecar absent — pre-migration or never signed.
    mismatch,       ///< Sidecar present, HMAC does not match (tamper).
    unreadable,     ///< I/O failure reading file or sidecar.
    no_key,         ///< Integrity backend unavailable on this host.
  };

  /**
   * @brief Atomically write @p contents to @p path and emit the
   *        companion `.sig` sidecar. When the integrity backend is
   *        unavailable, falls back to a plain atomic write so the
   *        caller's existing behaviour is preserved on TPM/DPAPI-less
   *        hosts.
   */
  bool write_signed(const std::filesystem::path &path, std::string_view contents);

  /**
   * @brief Verify that @p path matches its sidecar's HMAC.
   */
  verify_status verify(const std::filesystem::path &path);

  /**
   * @brief Rename @p path (and its `.sig` sidecar, if any) to
   *        `<original>.tamper-<UTC>`. Returns the archive path on
   *        success, empty on failure.
   */
  std::filesystem::path quarantine(const std::filesystem::path &path);

  /// Active backend identifier for diagnostics ("tpm", "dpapi-lm", "unavailable").
  std::string backend_name();

}  // namespace integrity
