/**
 * @file tests/unit/test_session_lifecycle_coordinator.cpp
 * @brief Regression tests for launch/last-session cleanup serialization.
 */

#include "src/session_lifecycle_coordinator.h"

#include <gtest/gtest.h>

#include <memory>

using namespace std::chrono_literals;

namespace {

  using stream::lifecycle::cleanup_acquire_result_e;
  using stream::lifecycle::coordinator_t;

  TEST(SessionLifecycleCoordinator, StoppingRtspSessionDoesNotRequireReapingUnderLaunchLease) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());

    // A STOPPING session has already left the logical active count even if it
    // remains in RTSP's container awaiting join. The launch path must use this
    // non-blocking policy instead of calling the reaping session_count().
    EXPECT_TRUE(stream::lifecycle::capture_is_logically_idle(0, false));
    EXPECT_FALSE(stream::lifecycle::capture_is_logically_idle(1, false));
    EXPECT_FALSE(stream::lifecycle::capture_is_logically_idle(0, true));
  }

  TEST(SessionLifecycleCoordinator, CleanupRevalidationRejectsAStartedSuccessor) {
    EXPECT_TRUE(stream::lifecycle::last_session_cleanup_still_valid(0, 0));
    EXPECT_FALSE(stream::lifecycle::last_session_cleanup_still_valid(1, 1));
    EXPECT_FALSE(stream::lifecycle::last_session_cleanup_still_valid(1, 0));
    EXPECT_FALSE(stream::lifecycle::last_session_cleanup_still_valid(0, 1));
  }

  TEST(SessionLifecycleCoordinator, UnclaimedLaunchExpiryReopensRtspWhenIdle) {
    EXPECT_TRUE(stream::lifecycle::expired_launch_should_clear_rtsp_active(0));
    EXPECT_FALSE(stream::lifecycle::expired_launch_should_clear_rtsp_active(1));
  }

  TEST(SessionLifecycleCoordinator, ExpiryCannotCancelClaimedAdmissionButExplicitCancelCan) {
    stream::lifecycle::admission_state_policy_t admission;
    EXPECT_TRUE(admission.can_start());
    EXPECT_TRUE(admission.mark_claimed());
    EXPECT_FALSE(admission.retire(false));
    EXPECT_TRUE(admission.retire(true));
    EXPECT_FALSE(admission.can_start());
  }

  TEST(SessionLifecycleCoordinator, CancelFirstPreventsLateAnnounceClaim) {
    stream::lifecycle::admission_state_policy_t admission;
    EXPECT_TRUE(admission.retire(true));
    EXPECT_FALSE(admission.mark_claimed());
  }

  TEST(SessionLifecycleCoordinator, AnnounceFirstSurvivesTimeoutButNotExplicitCancel) {
    stream::lifecycle::admission_state_policy_t admission;
    ASSERT_TRUE(admission.mark_claimed());
    EXPECT_FALSE(admission.retire(false));
    EXPECT_TRUE(admission.retire(true));
    EXPECT_FALSE(admission.can_start());
  }

  TEST(SessionLifecycleCoordinator, DuplicateAcceptedSocketCannotClaimSameLaunch) {
    stream::lifecycle::admission_state_policy_t admission;
    ASSERT_TRUE(admission.mark_claimed());
    EXPECT_FALSE(admission.can_start());
    EXPECT_FALSE(admission.mark_claimed());
  }

  TEST(SessionLifecycleCoordinator, CleanupOwnerMakesLaunchFailBoundedly) {
    coordinator_t coordinator;
    auto cleanup = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(cleanup.result, cleanup_acquire_result_e::acquired);
    ASSERT_TRUE(cleanup.lease.has_value());

    EXPECT_FALSE(coordinator.acquire_launch(1ms).has_value());

    cleanup.lease.reset();
    EXPECT_TRUE(coordinator.acquire_launch(0ms).has_value());
  }

  TEST(SessionLifecycleCoordinator, CommittedLaunchSupersedesOldCleanup) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());
    launch->commit();

    auto cleanup = coordinator.acquire_cleanup(0ms);
    EXPECT_EQ(cleanup.result, cleanup_acquire_result_e::superseded_by_launch);
    EXPECT_FALSE(cleanup.lease.has_value());
  }

  TEST(SessionLifecycleCoordinator, PendingLaunchOwnerSurvivesSessionStartCopyRelease) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());

    auto pending_event_owner = std::make_shared<coordinator_t::launch_lease_t>(std::move(*launch));
    pending_event_owner->commit();
    auto starting_session_owner = pending_event_owner;

    // session::start drops its copy, but RTSP's pending launch event must keep
    // the gate occupied until control claim/timeout. A retry cannot enter and
    // bind to the wrong pending GameStream keys.
    starting_session_owner.reset();
    EXPECT_FALSE(coordinator.acquire_launch(1ms).has_value());

    pending_event_owner.reset();
    EXPECT_TRUE(coordinator.acquire_launch(0ms).has_value());
  }

  TEST(SessionLifecycleCoordinator, AbortedLaunchLetsCleanupProceed) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());
    launch.reset();

    auto cleanup = coordinator.acquire_cleanup(0ms);
    EXPECT_EQ(cleanup.result, cleanup_acquire_result_e::acquired);
    EXPECT_TRUE(cleanup.lease.has_value());
  }

  TEST(SessionLifecycleCoordinator, AbortedSuccessorCleanupIsTransferredExactlyOnce) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());

    const auto predecessor = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(predecessor.result, cleanup_acquire_result_e::superseded_by_launch);
    launch.reset();
    auto abort_cleanup = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(abort_cleanup.result, cleanup_acquire_result_e::acquired);
    EXPECT_TRUE(abort_cleanup.deferred_cleanup_pending);
    abort_cleanup.lease.reset();

    auto later = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(later.result, cleanup_acquire_result_e::acquired);
    EXPECT_FALSE(later.deferred_cleanup_pending);
  }

  TEST(SessionLifecycleCoordinator, ReleasedAbortTransfersDeferredCleanupToNextOwner) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());
    EXPECT_EQ(
      coordinator.acquire_cleanup(0ms).result,
      cleanup_acquire_result_e::superseded_by_launch
    );

    launch.reset();
    auto cleanup = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(cleanup.result, cleanup_acquire_result_e::acquired);
    EXPECT_TRUE(cleanup.deferred_cleanup_pending);
    cleanup.lease.reset();

    auto second = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(second.result, cleanup_acquire_result_e::acquired);
    EXPECT_FALSE(second.deferred_cleanup_pending);
  }

  TEST(SessionLifecycleCoordinator, RunningSuccessorAbsorbsDeferredCleanup) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());
    EXPECT_EQ(
      coordinator.acquire_cleanup(0ms).result,
      cleanup_acquire_result_e::superseded_by_launch
    );

    launch->mark_started();
    launch.reset();

    auto cleanup = coordinator.acquire_cleanup(0ms);
    ASSERT_EQ(cleanup.result, cleanup_acquire_result_e::acquired);
    EXPECT_FALSE(cleanup.deferred_cleanup_pending);
  }

  TEST(SessionLifecycleCoordinator, UncommittedLaunchImmediatelyDefersOldCleanup) {
    coordinator_t coordinator;
    auto launch = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(launch.has_value());

    const auto started = std::chrono::steady_clock::now();
    auto cleanup = coordinator.acquire_cleanup(2s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(cleanup.result, cleanup_acquire_result_e::superseded_by_launch);
    EXPECT_FALSE(cleanup.lease.has_value());
    // This models RTSP's single handler reaping a STOPPING session while the
    // HTTP launch thread prepares the successor. It must remain free to accept
    // ANNOUNCE instead of consuming the client's connection budget.
    EXPECT_LT(elapsed, 250ms);
  }

  TEST(SessionLifecycleCoordinator, DuplicateLaunchFailsWithoutConsumingRetryBudget) {
    coordinator_t coordinator;
    auto first = coordinator.acquire_launch(0ms);
    ASSERT_TRUE(first.has_value());

    const auto started = std::chrono::steady_clock::now();
    auto duplicate = coordinator.acquire_launch(2s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_FALSE(duplicate.has_value());
    EXPECT_LT(elapsed, 250ms);
  }

}  // namespace
