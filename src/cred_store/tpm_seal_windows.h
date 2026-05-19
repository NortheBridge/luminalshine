/**
 * @file src/cred_store/tpm_seal_windows.h
 * @brief TPM-bound seal / unseal helpers for the Windows cred_store
 *        backend. Wrap an AES-256-GCM key with a non-exportable RSA-2048
 *        key sealed in the TPM via the `Microsoft Platform Crypto
 *        Provider` KSP; encrypt the credential blob under that AES key;
 *        emit a single binary envelope that the WCM backend stores as-is.
 *
 * The envelope is recognisable by a four-byte `TPM1` magic prefix, so
 * the load path can transparently distinguish TPM-sealed payloads from
 * pre-PR-5 plaintext entries written by older builds. That makes the
 * `tpm_binding` config toggle transparently migrate-on-next-save: load
 * always unwraps if the magic is present; save wraps only when the
 * toggle is on.
 *
 * Threat-model effect: the wrapping key cannot leave the TPM. An
 * attacker who acquires the credential bytes (e.g. cold-drive recovery
 * of the WCM database) cannot decrypt them without re-presenting the
 * same physical TPM device. This sits on top of the DPAPI sealing the
 * WCM already provides; the value of this layer is that DPAPI master
 * keys can in principle be exported by a determined attacker with
 * SYSTEM access, whereas the TPM-bound RSA key cannot — `NCryptExport`
 * on it returns `NTE_NOT_SUPPORTED` because the key was finalised with
 * `NCRYPT_EXPORT_POLICY_PROPERTY = 0`.
 *
 * NOT in scope here:
 *   - PCR policy binding (PCR 11 etc.). The TPM key is host-bound but
 *     not boot-state-bound. A determined attacker who boots the same
 *     machine from a different OS can still unseal. Tracked as a
 *     follow-up if the security requirement tightens.
 *   - Pairings: pairings live in `sunshine_state.json`, not in WCM, and
 *     are explicitly NOT TPM-bound — they survive credential resets and
 *     TPM clears by design.
 */
#pragma once

#include <string>
#include <string_view>

namespace cred_store::tpm_seal {

  /**
   * @brief Whether TPM sealing is reachable on this host. Probes the
   *        Microsoft Platform Crypto Provider (TPM 2.0 KSP) once per
   *        process and caches the result. Returns false when the TPM
   *        is absent / disabled / the KSP cannot be opened.
   */
  bool available();

  /**
   * @brief Whether @p blob carries the TPM1 envelope magic. Cheap (4-byte
   *        memcmp). Used by the load path to decide whether to unwrap.
   */
  bool looks_sealed(std::string_view blob);

  /**
   * @brief Wrap @p plaintext with a TPM-bound RSA key + AES-256-GCM and
   *        write the envelope to @p sealed_out. Idempotent only on
   *        plaintext input — calling seal() on an already-sealed blob
   *        will produce a double-sealed envelope (the load path doesn't
   *        chase chains, so don't do this).
   * @return true on success. On false @p sealed_out is cleared and the
   *         caller should fall back to storing the plaintext.
   */
  bool seal(std::string_view plaintext, std::string &sealed_out);

  /**
   * @brief Unwrap a TPM1 envelope from @p sealed into @p plaintext_out.
   *        Returns false if the magic is missing, the TPM cannot decrypt
   *        the wrapped key (e.g. drive moved to a different host), or
   *        AES-GCM authentication fails.
   */
  bool unseal(std::string_view sealed, std::string &plaintext_out);

}  // namespace cred_store::tpm_seal
