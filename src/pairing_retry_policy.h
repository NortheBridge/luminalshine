/**
 * @file src/pairing_retry_policy.h
 * @brief Policy for phase-one pairing retries and pairing exchange expiry.
 */
#pragma once

#include <chrono>
#include <string_view>

namespace nvhttp::pairing_detail {

  enum class stored_phase_e {
    awaiting_pin,
    phase_one_submitted,
    later,
  };

  enum class phase_one_action_e {
    new_session,
    pending_retry,
    retransmit,
    replace,
    reject_in_progress,
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

  // Test-first stub: the decisions are filled in by the fix for #216.
  constexpr phase_one_action_e phase_one_action(const phase_one_request_t &) noexcept {
    return phase_one_action_e::reject_in_progress;
  }

  constexpr std::chrono::seconds session_ttl(bool) noexcept {
    return std::chrono::minutes(5);
  }

}  // namespace nvhttp::pairing_detail
