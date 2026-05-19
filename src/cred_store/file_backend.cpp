/**
 * @file src/cred_store/file_backend.cpp
 * @brief Implementation of the internal `cred_store::file_backend`
 *        helpers. The public `cred_store::` API in cred_store_file.cpp
 *        and the libsecret-fallback paths in cred_store_linux.cpp both
 *        delegate here so the on-disk format stays in one place.
 */
#include "src/cred_store/file_backend.h"

#include "src/logging.h"
#include "src/state_storage.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <sstream>

namespace cred_store::file_backend {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  bool exists(std::string_view path) {
    if (path.empty()) {
      return false;
    }
    fs::path p(std::string {path});
    std::error_code ec;
    if (fs::exists(p, ec)) {
      return true;
    }
    fs::path bak = p;
    bak += ".bak";
    return fs::exists(bak, ec);
  }

  bool load(std::string_view path, std::string &out) {
    out.clear();
    if (path.empty()) {
      return false;
    }
    pt::ptree tree;
    if (!statefile::load_or_recover(fs::path {std::string {path}}, tree)) {
      return false;
    }
    std::ostringstream serialized;
    try {
      pt::write_json(serialized, tree);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "cred_store(file): failed to serialise credentials at "
                       << path << ": " << e.what();
      return false;
    }
    out = serialized.str();
    return true;
  }

  bool store(std::string_view path, std::string_view blob) {
    if (path.empty()) {
      return false;
    }
    pt::ptree tree;
    try {
      std::istringstream in {std::string {blob}};
      pt::read_json(in, tree);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "cred_store(file): refusing to write malformed JSON blob to "
                       << path << ": " << e.what();
      return false;
    }
    return statefile::atomic_write_json(fs::path {std::string {path}}, tree);
  }

  bool erase(std::string_view path) {
    if (path.empty()) {
      return true;
    }
    std::error_code ec;
    fs::path p(std::string {path});
    bool ok = true;
    fs::remove(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "cred_store(file): failed to remove primary at "
                         << path << ": " << ec.message();
      ok = false;
    }
    fs::path bak = p;
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

}  // namespace cred_store::file_backend
