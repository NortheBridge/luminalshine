/**
 * @file src/cred_store/cred_store_macos.cpp
 * @brief macOS Keychain backend for the cred_store layer. Replaces
 *        cred_store_file.cpp on Apple builds via the CMake conditional
 *        in common.cmake.
 *
 * Credentials are stored as a `kSecClassGenericPassword` item in the
 * default keychain (Login keychain when LuminalShine runs from a user
 * session; System keychain when packaged as a launchd daemon — see the
 * launchd plist in `cmake/packaging/macos.cmake`). Service name is
 * `io.northebridge.LuminalShine` and account is `AdminCredentials`.
 *
 * On Apple Silicon and modern Intel Macs the Keychain database is
 * sealed with a key derived from the user's account password and stored
 * in the Secure Enclave when a Touch ID / T2 chip is present. The
 * threat-model effect is similar to the Windows backend on Windows 11:
 * a drive removed from the host and mounted elsewhere cannot decrypt
 * the credential blob.
 *
 * Like the Windows backend, the first invocation also runs a one-shot
 * import: if no Keychain item exists at the service/account pair but a
 * plaintext file is present at the configured path, the file's contents
 * are imported into Keychain, the file is renamed with a
 * `.migrated-<UTC>` suffix, and `cred_store::exists/load` start
 * returning the Keychain-backed value.
 */
#include "src/cred_store/cred_store.h"

#include "src/config.h"
#include "src/logging.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <fstream>
#include <string>

namespace cred_store {

  namespace {
    constexpr const char *kBackendName = "macos-keychain";
    constexpr const char *kServiceName = "io.northebridge.LuminalShine";
    constexpr const char *kAccountName = "AdminCredentials";

    std::mutex &import_mutex() {
      static std::mutex m;
      return m;
    }

    bool &import_attempted_flag() {
      static bool attempted = false;
      return attempted;
    }

