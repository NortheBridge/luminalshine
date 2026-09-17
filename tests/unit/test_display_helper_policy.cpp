/**
 * @file tests/unit/test_display_helper_policy.cpp
 * @brief Tests for the pure display-helper integration decisions.
 */
#include "src/platform/windows/display_helper_policy.h"

#include "../tests_common.h"

using display_helper_integration::duplicate_revert_suppressed;
using display_helper_integration::kDuplicateRevertWindowUs;

TEST(DisplayHelperPolicy, SecondRevertRightAfterAVerifiedRestoreIsADuplicate) {
  // The app-exit path reverts ~0.4 s after the session-end cleanup's restore was
  // verified and its helper exited: nothing is left to restore.
  EXPECT_TRUE(duplicate_revert_suppressed(false, 10'000'000, 10'400'000, false));
}

TEST(DisplayHelperPolicy, GoldenFallbackRequestsAreNeverSuppressed) {
  EXPECT_FALSE(duplicate_revert_suppressed(true, 10'000'000, 10'400'000, false));
}

TEST(DisplayHelperPolicy, ALiveHelperMeansARestoreMayStillBeOwed) {
  // With a helper alive the completed restore may not be the latest word; only a
  // helper-less completion proves the session is fully restored.
  EXPECT_FALSE(duplicate_revert_suppressed(false, 10'000'000, 10'400'000, true));
}

TEST(DisplayHelperPolicy, OnlyARecentCompletionCounts) {
  EXPECT_FALSE(duplicate_revert_suppressed(false, 0, 10'400'000, false));
  EXPECT_FALSE(duplicate_revert_suppressed(false, 10'000'000, 10'000'000 + kDuplicateRevertWindowUs, false));
  EXPECT_TRUE(duplicate_revert_suppressed(false, 10'000'000, 10'000'000 + kDuplicateRevertWindowUs - 1, false));
}
