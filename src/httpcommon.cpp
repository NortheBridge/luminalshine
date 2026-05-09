/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <curl/curl.h>
#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"

#ifdef _WIN32
  #include <wincrypt.h>
  #include <Windows.h>
#endif

namespace {
  std::once_flag curl_global_once;

  void ensure_curl_global_init() {
    std::call_once(curl_global_once, []() {
      curl_global_init(CURL_GLOBAL_DEFAULT);
      std::atexit(curl_global_cleanup);
    });
  }

#ifdef _WIN32
  std::once_flag windows_ca_once;
  std::string windows_ca_bundle;
  bool windows_ca_loaded = false;
  std::size_t windows_ca_count = 0;
  std::size_t windows_ca_skipped_non_ca = 0;
  std::size_t windows_ca_skipped_dup = 0;

  // curl's schannel backend hard-rejects CAINFO files larger than 1 MiB
  // (CURL_MAX_INPUT_LENGTH for the schannel cert importer). Cap our bundle
  // before passing it to libcurl so update checks don't fail with
  // "schannel: CA file exceeds max size of 1048576 bytes".
  constexpr std::size_t kSchannelCaInfoMaxBytes = 1048576;

  // Treat a certificate as a usable CA only when both BasicConstraints
  // marks it as a CA AND key usage permits keyCertSign. The Windows ROOT
  // store on a typical desktop is full of intermediates, end-entity certs
  // pulled from CurrentUser\My, and other non-CA material that schannel
  // refuses to use as a trust anchor anyway. Filtering early shrinks the
  // bundle from ~1260 entries to the actual ~200 trust anchors.
  bool is_usable_ca_cert(PCCERT_CONTEXT ctx) {
    if (!ctx || !ctx->pCertInfo) {
      return false;
    }
    bool has_basic_ca = false;
    if (auto *bc_ext = CertFindExtension(
          szOID_BASIC_CONSTRAINTS2,
          ctx->pCertInfo->cExtension,
          ctx->pCertInfo->rgExtension)) {
      CERT_BASIC_CONSTRAINTS2_INFO info {};
      DWORD info_size = sizeof(info);
      if (CryptDecodeObjectEx(
            X509_ASN_ENCODING,
            X509_BASIC_CONSTRAINTS2,
            bc_ext->Value.pbData,
            bc_ext->Value.cbData,
            CRYPT_DECODE_NOCOPY_FLAG,
            nullptr,
            &info,
            &info_size)) {
        has_basic_ca = info.fCA != FALSE;
      }
    } else {
      // No BasicConstraints extension — root certs predating RFC 3280 are
      // legitimately CAs. Allow self-signed certs (issuer == subject) through
      // even without the extension; schannel will accept them as trust anchors.
      const auto &issuer = ctx->pCertInfo->Issuer;
      const auto &subject = ctx->pCertInfo->Subject;
      if (issuer.cbData == subject.cbData
          && issuer.cbData > 0
          && std::memcmp(issuer.pbData, subject.pbData, issuer.cbData) == 0) {
        has_basic_ca = true;
      }
    }
    if (!has_basic_ca) {
      return false;
    }

    // Key usage check: the cert must permit keyCertSign. If the extension is
    // absent, default-allow (RFC 5280 §4.2.1.3 makes KU advisory).
    if (auto *ku_ext = CertFindExtension(
          szOID_KEY_USAGE,
          ctx->pCertInfo->cExtension,
          ctx->pCertInfo->rgExtension)) {
      CRYPT_BIT_BLOB ku_blob {};
      DWORD blob_size = sizeof(ku_blob);
      BYTE ku_bytes[2] = {0, 0};
      ku_blob.pbData = ku_bytes;
      ku_blob.cbData = sizeof(ku_bytes);
      if (CryptDecodeObjectEx(
            X509_ASN_ENCODING,
            X509_KEY_USAGE,
            ku_ext->Value.pbData,
            ku_ext->Value.cbData,
            0,
            nullptr,
            &ku_blob,
            &blob_size)
          && ku_blob.cbData >= 1) {
        if ((ku_blob.pbData[0] & CERT_KEY_CERT_SIGN_KEY_USAGE) == 0) {
          return false;
        }
      }
    }
    return true;
  }

  void append_pem_chunk(const char *data, DWORD length) {
    if (!data || length == 0) {
      return;
    }
    std::string chunk(data, data + length);
    if (!chunk.empty() && chunk.back() == '\0') {
      chunk.pop_back();
    }
    if (chunk.empty()) {
      return;
    }
    windows_ca_bundle.append(chunk);
    if (!windows_ca_bundle.empty() && windows_ca_bundle.back() != '\n') {
      windows_ca_bundle.push_back('\n');
    }
  }

