/**
 * @file src/cred_store/cred_store_file.cpp
 * @brief File-backed credential store. This is the default fallback;
 *        per-platform overrides ship as separate translation units
 *        registered ahead of this one in CMake on platforms with a
 *        system secret store available.
 *
 * Behaviour matches the pre-PR-2 file path exactly: writes go through
 * `statefile::atomic_write_json` (temp + fsync + rename + .bak
 * rotation) and reads through `statefile::load_or_recover` (primary,
 * then .bak). Keys are treated as filesystem paths.
 */
#include "src/cred_store/cred_store.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/state_storage.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <sstream>

namespace cred_store {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  // PR 2 ships this file as the only backend. PR 3 (Windows
  // Credential Manager) will swap this TU out for cred_store_windows.cpp
  // via CMake when _WIN32 is set; PR 4 does the same for Linux /
  // macOS. The signatures are identical so the dispatch is a build-
  // time pick, not a runtime indirection.

  std::string backend_name() {
    return "file";
  }

  std::string default_key() {
    return config::sunshine.credentials_file;
  }

  bool exists(std::string_view key) {
    if (key.empty()) {
      return false;
    }
    fs::path path(std::string {key});
    std::error_code ec;
    if (fs::exists(path, ec)) {
      return true;
    }
    // The atomic-write recovery path also checks .bak, so a primary-
    // missing-but-bak-present state still counts as "exists" from the
    // caller's perspective. load_or_recover will surface it.
    fs::path bak = path;
    bak += ".bak";
    return fs::exists(bak, ec);
  }

  bool load(std::string_view key, std::string &out) {
    out.clear();
    if (key.empty()) {
      return false;
    }
    pt::ptree tree;
    if (!statefile::load_or_recover(fs::path {std::string {key}}, tree)) {
      return false;
    }
    std::ostringstream serialized;
    try {
      pt::write_json(serialized, tree);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "cred_store(file): failed to serialise credentials at "
                       << key << ": " << e.what();
      return false;
    }
    out = serialized.str();
    return true;
  }

  bool store(std::string_view key, std::string_view blob) {
    if (key.empty()) {
      return false;
    }
    pt::ptree tree;
    try {
      std::istringstream in {std::string {blob}};
      pt::read_json(in, tree);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "cred_store(file): refusing to write malformed JSON blob to "
                       << key << ": " << e.what();
      return false;
    }
    return statefile::atomic_write_json(fs::path {std::string {key}}, tree);
  }

  bool erase(std::string_view key) {
    if (key.empty()) {
      return true;
    }
    std::error_code ec;
    fs::path path(std::string {key});
    bool ok = true;
    fs::remove(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "cred_store(file): failed to remove primary at "
                         << key << ": " << ec.message();
      ok = false;
    }
    fs::path bak = path;
    bak += ".bak";
    std::error_code bak_ec;
    fs::remove(bak, bak_ec);
    if (bak_ec && bak_ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "cred_store(file): failed to remove backup at "
                         << bak.string() << ": " << bak_ec.message();
      ok = false;
    }
    return ok;
  }

}  // namespace cred_store
