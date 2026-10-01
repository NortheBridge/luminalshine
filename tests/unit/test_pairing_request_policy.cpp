/**
 * @file tests/unit/test_pairing_request_policy.cpp
 * @brief Regression tests for deterministic, connection-safe PIN request selection.
 */

#include "../tests_common.h"

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

#include <src/nvhttp.h>

namespace {
  using namespace std::chrono_literals;
  using nvhttp::pairing_detail::candidate_t;
  using nvhttp::pin_result_e;

  TEST(PairingRequestPolicy, ValidPinRequiresExactlyFourAsciiDigits) {
    EXPECT_TRUE(nvhttp::pairing_detail::valid_pin("0000"));
    EXPECT_TRUE(nvhttp::pairing_detail::valid_pin("9999"));
    EXPECT_FALSE(nvhttp::pairing_detail::valid_pin("123"));
    EXPECT_FALSE(nvhttp::pairing_detail::valid_pin("12345"));
    EXPECT_FALSE(nvhttp::pairing_detail::valid_pin("12a4"));
    EXPECT_FALSE(nvhttp::pairing_detail::valid_pin(" 123"));
  }

  TEST(PairingRequestPolicy, SessionExpiryUsesTheFiveMinuteBoundary) {
    const auto updated_at = std::chrono::steady_clock::time_point {10min};
    EXPECT_FALSE(nvhttp::pairing_detail::session_expired(updated_at, updated_at + 5min - 1ms));
    EXPECT_TRUE(nvhttp::pairing_detail::session_expired(updated_at, updated_at + 5min));
    EXPECT_TRUE(nvhttp::pairing_detail::session_expired(updated_at, updated_at + 6min));
  }