  bool populate_from_store(DWORD flags, std::unordered_set<std::string> &seen_fingerprints) {
    HCERTSTORE store = CertOpenStore(
      CERT_STORE_PROV_SYSTEM_W,
      0,
      static_cast<HCRYPTPROV_LEGACY>(0),
      flags | CERT_STORE_READONLY_FLAG,
      L"ROOT"
    );
    if (!store) {
      BOOST_LOG(error) << "CertOpenStore failed for flags " << flags << " error " << GetLastError();
      return false;
    }

    std::size_t added = 0;
    std::size_t skipped_non_ca = 0;
    std::size_t skipped_dup = 0;
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
      // Filter to actual trust anchors before doing any work.
      if (!is_usable_ca_cert(ctx)) {
        ++skipped_non_ca;
        continue;
      }

      // Deduplicate by SHA-256 fingerprint — both stores commonly contain the
      // same Microsoft / public-CA roots, and the LM/CU duplication alone was
      // the primary cause of the >1 MiB bundle.
      BYTE fp[32] = {};
      DWORD fp_len = sizeof(fp);
      if (!CertGetCertificateContextProperty(ctx, CERT_HASH_PROP_ID, fp, &fp_len) || fp_len == 0) {
        // Fall back to using the encoded body as the dedup key; this is rare.
        fp_len = (ctx->cbCertEncoded < sizeof(fp)) ? ctx->cbCertEncoded : sizeof(fp);
        std::memcpy(fp, ctx->pbCertEncoded, fp_len);
      }
      std::string fp_key(reinterpret_cast<const char *>(fp), fp_len);
      if (!seen_fingerprints.insert(std::move(fp_key)).second) {
        ++skipped_dup;
        continue;
      }

      DWORD out_len = 0;
      if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &out_len)) {
        continue;
      }
      std::string buffer(out_len, '\0');
      if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER, buffer.data(), &out_len)) {
        continue;
      }
      append_pem_chunk(buffer.data(), out_len);
      ++added;
    }
    CertCloseStore(store, 0);
    windows_ca_count += added;
    windows_ca_skipped_non_ca += skipped_non_ca;
    windows_ca_skipped_dup += skipped_dup;
    if (added > 0 || skipped_non_ca > 0 || skipped_dup > 0) {
      BOOST_LOG(debug) << "Loaded " << added << " trust anchors from Windows store flags " << flags
                       << " (skipped " << skipped_non_ca << " non-CA, " << skipped_dup << " duplicate)";
    }
    return added > 0;
  }

  void load_windows_root_store() {
    windows_ca_bundle.clear();
    windows_ca_count = 0;
    windows_ca_skipped_non_ca = 0;
    windows_ca_skipped_dup = 0;
    std::unordered_set<std::string> seen_fingerprints;
    seen_fingerprints.reserve(512);
    bool loaded_machine = populate_from_store(CERT_SYSTEM_STORE_LOCAL_MACHINE, seen_fingerprints);
    bool loaded_user = populate_from_store(CERT_SYSTEM_STORE_CURRENT_USER, seen_fingerprints);
    windows_ca_loaded = loaded_machine || loaded_user;

    // Final size guard: if even after dedup + CA-only filter the bundle
    // still exceeds schannel's hard limit, drop the bundle entirely so the
    // caller can fall back to CURLSSLOPT_NATIVE_CA. Better to ask schannel
    // to walk the OS store directly than to ship a file curl will reject.
    if (windows_ca_loaded && windows_ca_bundle.size() > kSchannelCaInfoMaxBytes) {
      BOOST_LOG(warning) << "Windows CA bundle still exceeds schannel limit after filtering ("
                         << windows_ca_bundle.size() << " bytes, " << windows_ca_count
                         << " trust anchors); discarding bundle and relying on schannel native trust store.";
      windows_ca_bundle.clear();
      windows_ca_loaded = false;
    }

    if (windows_ca_loaded) {
      BOOST_LOG(info) << "Loaded " << windows_ca_count << " Windows trust anchors (machine="
                      << (loaded_machine ? "yes" : "no") << ", user=" << (loaded_user ? "yes" : "no")
                      << ", bundle=" << windows_ca_bundle.size() << "B, skipped="
                      << windows_ca_skipped_non_ca << " non-CA / "
                      << windows_ca_skipped_dup << " dup)";
    } else if (!loaded_machine && !loaded_user) {
      BOOST_LOG(error) << "Unable to load certificates from any Windows ROOT store. Last error " << GetLastError();
    }
  }

  std::optional<std::string> windows_ca_file_path() {
    static std::once_flag write_once;
    static std::optional<std::string> path;
    std::call_once(write_once, []() {
      if (!windows_ca_loaded || windows_ca_bundle.empty()) {
        return;
      }
      if (windows_ca_bundle.size() > kSchannelCaInfoMaxBytes) {
        // Should already have been guarded in load_windows_root_store, but
        // keep this defensive check to avoid ever emitting a too-large file.
        BOOST_LOG(warning) << "Skipping CA bundle file write: size "
                           << windows_ca_bundle.size() << "B exceeds schannel limit";
        return;
      }
      try {
        auto temp = std::filesystem::temp_directory_path() / "sunshine-ca-bundle.pem";
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(windows_ca_bundle.data(), static_cast<std::streamsize>(windows_ca_bundle.size()));
        if (out && out.good()) {
          path = temp.string();
          BOOST_LOG(debug) << "Persisted Windows CA bundle to " << *path
                           << " (" << windows_ca_bundle.size() << " bytes)";
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(error) << "Failed to persist Windows CA bundle: " << ex.what();
      }
    });
    return path;
  }
#endif

  bool apply_default_ca_store(CURL *curl) {
    if (!curl) {
      return false;
    }
#if defined(_WIN32)
    // Preferred path: tell curl to walk the schannel/native trust store
    // directly. This avoids materializing a PEM bundle entirely and is the
    // schannel-recommended approach on Windows. Try it before doing any of
    // the expensive cert enumeration / dedup work below.
  #if defined(CURLOPT_SSL_OPTIONS) && defined(CURLSSLOPT_NATIVE_CA)
    CURLcode native_res = curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    if (native_res == CURLE_OK) {
      return true;
    }
    BOOST_LOG(debug) << "CURLSSLOPT_NATIVE_CA setopt rejected (code " << native_res
                     << "); falling back to manually-built CA bundle.";
  #endif

    // Fallback path: materialize a deduplicated, CA-filtered, size-capped
    // PEM bundle from the Windows ROOT stores. Loaded lazily so a build
    // where NATIVE_CA always succeeds never pays this cost.
    std::call_once(windows_ca_once, load_windows_root_store);

    if (!windows_ca_loaded) {
      BOOST_LOG(warning) << "Windows root certificate bundle not available for HTTPS requests; "
                            "schannel will use its built-in defaults if any.";
      return false;
    }

  #if defined(CURLOPT_CAINFO_BLOB)
    curl_blob blob;
    blob.data = const_cast<char *>(windows_ca_bundle.data());
    blob.len = windows_ca_bundle.size();
    blob.flags = CURL_BLOB_COPY;
    CURLcode blob_res = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    if (blob_res == CURLE_OK) {
      return true;
    }
    BOOST_LOG(error) << "CURLOPT_CAINFO_BLOB failed, code " << blob_res;
  #endif

    if (auto file_path = windows_ca_file_path()) {
      CURLcode file_res = curl_easy_setopt(curl, CURLOPT_CAINFO, file_path->c_str());
      if (file_res == CURLE_OK) {
        return true;
      }
      BOOST_LOG(error) << "CURLOPT_CAINFO failed for " << *file_path << ", code " << file_res;
    }
    BOOST_LOG(error) << "Failed to supply CA bundle to libcurl for HTTPS";
    return false;
#else
    (void) curl;
    return true;
#endif
  }
}  // namespace

