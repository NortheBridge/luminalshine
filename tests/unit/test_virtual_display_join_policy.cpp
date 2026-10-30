/**
 * @file tests/unit/test_virtual_display_join_policy.cpp
 * @brief Unit tests for the policy that keeps a joining client from changing the owner's virtual display.
 */

#include "src/virtual_display_join_policy.h"

#include <gtest/gtest.h>

namespace {

  using virtual_display_join::may_change_output_override;
  using virtual_display_join::may_reconcile_mode;
  using virtual_display_join::may_supersede_fallback_monitors;

  TEST(VirtualDisplayJoinPolicy, JoinerDoesNotSupersedeOwnersFallbackMonitor) {
    // Another session is streaming: a fallback monitor armed by its launch is live
    // state, so the joining launch/resume must leave it alone.
    EXPECT_FALSE(may_supersede_fallback_monitors(false));
  }

  TEST(VirtualDisplayJoinPolicy, FreshLaunchAndIdleResumeSupersedeStaleMonitors) {
    // No session is active: a fresh launch and an idle /resume (reused display,
    // revert-on-disconnect off) own the virtual-display state and must cancel
    // any monitor armed by an earlier failed launch.
    EXPECT_TRUE(may_supersede_fallback_monitors(true));
  }

  TEST(VirtualDisplayJoinPolicy, JoinerNeverRollsBackSharedOutputOverride) {
    // The runtime output override already points at the owner's shared display.
    // A joiner that rewrote it (or recorded it as pending) would clear it on a
    // failed /resume and strand the owner's capture.
    EXPECT_FALSE(may_change_output_override(true));
    EXPECT_TRUE(may_change_output_override(false));
  }

  TEST(VirtualDisplayJoinPolicy, JoinerSkipsModeReconcile) {
    // The joiner's launch has no display guid and the display belongs to the
    // owner, so an HTTP/RTSP mode mismatch must not remove and recreate it.
    EXPECT_FALSE(may_reconcile_mode(true));
    EXPECT_TRUE(may_reconcile_mode(false));
  }

}  // namespace