    /// Helper: build the {service, account} query dictionary common to
    /// all SecItem* calls in this backend. Caller takes ownership of
    /// the returned CFMutableDictionaryRef.
    CFMutableDictionaryRef make_base_query() {
      CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
      );
      CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
      CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault, kServiceName, kCFStringEncodingUTF8);
      CFStringRef account = CFStringCreateWithCString(kCFAllocatorDefault, kAccountName, kCFStringEncodingUTF8);
      CFDictionarySetValue(query, kSecAttrService, service);
      CFDictionarySetValue(query, kSecAttrAccount, account);
      CFRelease(service);
      CFRelease(account);
      return query;
    }

    std::string utf8_timestamp_now() {
      using namespace std::chrono;
      const auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
      std::time_t tt = static_cast<std::time_t>(secs);
      std::tm utc {};
      gmtime_r(&tt, &utc);
      char buf[24] = {};
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
      return buf;
    }

    /// Direct (no-migration) writer used by both the public `store`
    /// path and the one-shot import path so they don't duplicate the
    /// SecItemAdd / SecItemUpdate dance.
    bool keychain_write(std::string_view blob) {
      CFMutableDictionaryRef add_query = make_base_query();
      CFDataRef payload = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(blob.data()),
        static_cast<CFIndex>(blob.size())
      );
      CFDictionarySetValue(add_query, kSecValueData, payload);

      OSStatus rc = SecItemAdd(add_query, nullptr);
      CFRelease(add_query);

      if (rc == errSecDuplicateItem) {
        CFMutableDictionaryRef match = make_base_query();
        CFMutableDictionaryRef update = CFDictionaryCreateMutable(
          kCFAllocatorDefault, 0,
          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
        );
        CFDictionarySetValue(update, kSecValueData, payload);
        rc = SecItemUpdate(match, update);
        CFRelease(match);
        CFRelease(update);
      }
      CFRelease(payload);

      if (rc != errSecSuccess) {
        BOOST_LOG(error) << "cred_store(keychain): SecItemAdd/Update failed (status="
                         << rc << ")";
        return false;
      }
      return true;
    }

    /// One-shot import: if no Keychain entry exists at our service/
    /// account pair AND a legacy plaintext file exists at @p key, copy
    /// its contents into Keychain and rename the file with a
    /// `.migrated-<UTC>` suffix. Idempotent.
    void maybe_import_file_once(std::string_view key) {
      std::lock_guard<std::mutex> lk(import_mutex());
      if (import_attempted_flag()) {
        return;
      }
      import_attempted_flag() = true;

      CFMutableDictionaryRef probe = make_base_query();
      OSStatus probe_rc = SecItemCopyMatching(probe, nullptr);
      CFRelease(probe);
      if (probe_rc == errSecSuccess) {
        return;  // Entry already exists.
      }
      if (probe_rc != errSecItemNotFound) {
        BOOST_LOG(warning) << "cred_store(keychain): import probe returned unexpected "
                           << "status " << probe_rc;
      }

      if (key.empty()) {
        return;
      }
      namespace fs = std::filesystem;
      fs::path legacy_path(std::string {key});
      std::error_code ec;
      if (!fs::exists(legacy_path, ec)) {
        return;
      }

      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_json(legacy_path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "cred_store(keychain): legacy credentials file at "
                           << legacy_path.string() << " is unparseable ("
                           << e.what() << "); skipping import.";
        return;
      }

      std::ostringstream blob;
      try {
        boost::property_tree::write_json(blob, tree);
      } catch (...) {
        return;
      }

      if (!keychain_write(blob.str())) {
        return;
      }

      fs::path archive_path = legacy_path;
      archive_path += ".migrated-" + utf8_timestamp_now();
      std::error_code rename_ec;
      fs::rename(legacy_path, archive_path, rename_ec);
      if (rename_ec) {
        BOOST_LOG(warning) << "cred_store(keychain): imported credentials but could "
                           << "not rename source file (" << legacy_path.string()
                           << " -> " << archive_path.string() << "): "
                           << rename_ec.message();
      } else {
        BOOST_LOG(info) << "cred_store(keychain): migrated legacy credentials from "
                        << legacy_path.string() << " into the macOS Keychain "
                        << "(archived as " << archive_path.string() << ").";
      }
    }
  }  // namespace

  std::string backend_name() {
    return kBackendName;
  }

  std::string default_key() {
    return config::sunshine.credentials_file;
  }

  bool exists(std::string_view key) {
    maybe_import_file_once(key);

    CFMutableDictionaryRef query = make_base_query();
    OSStatus rc = SecItemCopyMatching(query, nullptr);
    CFRelease(query);
    return rc == errSecSuccess;
  }

  bool load(std::string_view key, std::string &out) {
    out.clear();
    maybe_import_file_once(key);

    CFMutableDictionaryRef query = make_base_query();
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    OSStatus rc = SecItemCopyMatching(query, &result);
    CFRelease(query);

    if (rc != errSecSuccess) {
      if (rc != errSecItemNotFound) {
        BOOST_LOG(warning) << "cred_store(keychain): SecItemCopyMatching failed (status="
                           << rc << ")";
      }
      return false;
    }
    if (!result) {
      return false;
    }
    CFDataRef data = static_cast<CFDataRef>(result);
    const CFIndex length = CFDataGetLength(data);
    if (length > 0) {
      out.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                 static_cast<size_t>(length));
    }
    CFRelease(data);
    return !out.empty();
  }

  bool store(std::string_view key, std::string_view blob) {
    maybe_import_file_once(key);
    return keychain_write(blob);
  }

  bool erase(std::string_view key) {
    (void) key;
    CFMutableDictionaryRef query = make_base_query();
    OSStatus rc = SecItemDelete(query);
    CFRelease(query);
    if (rc == errSecSuccess || rc == errSecItemNotFound) {
      if (rc == errSecSuccess) {
        BOOST_LOG(info) << "cred_store(keychain): credential entry removed.";
      }
      return true;
    }
    BOOST_LOG(warning) << "cred_store(keychain): SecItemDelete failed (status=" << rc << ")";
    return false;
  }

  probe_result probe(std::string_view key) {
    if (!exists(key)) {
      return probe_result::absent;
    }
    std::string blob;
    const bool loadable = load(key, blob) && !blob.empty();
    return loadable ? probe_result::present_loadable : probe_result::present_unloadable;
  }

  bool quarantine(std::string_view key, std::string_view blob) {
    // Legacy platform: preserve the suspect bytes next to the configured
    // credentials file. Raw write on purpose — the blob may not be JSON.
    std::ofstream out(std::string(key) + ".quarantine", std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    return out.good();
  }

}  // namespace cred_store
