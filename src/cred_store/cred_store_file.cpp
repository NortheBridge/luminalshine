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
    return file_backend::erase(key);
  }

}  // namespace cred_store
