/**
 * @file tests/unit/test_vgd_ring_consumers.cpp
 * @brief Unit tests for the LuminalVGD frame-ring consumer registry.
 */

#include "src/platform/windows/vgd_ring_consumers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  using platf::video_worker::ring_consumers::decision_e;
  using platf::video_worker::ring_consumers::registry_t;
  using platf::video_worker::ring_consumers::release_wait_budget;
  using platf::video_worker::ring_consumers::ring_lease_t;

  constexpr std::uint64_t ring_a = 0xA001;
  constexpr std::uint64_t ring_b = 0xB002;

  TEST(VgdRingConsumers, FirstWorkerClaimsRing) {
    registry_t registry;
    auto result = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    EXPECT_EQ(result.decision, decision_e::claim);
    EXPECT_TRUE(result.lease);
    EXPECT_TRUE(registry.is_held(ring_a));
  }

  TEST(VgdRingConsumers, SecondLiveWorkerOnSameRingGetsSafeCapture) {
    registry_t registry;
    auto first = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    ASSERT_EQ(first.decision, decision_e::claim);

    auto second = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    EXPECT_EQ(second.decision, decision_e::use_safe_capture);
    EXPECT_FALSE(second.lease);

    // A losing joiner must not disturb the live holder.
    EXPECT_TRUE(registry.is_held(ring_a));
    auto third = registry.try_acquire(ring_a, [] { return false; }, 10s);
    EXPECT_EQ(third.decision, decision_e::use_safe_capture);
  }

  TEST(VgdRingConsumers, RetiringHolderIsWaitedForThenClaimed) {
    registry_t registry;
    bool retiring = false;
    auto first = registry.try_acquire(ring_a, [&] { return retiring; }, 0ms);
    ASSERT_EQ(first.decision, decision_e::claim);

    // The holder is shutting down: the next worker waits instead of falling back.
    retiring = true;
    auto waiting = registry.try_acquire(ring_a, [] { return false; }, 100ms);
    EXPECT_EQ(waiting.decision, decision_e::wait_for_release);
    EXPECT_FALSE(waiting.lease);

    // Once the holder's worker is reaped and its lease released, the retry claims the ring.
    first.lease.reset();
    auto claimed = registry.try_acquire(ring_a, [] { return false; }, 200ms);
    EXPECT_EQ(claimed.decision, decision_e::claim);
    EXPECT_TRUE(claimed.lease);
  }

  TEST(VgdRingConsumers, RetiringHolderPastBudgetGetsSafeCapture) {
    registry_t registry;
    auto first = registry.try_acquire(ring_a, [] { return true; }, 0ms);
    ASSERT_EQ(first.decision, decision_e::claim);

    // One tick inside the budget still waits; the budget itself is exhausted.
    auto inside = registry.try_acquire(ring_a, [] { return false; }, release_wait_budget - 1ms);
    EXPECT_EQ(inside.decision, decision_e::wait_for_release);
    auto exhausted = registry.try_acquire(ring_a, [] { return false; }, release_wait_budget);
    EXPECT_EQ(exhausted.decision, decision_e::use_safe_capture);
    auto beyond = registry.try_acquire(ring_a, [] { return false; }, release_wait_budget + 1s);
    EXPECT_EQ(beyond.decision, decision_e::use_safe_capture);
    EXPECT_FALSE(beyond.lease);
  }

  TEST(VgdRingConsumers, ReleasedLeaseLetsNextWorkerClaim) {
    registry_t registry;
    {
      auto first = registry.try_acquire(ring_a, [] { return false; }, 0ms);
      ASSERT_EQ(first.decision, decision_e::claim);
      EXPECT_TRUE(registry.is_held(ring_a));
    }
    EXPECT_FALSE(registry.is_held(ring_a));

    auto next = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    EXPECT_EQ(next.decision, decision_e::claim);
  }

  TEST(VgdRingConsumers, DifferentRingSessionsAreIndependent) {
    registry_t registry;
    auto first = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    auto other = registry.try_acquire(ring_b, [] { return false; }, 0ms);
    EXPECT_EQ(first.decision, decision_e::claim);
    EXPECT_EQ(other.decision, decision_e::claim);

    // A new generation of the display is a new ring session and is not blocked by the old one.
    first.lease.reset();
    EXPECT_FALSE(registry.is_held(ring_a));
    EXPECT_TRUE(registry.is_held(ring_b));
  }

  TEST(VgdRingConsumers, MissingRetiringPredicateIsNeverRetiring) {
    registry_t registry;
    auto first = registry.try_acquire(ring_a, nullptr, 0ms);
    ASSERT_EQ(first.decision, decision_e::claim);
    auto second = registry.try_acquire(ring_a, nullptr, 0ms);
    EXPECT_EQ(second.decision, decision_e::use_safe_capture);
  }

  TEST(VgdRingConsumers, LeaseReleasesExactlyOnceAfterMove) {
    registry_t registry;
    auto first = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    ASSERT_EQ(first.decision, decision_e::claim);

    ring_lease_t moved = std::move(first.lease);
    EXPECT_FALSE(first.lease);
    EXPECT_TRUE(moved);

    // Resetting the moved-from lease is a no-op: the ring stays held.
    first.lease.reset();
    EXPECT_TRUE(registry.is_held(ring_a));

    moved.reset();
    EXPECT_FALSE(registry.is_held(ring_a));

    // A new holder must not be released by a stale lease resetting a second time.
    auto next = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    ASSERT_EQ(next.decision, decision_e::claim);
    moved.reset();
    EXPECT_TRUE(registry.is_held(ring_a));
  }

  TEST(VgdRingConsumers, MoveAssignReleasesTheRingTheTargetHeld) {
    registry_t registry;
    auto on_a = registry.try_acquire(ring_a, [] { return false; }, 0ms);
    auto on_b = registry.try_acquire(ring_b, [] { return false; }, 0ms);
    ASSERT_EQ(on_a.decision, decision_e::claim);
    ASSERT_EQ(on_b.decision, decision_e::claim);

    on_a.lease = std::move(on_b.lease);
    EXPECT_FALSE(registry.is_held(ring_a));
    EXPECT_TRUE(registry.is_held(ring_b));

    on_a.lease.reset();
    EXPECT_FALSE(registry.is_held(ring_b));
  }

  TEST(VgdRingConsumers, SameClientReconnectTakesOverALiveHolder) {
    registry_t registry;
    auto stale = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-a");
    ASSERT_EQ(stale.decision, decision_e::claim);

    // The client dropped without a clean disconnect and came back before its old
    // session timed out. That session is not retiring yet, but the reconnect is the
    // same client, so it owns the ring instead of starting on WGC.
    auto reconnect = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-a");
    EXPECT_EQ(reconnect.decision, decision_e::claim);
    EXPECT_TRUE(reconnect.lease);
  }

  TEST(VgdRingConsumers, SameClientWaitsForItsRetiringSessionThenTakesOver) {
    registry_t registry;
    bool retiring = false;
    auto old_session = registry.try_acquire(ring_a, [&] { return retiring; }, 0ms, "client-a");
    ASSERT_EQ(old_session.decision, decision_e::claim);

    // A clean quit and relaunch: the old worker is still being reaped. Waiting for
    // it avoids splitting the first frames between the dying and the new worker.
    retiring = true;
    EXPECT_EQ(registry.try_acquire(ring_a, [] { return false; }, 100ms, "client-a").decision, decision_e::wait_for_release);

    // If the old worker outlives the budget, the reconnect still takes the ring over.
    auto reconnect = registry.try_acquire(ring_a, [] { return false; }, release_wait_budget, "client-a");
    EXPECT_EQ(reconnect.decision, decision_e::claim);
    EXPECT_TRUE(reconnect.lease);
  }

  TEST(VgdRingConsumers, SupersededLeaseDoesNotReleaseTheNewHolder) {
    registry_t registry;
    auto stale = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-a");
    auto reconnect = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-a");
    ASSERT_EQ(reconnect.decision, decision_e::claim);

    // The old session times out later; its release must leave the reconnect holding the ring.
    stale.lease.reset();
    EXPECT_TRUE(registry.is_held(ring_a));
    auto joiner = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-b");
    EXPECT_EQ(joiner.decision, decision_e::use_safe_capture);

    reconnect.lease.reset();
    EXPECT_FALSE(registry.is_held(ring_a));
  }

  TEST(VgdRingConsumers, OtherOrUnknownClientNeverTakesOver) {
    registry_t registry;
    auto holder = registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-a");
    ASSERT_EQ(holder.decision, decision_e::claim);
    EXPECT_EQ(registry.try_acquire(ring_a, [] { return false; }, 0ms, "client-b").decision, decision_e::use_safe_capture);
    EXPECT_EQ(registry.try_acquire(ring_a, [] { return false; }, 0ms, "").decision, decision_e::use_safe_capture);

    // An unidentified holder cannot be matched to a reconnect either.
    auto anonymous = registry.try_acquire(ring_b, [] { return false; }, 0ms);
    ASSERT_EQ(anonymous.decision, decision_e::claim);
    EXPECT_EQ(registry.try_acquire(ring_b, [] { return false; }, 0ms).decision, decision_e::use_safe_capture);
  }

  TEST(VgdRingConsumers, OnlyOneConcurrentWorkerClaimsTheRing) {
    registry_t registry;
    constexpr int workers = 16;
    std::vector<registry_t::acquire_result_t> results(workers);
    {
      std::vector<std::jthread> threads;
      threads.reserve(workers);
      for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&registry, &results, i] {
          results[i] = registry.try_acquire(ring_a, [] { return false; }, 0ms);
        });
      }
    }

    int claims = 0;
    for (const auto &result : results) {
      if (result.decision == decision_e::claim) {
        ++claims;
        EXPECT_TRUE(result.lease);
      } else {
        EXPECT_EQ(result.decision, decision_e::use_safe_capture);
        EXPECT_FALSE(result.lease);
      }
    }
    EXPECT_EQ(claims, 1);
  }

}  // namespace
