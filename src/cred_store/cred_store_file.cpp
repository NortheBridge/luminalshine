/**
 * @file src/cred_store/cred_store_file.cpp
 * @brief File-backed implementation of the public `cred_store::` API.
 *        Selected by CMake when no system secret store is available
 *        (non-Windows / non-macOS hosts, or Linux hosts without
 *        libsecret installed). The actual file I/O lives in
 *        `cred_store::file_backend::` so the libsecret backend on Linux
 *        can fall back to the same code path when the secret service
 *        is unreachable.
 */
#include "src/cred_store/cred_store.h"

#include <fstream>
#include <string>

#include "src/config.h"
#include "src/cred_store/file_backend.h"

namespace cred_store {

  std::string backend_name() {
    return "file";
  }

  std::string default_key() {
    return config::sunshine.credentials_file;
  }

  bool exists(std::string_view key) {
    return file_backend::exists(key);
  }

  bool load(std::string_view key, std::string &out) {
    return file_backend::load(key, out);
  }

  bool store(std::string_view key, std::string_view blob) {
    return file_backend::store(key, blob);
  }

  bool erase(std::string_view key) {
    bool ok = file_backend::erase(key);
    // Discard any quarantined copy too — see cred_store.h.
    (void) file_backend::erase(std::string(key) + ".quarantine");
    return ok;
  }

  probe_result probe(std::string_view key) {
    if (!file_backend::exists(key)) {
      return probe_result::absent;
    }
    std::string blob;
    const bool loadable = file_backend::load(key, blob) && !blob.empty();
    return loadable ? probe_result::present_loadable : probe_result::present_unloadable;
  }

  bool quarantine(std::string_view key, std::string_view blob) {
    // Raw byte write on purpose: the quarantined blob is by definition
    // suspect (possibly not valid JSON), so file_backend::store's JSON
    // validation must not apply here.
    std::ofstream out(std::string(key) + ".quarantine", std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    return out.good();
  }

}  // namespace cred_store
