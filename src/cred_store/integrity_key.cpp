/**
 * @file src/cred_store/integrity_key.cpp
 * @brief See integrity_key.h.
 *
 * Single translation unit with #ifdef _WIN32 gates because the
 * authoritative storage primitive (TPM via cred_store::tpm_seal +
 * DPAPI-LocalMachine fallback) is Windows-only. Non-Windows builds
 * compile a no-op that returns std::nullopt so callers degrade to
 * "no integrity verification" rather than failing to link.
 */
#include "src/cred_store/integrity_key.h"

#include "src/logging.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
  #include <filesystem>

  // clang-format off
  #include <winsock2.h>
  #include <windows.h>
  #include <dpapi.h>
  #include <wincrypt.h>
  // clang-format on

  #include <openssl/rand.h>

  #include "src/cred_store/tpm_seal_windows.h"
  #include "src/platform/common.h"
#endif

namespace cred_store::integrity_key {

#ifdef _WIN32

  namespace {

    constexpr std::string_view kSealedFileName = "integrity.key.sealed";
    constexpr std::size_t kKeyBytes = 32;
    constexpr char kDpapiMagic[4] = {'D', 'P', 'M', 'K'};

    struct cached_state {
      bool initialized = false;
      bool available = false;
      std::vector<std::uint8_t> bytes;
      std::string backend = "unavailable";
    };

    cached_state &cache() {
      static cached_state c;
      return c;
    }

    std::mutex &cache_mutex() {
      static std::mutex m;
      return m;
    }

    std::filesystem::path key_path() {
      return platf::appdata() / std::string(kSealedFileName);
    }

    bool read_file_bytes(const std::filesystem::path &p, std::string &out) {
      std::ifstream f(p, std::ios::binary);
      if (!f) {
        return false;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      out = ss.str();
      return !out.empty();
    }

    bool write_file_atomic(const std::filesystem::path &p, std::string_view bytes) {
      std::filesystem::path tmp = p;
      tmp += ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
          return false;
        }
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) {
          return false;
        }
        f.flush();
      }
      std::error_code ec;
      std::filesystem::rename(tmp, p, ec);
      if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
      }
      return true;
    }

    bool dpapi_wrap(const std::vector<std::uint8_t> &plain, std::string &out) {
      DATA_BLOB in {};
      in.cbData = static_cast<DWORD>(plain.size());
      in.pbData = const_cast<BYTE *>(plain.data());
      DATA_BLOB result {};
      if (!CryptProtectData(
            &in,
            L"LuminalShine integrity key",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_LOCAL_MACHINE,
            &result)) {
        return false;
      }
      out.assign(kDpapiMagic, sizeof(kDpapiMagic));
      out.append(reinterpret_cast<const char *>(result.pbData), result.cbData);
      LocalFree(result.pbData);
      return true;
    }

    bool dpapi_unwrap(std::string_view sealed, std::vector<std::uint8_t> &out) {
      if (sealed.size() <= sizeof(kDpapiMagic) ||
          std::memcmp(sealed.data(), kDpapiMagic, sizeof(kDpapiMagic)) != 0) {
        return false;
      }
      const auto *body = reinterpret_cast<const BYTE *>(sealed.data() + sizeof(kDpapiMagic));
      DWORD body_size = static_cast<DWORD>(sealed.size() - sizeof(kDpapiMagic));
      DATA_BLOB in {body_size, const_cast<BYTE *>(body)};
      DATA_BLOB result {};
      if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &result)) {
        return false;
      }
      out.assign(result.pbData, result.pbData + result.cbData);
      LocalFree(result.pbData);
      return true;
    }

    bool initialize_locked() {
      auto &c = cache();
      if (c.initialized) {
        return c.available;
      }
      c.initialized = true;

      const auto path = key_path();
      std::string sealed_blob;
      if (read_file_bytes(path, sealed_blob)) {
        std::string tpm_unsealed;
        if (tpm_seal::looks_sealed(sealed_blob) && tpm_seal::unseal(sealed_blob, tpm_unsealed)) {
          if (tpm_unsealed.size() == kKeyBytes) {
            c.bytes.assign(tpm_unsealed.begin(), tpm_unsealed.end());
            c.backend = "tpm";
            c.available = true;
            return true;
          }
        }
        std::vector<std::uint8_t> dpapi_unsealed;
        if (dpapi_unwrap(sealed_blob, dpapi_unsealed) && dpapi_unsealed.size() == kKeyBytes) {
          c.bytes = std::move(dpapi_unsealed);
          c.backend = "dpapi-lm";
          c.available = true;
          return true;
        }
        BOOST_LOG(warning) << "integrity_key: sealed key at "
                           << path.string()
                           << " is unreadable on this host; regenerating.";
      }

      std::vector<std::uint8_t> fresh(kKeyBytes);
      if (RAND_bytes(fresh.data(), static_cast<int>(fresh.size())) != 1) {
        BOOST_LOG(error) << "integrity_key: RAND_bytes failed; tamper detection disabled.";
        return false;
      }

      std::string sealed_out;
      bool used_tpm = false;
      if (tpm_seal::available()) {
        std::string plain(reinterpret_cast<const char *>(fresh.data()), fresh.size());
        if (tpm_seal::seal(plain, sealed_out)) {
          used_tpm = true;
        }
      }
      if (!used_tpm && !dpapi_wrap(fresh, sealed_out)) {
        BOOST_LOG(error) << "integrity_key: neither TPM nor DPAPI sealing succeeded; "
                            "tamper detection disabled.";
        return false;
      }

      if (!write_file_atomic(path, sealed_out)) {
        BOOST_LOG(error) << "integrity_key: could not persist sealed key to "
                         << path.string() << "; tamper detection disabled.";
        return false;
      }

      c.bytes = std::move(fresh);
      c.backend = used_tpm ? "tpm" : "dpapi-lm";
      c.available = true;
      BOOST_LOG(info) << "integrity_key: generated new key (backend=" << c.backend << ").";
      return true;
    }

  }  // namespace

  std::string backend_name() {
    std::lock_guard<std::mutex> lk(cache_mutex());
    initialize_locked();
    return cache().backend;
  }

  std::optional<std::vector<std::uint8_t>> get() {
    std::lock_guard<std::mutex> lk(cache_mutex());
    if (!initialize_locked()) {
      return std::nullopt;
    }
    return cache().bytes;
  }

  void prime() {
    std::lock_guard<std::mutex> lk(cache_mutex());
    initialize_locked();
    BOOST_LOG(info) << "integrity_backend=" << cache().backend;
  }

#else  // !_WIN32

  std::string backend_name() {
    return "unavailable";
  }

  std::optional<std::vector<std::uint8_t>> get() {
    return std::nullopt;
  }

  void prime() {
    BOOST_LOG(info) << "integrity_backend=unavailable (non-Windows build)";
  }

#endif

}  // namespace cred_store::integrity_key
