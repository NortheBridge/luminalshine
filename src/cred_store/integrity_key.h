/**
 * @file src/cred_store/integrity_key.h
 * @brief Process-wide HMAC-SHA256 key for tamper detection of Tier A
 *        config files (apps.json, sunshine.conf, paired CA material).
 *
 * The 32-byte key is generated on first use and persisted in a sealed
 * envelope under `<appdata>/integrity.key.sealed`. Sealing prefers the
 * TPM via the existing cred_store::tpm_seal API; on TPM-less hosts the
 * envelope falls back to DPAPI-LocalMachine so the file is unrecoverable
 * across hosts (the same disk on a different machine) but readable by
 * any SYSTEM process on the same host.
 *
 * Threat model: tamper detection. An attacker who can write to the config
 * directory (whether by ACL misconfiguration, OEM image weirdness, or a
 * future regression in the directory-DACL repair) cannot forge a valid
 * HMAC without the sealed key. Encryption-at-rest is NOT the goal — the
 * JSON files this protects don't carry secrets, only paths and command
 * lines the service trusts at execve time.
 *
 * NOT in scope: cross-machine portability. A motherboard swap / TPM
 * clear / OS reinstall invalidates the key; sign-then-verify of existing
 * Tier A files will mismatch and route to quarantine, which seeds
 * defaults. The cred_store carries paired clients separately and is
 * deliberately excluded from this layer.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cred_store::integrity_key {

  /**
   * @brief Active backend identifier for logging / diagnostics.
   * Values: "tpm", "dpapi-lm", "unavailable".
   */
  std::string backend_name();

  /**
   * @brief Return the 32-byte HMAC key. First call generates and seals
   *        the key; subsequent calls return the cached value.
   *        std::nullopt indicates the key could not be initialised on
   *        this host; callers should skip sign/verify rather than
   *        crash.
   */
  std::optional<std::vector<std::uint8_t>> get();

  /**
   * @brief Eagerly initialise so a startup log line surfaces the active
   *        backend. Safe to call multiple times; only the first attempt
   *        does work.
   */
  void prime();

}  // namespace cred_store::integrity_key
