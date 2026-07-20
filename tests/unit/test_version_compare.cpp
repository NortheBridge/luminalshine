/**
 * @file tests/unit/test_version_compare.cpp
 * @brief Unit tests for LuminalShine's semver comparison rules.
 */

#include "../tests_common.h"

#include <src/version_compare.h>

TEST(VersionCompareTest, StableRespinsSortAbovePlainRelease) {
  EXPECT_LT(version_compare::compare_semver("1.14.14", "1.14.14-stable.1"), 0);
  EXPECT_GT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14"), 0);
}

TEST(VersionCompareTest, StandardPrereleasesStayBelowRelease) {
  EXPECT_LT(version_compare::compare_semver("1.14.14-alpha.1", "1.14.14"), 0);
  EXPECT_LT(version_compare::compare_semver("1.14.14-beta.1", "1.14.14"), 0);
  EXPECT_LT(version_compare::compare_semver("1.14.14-rc.1", "1.14.14"), 0);
}

TEST(VersionCompareTest, StableRespinsStillCompareWithinTheirChannel) {
  EXPECT_LT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14-stable.2"), 0);
  EXPECT_GT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14-rc.9"), 0);
  EXPECT_EQ(version_compare::compare_semver("1.14.14+build.1", "1.14.14+build.2"), 0);
}

TEST(VersionCompareTest, HotfixSuffixedPrereleasesRankByLeadingNumber) {
  // "beta.1shfx01" is a hotfix on beta.1: above its base, below beta.2.
  EXPECT_LT(version_compare::compare_semver("26.07.1-beta.1shfx01", "26.07.1-beta.2"), 0);
  EXPECT_GT(version_compare::compare_semver("26.07.1-beta.2", "26.07.1-beta.1shfx01"), 0);
  EXPECT_GT(version_compare::compare_semver("26.07.1-beta.1shfx01", "26.07.1-beta.1"), 0);
  EXPECT_LT(version_compare::compare_semver("26.07.1-beta.1shfx01", "26.07.1-beta.1shfx02"), 0);
  EXPECT_EQ(version_compare::compare_semver("26.07.1-beta.1shfx01", "26.07.1-beta.1shfx01"), 0);
}

TEST(VersionCompareTest, NonNumericAlphanumericsKeepStrictSemverOrder) {
  // No leading digits: strict semver still ranks numeric below alphanumeric.
  EXPECT_LT(version_compare::compare_semver("1.0.0-beta.1", "1.0.0-beta.rc"), 0);
  EXPECT_LT(version_compare::compare_semver("1.0.0-alpha.1", "1.0.0-beta.1"), 0);
}
