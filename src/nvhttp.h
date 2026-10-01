/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"
#include "thread_safe.h"

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Gracefully shutdown the TLS connection
      SimpleWeb::error_code ec;
      shutdown(ec);
    }
  };

  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
  };

  using pair_http_response_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using pair_https_response_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using pair_response_t = util::Either<pair_http_response_t, pair_https_response_t>;

  struct pair_session_t {
    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key = {};
    std::vector<uint8_t> clienthash = {};

    std::string serversecret = {};
    std::string serverchallenge = {};

    struct {
      pair_response_t response;
      std::string salt = {};
    } async_insert_pin;

    // Opaque host-generated identifier used by the authenticated Web UI to
    // address this exact pending request.  The GameStream uniqueID remains the
    // protocol map key and is deliberately not exposed through /api/pin.
    std::string pairing_id = {};
    std::string client_address = {};
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point updated_at = created_at;
    // Incremented for every accepted phase-one request, including an
    // idempotent retry after the PIN has been submitted. This lets the PIN
    // delivery path distinguish its held HTTP response from a newer retry.
    std::uint64_t phase_one_generation = 0;

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
  };

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   */
  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  enum class pairing_state_e {
    PENDING_PIN,
    AWAITING_CLIENT,
    PAIRED,
    FAILED,
    EXPIRED
  };

  enum class pin_result_e {
    PIN_DELIVERED,
    ALREADY_SUBMITTED,
    ALREADY_PAIRED,
    NO_PENDING_REQUEST,
    AMBIGUOUS_REQUEST,
    UNKNOWN_PAIRING_ID,
    EXPIRED,
    INVALID_PIN,
    RESPONSE_UNAVAILABLE,
    DELIVERY_FAILED,
    PAIRING_FAILED
  };

  struct pairing_request_t {
    std::string pairing_id;
    std::string client_address;
    pairing_state_e state = pairing_state_e::PENDING_PIN;
    std::uint64_t age_seconds = 0;
    std::string device_uuid;
  };

  struct pin_result_t {
    pin_result_e result = pin_result_e::NO_PENDING_REQUEST;
    std::string pairing_id;

    [[nodiscard]] bool accepted() const noexcept {
      return result == pin_result_e::PIN_DELIVERED ||
             result == pin_result_e::ALREADY_SUBMITTED ||
             result == pin_result_e::ALREADY_PAIRED;
    }
  };

  namespace pairing_detail {
    struct candidate_t {
      std::string_view pairing_id;
      bool awaiting_pin = false;
      bool response_available = false;
    };

    bool valid_pin(std::string_view pin) noexcept;
    bool session_expired(
      std::chrono::steady_clock::time_point updated_at,
      std::chrono::steady_clock::time_point now
    ) noexcept;
    bool phase_one_retry_matches(
      std::string_view expected_address,
      std::string_view expected_certificate,
      std::string_view expected_salt,
      std::string_view received_address,
      std::string_view received_certificate,
      std::string_view received_salt,
      PAIR_PHASE current_phase
    ) noexcept;
    bool phase_one_delivery_is_current(
      PAIR_PHASE current_phase,
      std::uint64_t current_generation,
      std::uint64_t attempted_generation
    ) noexcept;
    pin_result_e outcome_pin_result(pairing_state_e state) noexcept;
    pin_result_t select_candidate(
      std::span<const candidate_t> candidates,
      std::optional<std::string_view> pairing_id
    );
  }  // namespace pairing_detail

  /**
   * @brief Return active and recently-completed pairing requests for the authenticated UI.
   * @details Entries contain only an opaque request id, peer address, state, age and final
   *          device UUID. Certificate, salt and PIN material are never exposed.
   */
  std::vector<pairing_request_t> get_pairing_requests();

  /**
   * @brief Submit a PIN to one exact pending request.
   * @param pairing_id Opaque id returned by get_pairing_requests(). If omitted, legacy
   *        behavior is retained only when exactly one eligible request exists.
   */
  pin_result_t submit_pin(std::string pin, std::string name, std::optional<std::string> pairing_id = std::nullopt);

  std::string_view pairing_state_name(pairing_state_e state) noexcept;
  std::string_view pin_result_name(pin_result_e result) noexcept;

  /**
   * @brief Compare the user supplied pin to the Moonlight pin.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if the pin is correct, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("1234", "laptop");
   * @examples_end
   */
  bool pin(std::string pin, std::string name);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  /**
   * @brief Record a client's last seen time (seconds since epoch).
   */
  void mark_client_last_seen(const std::string &uuid);

  /**
   * @brief Update stored settings for a paired client.
   * @return True if the client was found and updated.
   */
  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const std::string &output_name_override,
    bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout,
    std::optional<std::unordered_map<std::string, std::string>> config_overrides,
    std::optional<bool> prefer_10bit_sdr,
    std::optional<std::string> hdr_profile
  );

  /**
   * @brief Disconnect any active sessions for a paired client.
   * @return True if one or more sessions were stopped.
   */
  bool disconnect_client(const std::string &uuid);

  /**
   * @brief Get a client's prefer_10bit_sdr override.
   */
  std::optional<bool> get_client_prefer_10bit_sdr_override(const std::string &uuid);

  /**
   * @brief Persist a per-client HDR color profile selection (Windows only).
   * @return True if the client was found and updated.
   */
  bool set_client_hdr_profile(const std::string &uuid, const std::string &hdr_profile);

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();

  /**
   * @brief Persist current nvhttp-related state (paired clients, update subsystem markers, etc.).
   * @note Exposed so subsystems (e.g. update) can trigger a save after mutating persisted fields.
   */
  void save_state();

  /**
   * @brief Result of a state-file reset operation surfaced via the Troubleshooting UI.
   */
  struct reset_state_result_t {
    bool status;  ///< True on full success.
    std::string error;  ///< Empty on success; populated with a user-facing message otherwise.
    std::vector<std::string> archived;  ///< Paths the original files were renamed to (.corrupt-<timestamp>).
  };

  /**
   * @brief Reset the on-disk pairing/state files to a clean slate.
   *
   * Renames "sunshine_state.json" and "luminalshine_state.json" (plus their
   * .bak siblings) to "<file>.corrupt-<UTC-timestamp>" so the user can
   * still recover them by hand if needed, clears all in-memory pairings,
   * generates a new host uniqueid, and persists the fresh state.
   *
   * Does NOT touch the dedicated credentials file — admin login survives.
   */
  reset_state_result_t reset_state();
}  // namespace nvhttp
