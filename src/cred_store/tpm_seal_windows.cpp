/**
 * @file src/cred_store/tpm_seal_windows.cpp
 * @brief Implementation of the TPM seal / unseal helpers used by the
 *        Windows cred_store backend when `config.sunshine.tpm_binding`
 *        is enabled (default ON on Windows 10/11 with TPM 2.0 present).
 *
 * Envelope wire format (binary, big-endian numerics):
 *
 *   [4 bytes]  magic = 'T' 'P' 'M' '1'
 *   [2 bytes]  wrapped_key_len N      (uint16 BE; 256 for RSA-2048)
 *   [N bytes]  wrapped_key            (NCryptEncrypt RSA-OAEP-SHA256 of the AES key)
 *   [12 bytes] iv                     (random per-seal nonce)
 *   [16 bytes] tag                    (AES-GCM auth tag)
 *   [...]      ciphertext             (AES-GCM(blob))
 *
 * The fixed-size header up to the ciphertext is 4 + 2 + N + 12 + 16
 * bytes (290 for RSA-2048). The ciphertext is the same length as the
 * plaintext under GCM, so there's no padding to strip on the unseal
 * path.
 */
#include "src/cred_store/tpm_seal_windows.h"

#include "src/logging.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
// clang-format on

#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "bcrypt.lib")

namespace cred_store::tpm_seal {

  namespace {
    constexpr char kMagic[4] = {'T', 'P', 'M', '1'};
    constexpr DWORD kAesKeyLen = 32;       // AES-256
    constexpr DWORD kIvLen = 12;            // GCM nonce
    constexpr DWORD kTagLen = 16;           // GCM auth tag
    constexpr LPCWSTR kKeyName = L"LuminalShineAdminCredentialsKey";

    std::mutex &probe_mutex() {
      static std::mutex m;
      return m;
    }

    bool &probe_done() {
      static bool done = false;
      return done;
    }

    bool &probe_result() {
      static bool ok = false;
      return ok;
    }

    /// Open the platform crypto provider once and cache the answer.
    /// Subsequent calls re-open on demand (the per-call overhead is
    /// negligible compared to RSA decrypt).
    bool open_provider(NCRYPT_PROV_HANDLE &out_prov) {
      const SECURITY_STATUS s = NCryptOpenStorageProvider(
        &out_prov, MS_PLATFORM_CRYPTO_PROVIDER, 0
      );
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "tpm_seal: NCryptOpenStorageProvider failed (0x"
                         << std::hex << s << ")";
        return false;
      }
      return true;
    }

    /// Open the persisted credential-wrapping key, creating it if it
    /// doesn't exist. Created keys are non-exportable, machine-scoped,
    /// and 2048-bit RSA.
    bool open_or_create_key(NCRYPT_PROV_HANDLE prov, NCRYPT_KEY_HANDLE &out_key) {
      SECURITY_STATUS s = NCryptOpenKey(prov, &out_key, kKeyName, 0, NCRYPT_MACHINE_KEY_FLAG);
      if (s == ERROR_SUCCESS) {
        return true;
      }
      if (s != NTE_BAD_KEYSET) {
        BOOST_LOG(warning) << "tpm_seal: NCryptOpenKey failed (0x"
                           << std::hex << s << "); attempting create.";
      }

      s = NCryptCreatePersistedKey(
        prov, &out_key, BCRYPT_RSA_ALGORITHM, kKeyName, 0,
        NCRYPT_MACHINE_KEY_FLAG
      );
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(error) << "tpm_seal: NCryptCreatePersistedKey failed (0x"
                         << std::hex << s << ")";
        return false;
      }

