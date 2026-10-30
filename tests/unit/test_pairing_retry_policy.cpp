/**
 * @file tests/unit/test_pairing_retry_policy.cpp
 * @brief Unit tests for the phase-one retry and pairing exchange expiry policy.
 */

#include "src/pairing_retry_policy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

namespace {

  using nvhttp::pairing_detail::phase_one_action;
  using nvhttp::pairing_detail::phase_one_action_e;
  using nvhttp::pairing_detail::phase_one_request_t;
  using nvhttp::pairing_detail::stored_phase_e;

  constexpr std::string_view address = "192.0.2.10";
  constexpr std::string_view certificate = "client-certificate";
  constexpr std::string_view salt = "0123456789abcdef0123456789abcdef";
  constexpr std::string_view fresh_salt = "fedcba9876543210fedcba9876543210";

  phase_one_request_t same_client(const stored_phase_e phase, const std::string_view received_salt) {
    return {
      .has_session = true,
      .stored_phase = phase,
      .stored_address = address,
      .stored_certificate = certificate,
      .stored_salt = salt,
      .received_address = address,
      .received_certificate = certificate,
      .received_salt = received_salt,
    };
  }

  TEST(PairingRetryPolicy, UnknownClientStartsANewSession) {
    EXPECT_EQ(phase_one_action(phase_one_request_t {}), phase_one_action_e::new_session);
  }

  TEST(PairingRetryPolicy, ClientAwaitingItsPinKeepsThePendingRetryPath) {
    EXPECT_EQ(phase_one_action(same_client(stored_phase_e::awaiting_pin, fresh_salt)), phase_one_action_e::pending_retry);

    auto other_peer = same_client(stored_phase_e::awaiting_pin, salt);
    other_peer.received_address = "192.0.2.11";
    EXPECT_EQ(phase_one_action(other_peer), phase_one_action_e::pending_retry);
  }

  TEST(PairingRetryPolicy, SameSaltRetransmissionAfterPinDeliveryIsIdempotent) {
    EXPECT_EQ(phase_one_action(same_client(stored_phase_e::phase_one_submitted, salt)), phase_one_action_e::retransmit);
  }

  TEST(PairingRetryPolicy, FreshSaltFromSameClientAfterPinDeliveryReplacesTheExchange) {
    EXPECT_EQ(phase_one_action(same_client(stored_phase_e::phase_one_submitted, fresh_salt)), phase_one_action_e::replace);
  }

  TEST(PairingRetryPolicy, SameClientRetryAfterALaterPhaseReplacesTheExchange) {
    // Only an unanswered phase one may be retransmitted. Once the client has
    // moved on, even a request that reuses the old salt starts over.
    EXPECT_EQ(phase_one_action(same_client(stored_phase_e::later, fresh_salt)), phase_one_action_e::replace);
    EXPECT_EQ(phase_one_action(same_client(stored_phase_e::later, salt)), phase_one_action_e::replace);
  }

  TEST(PairingRetryPolicy, DifferentPeerAddressKeepsTheExchangeInProgress) {
    for (const auto phase : {stored_phase_e::phase_one_submitted, stored_phase_e::later}) {
      auto request = same_client(phase, fresh_salt);
      request.received_address = "192.0.2.11";
      EXPECT_EQ(phase_one_action(request), phase_one_action_e::reject_in_progress);

      request.received_salt = salt;
      EXPECT_EQ(phase_one_action(request), phase_one_action_e::reject_in_progress);
    }
  }

  TEST(PairingRetryPolicy, DifferentCertificateKeepsTheExchangeInProgress) {
    for (const auto phase : {stored_phase_e::phase_one_submitted, stored_phase_e::later}) {
      auto request = same_client(phase, fresh_salt);
      request.received_certificate = "different-certificate";
      EXPECT_EQ(phase_one_action(request), phase_one_action_e::reject_in_progress);

      request.received_salt = salt;
      EXPECT_EQ(phase_one_action(request), phase_one_action_e::reject_in_progress);
    }
  }

  TEST(PairingRetryPolicy, EmptyStoredPeerAddressNeverMatches) {
    for (const auto phase : {stored_phase_e::phase_one_submitted, stored_phase_e::later}) {
      for (const auto received_salt : {salt, fresh_salt}) {
        auto request = same_client(phase, received_salt);
        request.stored_address = "";
        request.received_address = "";
        EXPECT_EQ(phase_one_action(request), phase_one_action_e::reject_in_progress);
      }
    }
  }

  TEST(PairingRetryPolicy, ExchangeAwaitingItsPinKeepsTheFiveMinuteLifetime) {
    EXPECT_EQ(nvhttp::pairing_detail::session_ttl(false), std::chrono::minutes(5));
  }

  TEST(PairingRetryPolicy, ExchangeWithDeliveredPinExpiresAfterThirtySeconds) {
    EXPECT_EQ(nvhttp::pairing_detail::session_ttl(true), std::chrono::seconds(30));
  }

}  // namespace