  TEST(PairingRequestPolicy, PendingPhaseOneRetryAllowsFreshSaltForSamePeerAndCertificate) {
    constexpr std::string_view address = "192.0.2.10";
    constexpr std::string_view certificate = "client-certificate";
    constexpr std::string_view salt = "0123456789abcdef0123456789abcdef";

    EXPECT_TRUE(nvhttp::pairing_detail::phase_one_retry_matches(
      address,
      certificate,
      salt,
      address,
      certificate,
      "fedcba9876543210fedcba9876543210",
      nvhttp::PAIR_PHASE::NONE
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_retry_matches(
      address,
      certificate,
      salt,
      "192.0.2.11",
      certificate,
      salt,
      nvhttp::PAIR_PHASE::NONE
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_retry_matches(
      address,
      certificate,
      salt,
      address,
      "different-certificate",
      salt,
      nvhttp::PAIR_PHASE::NONE
    ));
  }

  TEST(PairingRequestPolicy, SubmittedPhaseOneRetryRequiresOriginalSalt) {
    constexpr std::string_view address = "192.0.2.10";
    constexpr std::string_view certificate = "client-certificate";
    constexpr std::string_view salt = "0123456789abcdef0123456789abcdef";

    EXPECT_TRUE(nvhttp::pairing_detail::phase_one_retry_matches(
      address,
      certificate,
      salt,
      address,
      certificate,
      salt,
      nvhttp::PAIR_PHASE::GETSERVERCERT
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_retry_matches(
      address,
      certificate,
      salt,
      address,
      certificate,
      "fedcba9876543210fedcba9876543210",
      nvhttp::PAIR_PHASE::GETSERVERCERT
    ));
  }

  TEST(PairingRequestPolicy, PhaseOneRetryRejectsAnEmptyStoredPeerIdentity) {
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_retry_matches(
      "",
      "client-certificate",
      "0123456789abcdef0123456789abcdef",
      "192.0.2.10",
      "client-certificate",
      "0123456789abcdef0123456789abcdef",
      nvhttp::PAIR_PHASE::NONE
    ));
  }

  TEST(PairingRequestPolicy, DeliveryFailureOnlyOwnsUnchangedPhaseOneGeneration) {
    EXPECT_TRUE(nvhttp::pairing_detail::phase_one_delivery_is_current(
      nvhttp::PAIR_PHASE::GETSERVERCERT,
      7,
      7
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_delivery_is_current(
      nvhttp::PAIR_PHASE::GETSERVERCERT,
      8,
      7
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_delivery_is_current(
      nvhttp::PAIR_PHASE::CLIENTCHALLENGE,
      7,
      7
    ));
    EXPECT_FALSE(nvhttp::pairing_detail::phase_one_delivery_is_current(
      nvhttp::PAIR_PHASE::CLIENTPAIRINGSECRET,
      7,
      7
    ));
  }

  TEST(PairingRequestPolicy, CompletedPairingOutcomeSurvivesStaleDeliveryFailure) {
    EXPECT_EQ(
      nvhttp::pairing_detail::outcome_pin_result(nvhttp::pairing_state_e::PAIRED),
      pin_result_e::ALREADY_PAIRED
    );
    EXPECT_EQ(
      nvhttp::pairing_detail::outcome_pin_result(nvhttp::pairing_state_e::FAILED),
      pin_result_e::PAIRING_FAILED
    );
    EXPECT_EQ(
      nvhttp::pairing_detail::outcome_pin_result(nvhttp::pairing_state_e::EXPIRED),
      pin_result_e::EXPIRED
    );
  }

  TEST(PairingRequestPolicy, LegacySubmissionRequiresExactlyOnePendingRequest) {
    const std::array<candidate_t, 0> none {};
    EXPECT_EQ(
      nvhttp::pairing_detail::select_candidate(none, std::nullopt).result,
      pin_result_e::NO_PENDING_REQUEST
    );

    const std::array one {
      candidate_t {.pairing_id = "tv-a", .awaiting_pin = true, .response_available = true},
    };
    const auto selected = nvhttp::pairing_detail::select_candidate(one, std::nullopt);
    EXPECT_EQ(selected.result, pin_result_e::PIN_DELIVERED);
    EXPECT_EQ(selected.pairing_id, "tv-a");

    const std::array two {
      candidate_t {.pairing_id = "tv-a", .awaiting_pin = true, .response_available = true},
      candidate_t {.pairing_id = "tv-b", .awaiting_pin = true, .response_available = true},
    };
    const auto ambiguous = nvhttp::pairing_detail::select_candidate(two, std::nullopt);
    EXPECT_EQ(ambiguous.result, pin_result_e::AMBIGUOUS_REQUEST);
    EXPECT_TRUE(ambiguous.pairing_id.empty());
  }

  TEST(PairingRequestPolicy, LegacySubmissionIgnoresAnExchangeAlreadyAwaitingProof) {
    const std::array candidates {
      candidate_t {.pairing_id = "already-submitted", .awaiting_pin = false, .response_available = false},
      candidate_t {.pairing_id = "new-request", .awaiting_pin = true, .response_available = true},
    };
    const auto selected = nvhttp::pairing_detail::select_candidate(candidates, std::nullopt);
    EXPECT_EQ(selected.result, pin_result_e::PIN_DELIVERED);
    EXPECT_EQ(selected.pairing_id, "new-request");
  }

  TEST(PairingRequestPolicy, ExplicitSubmissionTargetsOnlyTheRequestedClient) {
    const std::array candidates {
      candidate_t {.pairing_id = "tv-a", .awaiting_pin = true, .response_available = true},
      candidate_t {.pairing_id = "tv-b", .awaiting_pin = true, .response_available = true},
    };
    const auto selected = nvhttp::pairing_detail::select_candidate(
      candidates,
      std::optional<std::string_view> {"tv-b"}
    );
    EXPECT_EQ(selected.result, pin_result_e::PIN_DELIVERED);
    EXPECT_EQ(selected.pairing_id, "tv-b");

    const auto unknown = nvhttp::pairing_detail::select_candidate(
      candidates,
      std::optional<std::string_view> {"not-present"}
    );
    EXPECT_EQ(unknown.result, pin_result_e::UNKNOWN_PAIRING_ID);
    EXPECT_EQ(unknown.pairing_id, "not-present");
  }

  TEST(PairingRequestPolicy, ExplicitRetryIsIdempotentAndMissingResponseIsReported) {
    const std::array submitted {
      candidate_t {.pairing_id = "tv-a", .awaiting_pin = false, .response_available = false},
    };
    const auto retry = nvhttp::pairing_detail::select_candidate(
      submitted,
      std::optional<std::string_view> {"tv-a"}
    );
    EXPECT_EQ(retry.result, pin_result_e::ALREADY_SUBMITTED);
    EXPECT_TRUE(retry.accepted());

    const std::array unavailable {
      candidate_t {.pairing_id = "tv-b", .awaiting_pin = true, .response_available = false},
    };
    const auto missing_response = nvhttp::pairing_detail::select_candidate(
      unavailable,
      std::optional<std::string_view> {"tv-b"}
    );
    EXPECT_EQ(missing_response.result, pin_result_e::RESPONSE_UNAVAILABLE);
    EXPECT_FALSE(missing_response.accepted());
  }

  TEST(PairingRequestPolicy, AcceptedResultsNeverTreatFailuresAsPaired) {
    EXPECT_TRUE(nvhttp::pin_result_t {.result = pin_result_e::PIN_DELIVERED}.accepted());
    EXPECT_TRUE(nvhttp::pin_result_t {.result = pin_result_e::ALREADY_SUBMITTED}.accepted());
    EXPECT_TRUE(nvhttp::pin_result_t {.result = pin_result_e::ALREADY_PAIRED}.accepted());
    EXPECT_FALSE(nvhttp::pin_result_t {.result = pin_result_e::INVALID_PIN}.accepted());
    EXPECT_FALSE(nvhttp::pin_result_t {.result = pin_result_e::AMBIGUOUS_REQUEST}.accepted());
    EXPECT_FALSE(nvhttp::pin_result_t {.result = pin_result_e::EXPIRED}.accepted());
    EXPECT_FALSE(nvhttp::pin_result_t {.result = pin_result_e::PAIRING_FAILED}.accepted());
  }

  TEST(PairingRequestPolicy, WireResultNamesRemainStable) {
    EXPECT_EQ(nvhttp::pin_result_name(pin_result_e::PIN_DELIVERED), "pin_delivered");
    EXPECT_EQ(nvhttp::pin_result_name(pin_result_e::AMBIGUOUS_REQUEST), "ambiguous_request");
    EXPECT_EQ(nvhttp::pin_result_name(pin_result_e::PAIRING_FAILED), "pairing_failed");
    EXPECT_EQ(nvhttp::pairing_state_name(nvhttp::pairing_state_e::AWAITING_CLIENT), "awaiting_client");
    EXPECT_EQ(nvhttp::pairing_state_name(nvhttp::pairing_state_e::PAIRED), "paired");
  }
}  // namespace
