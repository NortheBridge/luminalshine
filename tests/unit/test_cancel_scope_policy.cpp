/**
 * @file tests/unit/test_cancel_scope_policy.cpp
 * @brief Unit tests for the /cancel scope policy.
 */

#include "src/cancel_scope_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace {

  using cancel_scope::decide;
  using cancel_scope::other_clients;
  using cancel_scope::scope_e;
  using cancel_scope::session_t;

  constexpr std::string_view client_a = "aaaaaaaa-0000-0000-0000-000000000001";
  constexpr std::string_view client_b = "bbbbbbbb-0000-0000-0000-000000000002";

  TEST(CancelScopePolicy, SoleRequesterGetsFullCancel) {
    const std::vector<session_t> sessions {{client_a, false}};
    EXPECT_EQ(decide(client_a, sessions, std::nullopt, false), scope_e::full);
    EXPECT_EQ(other_clients(client_a, sessions, std::nullopt, false), 0U);

    // A client may hold several sessions; all of them are the requester's own.
    const std::vector<session_t> two_sessions {{client_a, false}, {client_a, false}};
    EXPECT_EQ(decide(client_a, two_sessions, std::nullopt, false), scope_e::full);

    // Nothing tracked at all is still a full cancel (cleans up an idle host).
    EXPECT_EQ(decide(client_a, {}, std::nullopt, false), scope_e::full);
  }

  TEST(CancelScopePolicy, OtherLiveClientKeepsAppRunning) {
    const std::vector<session_t> sessions {{client_a, false}, {client_b, false}};
    EXPECT_EQ(decide(client_b, sessions, std::nullopt, false), scope_e::requester_only);
    EXPECT_EQ(other_clients(client_b, sessions, std::nullopt, false), 1U);
    EXPECT_EQ(decide(client_a, sessions, std::nullopt, false), scope_e::requester_only);
  }

  TEST(CancelScopePolicy, StoppingOtherSessionDoesNotBlockFullCancel) {
    const std::vector<session_t> sessions {{client_a, true}, {client_b, false}};
    EXPECT_EQ(decide(client_b, sessions, std::nullopt, false), scope_e::full);
    EXPECT_EQ(other_clients(client_b, sessions, std::nullopt, false), 0U);
  }

  TEST(CancelScopePolicy, UnknownRequesterNeverEndsAnotherClientsSession) {
    const std::vector<session_t> sessions {{client_a, false}};
    EXPECT_EQ(decide({}, sessions, std::nullopt, false), scope_e::requester_only);

    // A requester that matches nobody is just as unable to end A's session.
    EXPECT_EQ(decide("cccccccc-0000-0000-0000-000000000003", sessions, std::nullopt, false), scope_e::requester_only);

    // An unknown requester also must not cancel the pending launch of a known client.
    EXPECT_EQ(decide({}, {}, std::optional<std::string_view> {client_a}, false), scope_e::requester_only);
  }

  TEST(CancelScopePolicy, PendingLaunchOfAnotherClientCountsAsOther) {
    const std::optional<std::string_view> pending {client_a};
    EXPECT_EQ(decide(client_b, {}, pending, false), scope_e::requester_only);
    EXPECT_EQ(other_clients(client_b, {}, pending, false), 1U);

    // A pending launch with no owner identity cannot be proven the requester's.
    const std::optional<std::string_view> anonymous_pending {std::string_view {}};
    EXPECT_EQ(decide(client_b, {}, anonymous_pending, false), scope_e::requester_only);
  }

  TEST(CancelScopePolicy, AnonymousOtherSessionCountsAsOther) {
    const std::vector<session_t> sessions {{std::string_view {}, false}};
    EXPECT_EQ(decide(client_b, sessions, std::nullopt, false), scope_e::requester_only);
    EXPECT_EQ(other_clients(client_b, sessions, std::nullopt, false), 1U);

    // Even an anonymous requester does not own an anonymous session.
    EXPECT_EQ(decide({}, sessions, std::nullopt, false), scope_e::requester_only);
  }

  TEST(CancelScopePolicy, RequestersOwnPendingLaunchIsCancellable) {
    const std::optional<std::string_view> pending {client_a};
    EXPECT_EQ(decide(client_a, {}, pending, false), scope_e::full);

    const std::vector<session_t> sessions {{client_a, false}};
    EXPECT_EQ(decide(client_a, sessions, pending, false), scope_e::full);
  }

  TEST(CancelScopePolicy, ActiveBrowserSessionCountsAsOther) {
    EXPECT_EQ(decide(client_a, {}, std::nullopt, true), scope_e::requester_only);
    EXPECT_EQ(other_clients(client_a, {}, std::nullopt, true), 1U);

    const std::vector<session_t> sessions {{client_a, false}};
    EXPECT_EQ(decide(client_a, sessions, std::nullopt, true), scope_e::requester_only);
  }

  TEST(CancelScopePolicy, EveryOtherClientIsCounted) {
    const std::vector<session_t> sessions {{client_a, false}, {client_b, false}, {std::string_view {}, false}, {client_b, true}};
    const std::optional<std::string_view> pending {"dddddddd-0000-0000-0000-000000000004"};
    // client_a and the anonymous session, the foreign pending launch and the browser; client_b's own and stopping sessions are not counted.
    EXPECT_EQ(other_clients(client_b, sessions, pending, true), 4U);
  }

}  // namespace
