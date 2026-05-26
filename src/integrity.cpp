/**
 * @file src/integrity.cpp
 * @brief See integrity.h.
 */
#include "src/integrity.h"

#include "src/cred_store/integrity_key.h"
#include "src/logging.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

namespace integrity {

  namespace {

    namespace fs = std::filesystem;

    fs::path sidecar_path(const fs::path &p) {
      fs::path q = p;
      q += ".sig";
      return q;
    }

    std::string utc_stamp() {
      using namespace std::chrono;
      const auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
      std::time_t tt = static_cast<std::time_t>(secs);
      std::tm utc {};
#ifdef _WIN32
      gmtime_s(&utc, &tt);
#else
      gmtime_r(&tt, &utc);
#endif
      char buf[24] {};
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
      return buf;
    }

    bool compute_hmac(const std::vector<std::uint8_t> &key,
                      std::string_view data,
                      std::array<std::uint8_t, 32> &out) {
      unsigned int out_len = 0;
      const auto *res = HMAC(
        EVP_sha256(),
        key.data(),
        static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char *>(data.data()),
        data.size(),
        out.data(),
        &out_len);
      return res != nullptr && out_len == out.size();
    }

    std::string hex_encode(const std::array<std::uint8_t, 32> &bytes) {
      std::ostringstream ss;
      ss << std::hex << std::setfill('0');
      for (auto b : bytes) {
        ss << std::setw(2) << static_cast<unsigned>(b);
      }
      return ss.str();
    }

    bool hex_decode(std::string_view hex, std::array<std::uint8_t, 32> &out) {
      if (hex.size() != out.size() * 2) {
        return false;
      }
      auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
          return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
          return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
          return c - 'A' + 10;
        }
        return -1;
      };
      for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
          return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
      }
      return true;
    }

    bool atomic_write_bytes(const fs::path &path, std::string_view bytes) {
      fs::path tmp = path;
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
      fs::rename(tmp, path, ec);
      if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return false;
      }
      return true;
    }

    bool read_file(const fs::path &p, std::string &out) {
      std::ifstream f(p, std::ios::binary);
      if (!f) {
        return false;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      out = ss.str();
      return true;
    }

  }  // namespace

  std::string backend_name() {
    return cred_store::integrity_key::backend_name();
  }

  bool write_signed(const fs::path &path, std::string_view contents) {
    auto key = cred_store::integrity_key::get();
    if (!key) {
      // No integrity backend; preserve pre-PR behaviour by writing the file
      // plain. The caller's quarantine handler treats `not_protected` as
      // graceful-degradation, so no follow-up action is required.
      return atomic_write_bytes(path, contents);
    }
    std::array<std::uint8_t, 32> mac {};
    if (!compute_hmac(*key, contents, mac)) {
      BOOST_LOG(error) << "integrity: HMAC compute failed for " << path.string();
      return false;
    }
    if (!atomic_write_bytes(path, contents)) {
      return false;
    }
    if (!atomic_write_bytes(sidecar_path(path), hex_encode(mac))) {
      BOOST_LOG(warning) << "integrity: wrote " << path.string()
                         << " but sidecar write failed; file is now unprotected.";
      return false;
    }
    return true;
  }

  verify_status verify(const fs::path &path) {
    auto key = cred_store::integrity_key::get();
    if (!key) {
      return verify_status::no_key;
    }

    std::string sidecar_contents;
    if (!read_file(sidecar_path(path), sidecar_contents)) {
      return verify_status::not_protected;
    }
    while (!sidecar_contents.empty() &&
           (sidecar_contents.back() == '\n' || sidecar_contents.back() == '\r' ||
            sidecar_contents.back() == ' ' || sidecar_contents.back() == '\t')) {
      sidecar_contents.pop_back();
    }
    std::array<std::uint8_t, 32> expected {};
    if (!hex_decode(sidecar_contents, expected)) {
      return verify_status::unreadable;
    }

    std::string body;
    if (!read_file(path, body)) {
      return verify_status::unreadable;
    }
    std::array<std::uint8_t, 32> actual {};
    if (!compute_hmac(*key, body, actual)) {
      return verify_status::unreadable;
    }

    if (CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) != 0) {
      return verify_status::mismatch;
    }
    return verify_status::ok;
  }

  fs::path quarantine(const fs::path &path) {
    const auto stamp = utc_stamp();
    fs::path archive = path;
    archive += ".tamper-" + stamp;
    std::error_code ec;
    fs::rename(path, archive, ec);
    if (ec) {
      BOOST_LOG(error) << "integrity: quarantine rename "
                       << path.string() << " -> " << archive.string()
                       << " failed: " << ec.message();
      return {};
    }
    fs::path sidecar = sidecar_path(path);
    if (fs::exists(sidecar, ec)) {
      fs::path sidecar_archive = sidecar;
      sidecar_archive += ".tamper-" + stamp;
      std::error_code rn_ec;
      fs::rename(sidecar, sidecar_archive, rn_ec);
      if (rn_ec) {
        BOOST_LOG(warning) << "integrity: quarantine of sidecar "
                           << sidecar.string() << " failed: " << rn_ec.message();
      }
    }
    BOOST_LOG(warning) << "integrity: quarantined " << path.string()
                       << " -> " << archive.string()
                       << " (tamper or corruption detected).";
    return archive;
  }

}  // namespace integrity