      DWORD key_length = 2048;
      s = NCryptSetProperty(
        out_key, NCRYPT_LENGTH_PROPERTY,
        reinterpret_cast<PBYTE>(&key_length), sizeof(key_length), 0
      );
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(error) << "tpm_seal: NCryptSetProperty(length) failed (0x"
                         << std::hex << s << ")";
        NCryptDeleteKey(out_key, 0);
        return false;
      }

      // Lock the key as non-exportable. NCRYPT_EXPORT_POLICY_PROPERTY = 0
      // means NCryptExportKey will refuse to dump it under any of the
      // standard blob types, including BCRYPT_PRIVATE_KEY_BLOB.
      DWORD export_policy = 0;
      s = NCryptSetProperty(
        out_key, NCRYPT_EXPORT_POLICY_PROPERTY,
        reinterpret_cast<PBYTE>(&export_policy), sizeof(export_policy), 0
      );
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "tpm_seal: NCryptSetProperty(export_policy) failed (0x"
                           << std::hex << s << ")";
      }

      // Allow decrypt only; we don't sign with this key.
      DWORD usage = NCRYPT_ALLOW_DECRYPT_FLAG;
      s = NCryptSetProperty(
        out_key, NCRYPT_KEY_USAGE_PROPERTY,
        reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0
      );
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "tpm_seal: NCryptSetProperty(key_usage) failed (0x"
                           << std::hex << s << ")";
      }

      s = NCryptFinalizeKey(out_key, 0);
      if (s != ERROR_SUCCESS) {
        BOOST_LOG(error) << "tpm_seal: NCryptFinalizeKey failed (0x"
                         << std::hex << s << ")";
        NCryptDeleteKey(out_key, 0);
        return false;
      }
      BOOST_LOG(info) << "tpm_seal: created TPM-bound RSA-2048 key '"
                      << "LuminalShineAdminCredentialsKey'";
      return true;
    }

    bool gen_random(BYTE *buf, DWORD len) {
      const NTSTATUS s = BCryptGenRandom(nullptr, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      return s == 0;
    }

    /// AES-256-GCM encrypt with a freshly-generated 12-byte IV. Writes
    /// the ciphertext to @p out_ct and the tag to @p out_tag.
    bool aes_gcm_encrypt(
      const BYTE *key, DWORD key_len,
      const BYTE *iv, DWORD iv_len,
      const BYTE *plaintext, DWORD pt_len,
      std::vector<BYTE> &out_ct,
      BYTE *out_tag, DWORD tag_len
    ) {
      BCRYPT_ALG_HANDLE alg = nullptr;
      NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
      if (s != 0) {
        return false;
      }
      s = BCryptSetProperty(
        alg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PBYTE>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0
      );
      if (s != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
      }
      BCRYPT_KEY_HANDLE sym = nullptr;
      s = BCryptGenerateSymmetricKey(
        alg, &sym, nullptr, 0,
        const_cast<BYTE *>(key), key_len, 0
      );
      BCryptCloseAlgorithmProvider(alg, 0);
      if (s != 0) {
        return false;
      }

      BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth = {};
      BCRYPT_INIT_AUTH_MODE_INFO(auth);
      auth.pbNonce = const_cast<PUCHAR>(iv);
      auth.cbNonce = iv_len;
      auth.pbTag = out_tag;
      auth.cbTag = tag_len;

      DWORD ct_size = 0;
      s = BCryptEncrypt(
        sym, const_cast<PUCHAR>(plaintext), pt_len, &auth,
        nullptr, 0, nullptr, 0, &ct_size, 0
      );
      if (s != 0) {
        BCryptDestroyKey(sym);
        return false;
      }
      out_ct.assign(ct_size, 0);
      s = BCryptEncrypt(
        sym, const_cast<PUCHAR>(plaintext), pt_len, &auth,
        nullptr, 0, out_ct.data(), ct_size, &ct_size, 0
      );
      BCryptDestroyKey(sym);
      if (s != 0) {
        out_ct.clear();
        return false;
      }
      out_ct.resize(ct_size);
      return true;
    }

    /// AES-256-GCM decrypt. Verifies the tag implicitly via the GCM
    /// authenticator and returns false on auth failure.
    bool aes_gcm_decrypt(
      const BYTE *key, DWORD key_len,
      const BYTE *iv, DWORD iv_len,
      const BYTE *tag, DWORD tag_len,
      const BYTE *ciphertext, DWORD ct_len,
      std::vector<BYTE> &out_pt
    ) {
      BCRYPT_ALG_HANDLE alg = nullptr;
      NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
      if (s != 0) {
        return false;
      }
      s = BCryptSetProperty(
        alg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PBYTE>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0
      );
      if (s != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
      }
      BCRYPT_KEY_HANDLE sym = nullptr;
      s = BCryptGenerateSymmetricKey(
        alg, &sym, nullptr, 0,
        const_cast<BYTE *>(key), key_len, 0
      );
      BCryptCloseAlgorithmProvider(alg, 0);
      if (s != 0) {
        return false;
      }

      BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth = {};
      BCRYPT_INIT_AUTH_MODE_INFO(auth);
      auth.pbNonce = const_cast<PUCHAR>(iv);
      auth.cbNonce = iv_len;
      auth.pbTag = const_cast<PUCHAR>(tag);
      auth.cbTag = tag_len;

      DWORD pt_size = 0;
      s = BCryptDecrypt(
        sym, const_cast<PUCHAR>(ciphertext), ct_len, &auth,
        nullptr, 0, nullptr, 0, &pt_size, 0
      );
      if (s != 0) {
        BCryptDestroyKey(sym);
        return false;
      }
      out_pt.assign(pt_size, 0);
      s = BCryptDecrypt(
        sym, const_cast<PUCHAR>(ciphertext), ct_len, &auth,
        nullptr, 0, out_pt.data(), pt_size, &pt_size, 0
      );
      BCryptDestroyKey(sym);
      if (s != 0) {
        // GCM tag mismatch lands here as STATUS_AUTH_TAG_MISMATCH.
        out_pt.clear();
        return false;
      }
      out_pt.resize(pt_size);
      return true;
    }

  }  // namespace

  bool available() {
    std::lock_guard<std::mutex> lk(probe_mutex());
    if (probe_done()) {
      return probe_result();
    }
    probe_done() = true;

    NCRYPT_PROV_HANDLE prov = 0;
    if (!open_provider(prov)) {
      BOOST_LOG(info) << "tpm_seal: Microsoft Platform Crypto Provider not available; "
                      << "TPM binding will be skipped even if tpm_binding=on.";
      probe_result() = false;
      return false;
    }
    NCryptFreeObject(prov);
    probe_result() = true;
    return true;
  }

  bool looks_sealed(std::string_view blob) {
    return blob.size() >= sizeof(kMagic) &&
           std::memcmp(blob.data(), kMagic, sizeof(kMagic)) == 0;
  }

  bool seal(std::string_view plaintext, std::string &sealed_out) {
    sealed_out.clear();
    if (!available()) {
      return false;
    }

    NCRYPT_PROV_HANDLE prov = 0;
    if (!open_provider(prov)) {
      return false;
    }
    NCRYPT_KEY_HANDLE key = 0;
    if (!open_or_create_key(prov, key)) {
      NCryptFreeObject(prov);
      return false;
    }

    std::array<BYTE, kAesKeyLen> aes_key {};
    std::array<BYTE, kIvLen> iv {};
    if (!gen_random(aes_key.data(), kAesKeyLen) ||
        !gen_random(iv.data(), kIvLen)) {
      NCryptFreeObject(key);
      NCryptFreeObject(prov);
      return false;
    }

    BCRYPT_OAEP_PADDING_INFO oaep = {BCRYPT_SHA256_ALGORITHM, nullptr, 0};
    DWORD wrapped_size = 0;
    SECURITY_STATUS s = NCryptEncrypt(
      key, aes_key.data(), kAesKeyLen, &oaep,
      nullptr, 0, &wrapped_size, NCRYPT_PAD_OAEP_FLAG
    );
    if (s != ERROR_SUCCESS || wrapped_size == 0 || wrapped_size > 0xFFFF) {
      BOOST_LOG(error) << "tpm_seal: NCryptEncrypt(probe) failed (0x"
                       << std::hex << s << ")";
      NCryptFreeObject(key);
      NCryptFreeObject(prov);
      return false;
    }
    std::vector<BYTE> wrapped(wrapped_size, 0);
    s = NCryptEncrypt(
      key, aes_key.data(), kAesKeyLen, &oaep,
      wrapped.data(), wrapped_size, &wrapped_size, NCRYPT_PAD_OAEP_FLAG
    );
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
    if (s != ERROR_SUCCESS) {
      BOOST_LOG(error) << "tpm_seal: NCryptEncrypt failed (0x"
                       << std::hex << s << ")";
      return false;
    }
    wrapped.resize(wrapped_size);

    std::array<BYTE, kTagLen> tag {};
    std::vector<BYTE> ciphertext;
    if (!aes_gcm_encrypt(
          aes_key.data(), kAesKeyLen,
          iv.data(), kIvLen,
          reinterpret_cast<const BYTE *>(plaintext.data()),
          static_cast<DWORD>(plaintext.size()),
          ciphertext, tag.data(), kTagLen
        )) {
      BOOST_LOG(error) << "tpm_seal: AES-GCM encrypt failed";
      return false;
    }

    sealed_out.reserve(
      sizeof(kMagic) + 2 + wrapped.size() + kIvLen + kTagLen + ciphertext.size()
    );
    sealed_out.append(kMagic, sizeof(kMagic));
    const auto wlen = static_cast<std::uint16_t>(wrapped.size());
    sealed_out.push_back(static_cast<char>((wlen >> 8) & 0xFF));
    sealed_out.push_back(static_cast<char>(wlen & 0xFF));
    sealed_out.append(reinterpret_cast<const char *>(wrapped.data()), wrapped.size());
    sealed_out.append(reinterpret_cast<const char *>(iv.data()), kIvLen);
    sealed_out.append(reinterpret_cast<const char *>(tag.data()), kTagLen);
    sealed_out.append(reinterpret_cast<const char *>(ciphertext.data()), ciphertext.size());

    // Wipe the AES key buffer; the TPM-wrapped copy is the only one
    // that persists.
    SecureZeroMemory(aes_key.data(), kAesKeyLen);
    return true;
  }

  bool unseal(std::string_view sealed, std::string &plaintext_out) {
    plaintext_out.clear();
    if (!looks_sealed(sealed)) {
      return false;
    }
    if (!available()) {
      return false;
    }
    if (sealed.size() < sizeof(kMagic) + 2 + kIvLen + kTagLen) {
      return false;
    }
    const BYTE *p = reinterpret_cast<const BYTE *>(sealed.data()) + sizeof(kMagic);
    const std::uint16_t wrapped_len = (static_cast<std::uint16_t>(p[0]) << 8) | p[1];
    p += 2;
    const size_t header_size = sizeof(kMagic) + 2 + wrapped_len + kIvLen + kTagLen;
    if (sealed.size() < header_size) {
      return false;
    }
    const BYTE *wrapped = p;
    p += wrapped_len;
    const BYTE *iv = p;
    p += kIvLen;
    const BYTE *tag = p;
    p += kTagLen;
    const BYTE *ciphertext = p;
    const DWORD ct_len = static_cast<DWORD>(sealed.size() - header_size);

    NCRYPT_PROV_HANDLE prov = 0;
    if (!open_provider(prov)) {
      return false;
    }
    NCRYPT_KEY_HANDLE key = 0;
    if (!open_or_create_key(prov, key)) {
      NCryptFreeObject(prov);
      return false;
    }

    BCRYPT_OAEP_PADDING_INFO oaep = {BCRYPT_SHA256_ALGORITHM, nullptr, 0};
    std::array<BYTE, kAesKeyLen> aes_key {};
    DWORD aes_size = 0;
    SECURITY_STATUS s = NCryptDecrypt(
      key, const_cast<BYTE *>(wrapped), wrapped_len, &oaep,
      aes_key.data(), kAesKeyLen, &aes_size, NCRYPT_PAD_OAEP_FLAG
    );
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
    if (s != ERROR_SUCCESS || aes_size != kAesKeyLen) {
      BOOST_LOG(warning) << "tpm_seal: NCryptDecrypt failed (0x"
                         << std::hex << s << "); credential blob is "
                         << "unrecoverable on this TPM.";
      SecureZeroMemory(aes_key.data(), kAesKeyLen);
      return false;
    }

    std::vector<BYTE> plaintext;
    const bool ok = aes_gcm_decrypt(
      aes_key.data(), kAesKeyLen,
      iv, kIvLen, tag, kTagLen,
      ciphertext, ct_len, plaintext
    );
    SecureZeroMemory(aes_key.data(), kAesKeyLen);
    if (!ok) {
      BOOST_LOG(warning) << "tpm_seal: AES-GCM decrypt / tag verify failed";
      return false;
    }

    plaintext_out.assign(reinterpret_cast<const char *>(plaintext.data()), plaintext.size());
    SecureZeroMemory(plaintext.data(), plaintext.size());
    return true;
  }

}  // namespace cred_store::tpm_seal