namespace http {
  using namespace std::literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  int reload_user_creds(const std::string &file);
  bool user_creds_exist(const std::string &file);

  std::string unique_id;
  net::net_e origin_web_ui_allowed;

#ifdef _WIN32
  std::string shared_virtual_display_guid;
#endif

  int init() {
    ensure_curl_global_init();
    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);

    if (clean_slate) {
      unique_id = uuid_util::uuid_t::generate().string();
      auto dir = std::filesystem::temp_directory_path() / "Sunshine"sv;
      config::nvhttp.cert = (dir / ("cert-"s + unique_id)).string();
      config::nvhttp.pkey = (dir / ("pkey-"s + unique_id)).string();
    }

    if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) &&
        create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
      return -1;
    }
    if (!user_creds_exist(config::sunshine.credentials_file)) {
      BOOST_LOG(info) << "Open the Web UI to set your new username and password and getting started";
    } else if (reload_user_creds(config::sunshine.credentials_file)) {
      return -1;
    }
    return 0;
  }

  void refresh_origin_acl() {
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);
  }

  int save_user_creds(const std::string &file, const std::string &username, const std::string &password, bool run_our_mouth) {
    pt::ptree outputTree;

    if (fs::exists(file)) {
      try {
        pt::read_json(file, outputTree);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read user credentials: "sv << e.what();
        return -1;
      }
    }

    auto salt = crypto::rand_alphabet(16);
    outputTree.put("username", username);
    outputTree.put("salt", salt);
    outputTree.put("password", util::hex(crypto::hash(password + salt)).to_string());
    try {
      pt::write_json(file, outputTree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "error writing to the credentials file, perhaps try this again as an administrator? Details: "sv << e.what();
      return -1;
    }

    BOOST_LOG(info) << "New credentials have been created"sv;
    return 0;
  }

  bool user_creds_exist(const std::string &file) {
    if (!fs::exists(file)) {
      return false;
    }

    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      return inputTree.find("username") != inputTree.not_found() &&
             inputTree.find("password") != inputTree.not_found() &&
             inputTree.find("salt") != inputTree.not_found();
    } catch (std::exception &e) {
      BOOST_LOG(error) << "validating user credentials: "sv << e.what();
    }

    return false;
  }

  int reload_user_creds(const std::string &file) {
    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      config::sunshine.username = inputTree.get<std::string>("username");
      config::sunshine.password = inputTree.get<std::string>("password");
      config::sunshine.salt = inputTree.get<std::string>("salt");
    } catch (std::exception &e) {
      BOOST_LOG(error) << "loading user credentials: "sv << e.what();
      return -1;
    }
    return 0;
  }

  int create_creds(const std::string &pkey, const std::string &cert) {
    fs::path pkey_path = pkey;
    fs::path cert_path = cert;

    auto creds = crypto::gen_creds("Sunshine Gamestream Host"sv, 2048);

    auto pkey_dir = pkey_path;
    auto cert_dir = cert_path;
    pkey_dir.remove_filename();
    cert_dir.remove_filename();

    std::error_code err_code {};
    fs::create_directories(pkey_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << pkey_dir << "] :"sv << err_code.message();
      return -1;
    }

    fs::create_directories(cert_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << cert_dir << "] :"sv << err_code.message();
      return -1;
    }

    if (file_handler::write_file(pkey.c_str(), creds.pkey)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.pkey << ']';
      return -1;
    }

    if (file_handler::write_file(cert.c_str(), creds.x509)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.cert << ']';
      return -1;
    }

    fs::permissions(pkey_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.pkey << "] :"sv << err_code.message();
      return -1;
    }

    fs::permissions(cert_path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.cert << "] :"sv << err_code.message();
      return -1;
    }

    return 0;
  }

  bool download_file(const std::string &url, const std::string &file, long ssl_version) {
    // sonar complains about weak ssl and tls versions; however sonar cannot detect the fix
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance";
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp) {
      BOOST_LOG(error) << "Couldn't open ["sv << file << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    // Perform the download
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);  // NOSONAR
    configure_curl_tls(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
      BOOST_LOG(error) << "Couldn't download ["sv << url << ", code:" << result << ']';
    }

    curl_easy_cleanup(curl);
    fclose(fp);
    return result == CURLE_OK;
  }

  bool configure_curl_tls(CURL *curl) {
    if (!curl) {
      return false;
    }
    ensure_curl_global_init();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    bool applied = apply_default_ca_store(curl);
    if (!applied) {
      BOOST_LOG(warning) << "Proceeding with libcurl default CA configuration";
    }
    return applied;
  }

  std::string url_escape(const std::string &url) {
    char *string = curl_easy_escape(nullptr, url.c_str(), static_cast<int>(url.length()));
    std::string result(string);
    curl_free(string);
    return result;
  }

  std::string url_get_host(const std::string &url) {
    CURLU *curlu = curl_url();
    curl_url_set(curlu, CURLUPART_URL, url.c_str(), static_cast<unsigned int>(url.length()));
    char *host;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }

  /**
   * @brief Escape a string for safe cookie usage, percent-encoding unsafe characters.
   * @param value The raw string to escape.
   * @return The escaped string.
   */
  std::string cookie_escape(const std::string &value) {
    char *out = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.length()));
    std::string result;
    if (out) {
      result.assign(out);
      curl_free(out);
    }
    return result;
  }

  /**
   * @brief Decode a percent-encoded cookie string.
   * @param value The escaped cookie string.
   * @return The original unescaped string.
   */
  std::string cookie_unescape(const std::string &value) {
    int outlen = 0;
    char *out = curl_easy_unescape(nullptr, value.c_str(), static_cast<int>(value.length()), &outlen);
    std::string result;
    if (out) {
      result.assign(out, outlen);
      curl_free(out);
    }
    return result;
  }

}  // namespace http
