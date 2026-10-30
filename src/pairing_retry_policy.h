/**
 * @file src/pairing_retry_policy.h
 * @brief Policy for phase-one pairing retries and pairing exchange expiry.
 */
#pragma once

#include <chrono>
#include <string_view>

namespace nvhttp::pairing_detail {

  /**
   * @brief How far the stored exchange for a client has progressed.
   */
  enum class stored_phase_e {
    awaiting_pin,  ///< Phase one is held until the user enters the PIN.
    phase_one_submitted,  ///< The PIN was delivered; no later phase has arrived yet.
    later,  ///< The client has sent at least one later phase.
  };

  /**
   * @brief What a phase-one (getservercert) request does to the stored exchange.
   */
  enum class phase_one_action_e {
    new_session,  ///< No exchange exists for this client.
    pending_retry,  ///< The exchange still awaits its PIN; the caller applies its own identity check.
    retransmit,  ///< Answer the same phase-one request again without changing the exchange.
    replace,  ///< End the stale exchange as failed and open a new one.
    reject_in_progress,  ///< Another peer or certificate owns the exchange.
  };

  struct phase_one_request_t {
    bool has_session = false;
    stored_phase_e stored_phase = stored_phase_e::awaiting_pin;
    std::string_view stored_address;
    std::string_view stored_certificate;
    std::string_view stored_salt;
    std::string_view received_address;
    std::string_view received_certificate;
    std::string_view received_salt;
  };

  constexpr phase_one_action_e phase_one_action(const phase_one_request_t &request) noexcept {
    if (!request.has_session) {
      return phase_one_action_e::new_session;
    }
    if (request.stored_phase == stored_phase_e::awaiting_pin) {
      return phase_one_action_e::pending_retry;
    }

    const bool same_client = !request.stored_address.empty() &&
                             request.stored_address == request.received_address &&
                             request.stored_certificate == request.received_certificate;
    if (!same_client) {
      return phase_one_action_e::reject_in_progress;
    }

    // Once the PIN has derived the cipher key, only an exact retransmission of
    // the answered phase-one request may reuse the exchange. Any other phase
    // one from the same peer and certificate means the client gave up on it
    // (for example after a wrong PIN) and starts over.
    if (request.stored_phase == stored_phase_e::phase_one_submitted &&
        request.stored_salt == request.received_salt) {
      return phase_one_action_e::retransmit;
    }
    return phase_one_action_e::replace;
  }

  /**
   * @brief Idle lifetime of a pairing exchange.
   * @details The user may take minutes to read and enter the PIN. Once it is
   *          delivered the client answers each phase at once, so an exchange
   *          left idle after that has been abandoned.
   */
  constexpr std::chrono::seconds session_ttl(const bool pin_delivered) noexcept {
    return pin_delivered ? std::chrono::seconds(30) : std::chrono::minutes(5);
  }

}  // namespace nvhttp::pairing_detail
