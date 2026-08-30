/**
 * @file tests/unit/test_coverage_probe.cpp
 * @brief Unit tests for util::clamp_fps.
 */

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/utility.h"

namespace {
  TEST(ClampFps, RequestInsideRangeIsUnchanged) {
    EXPECT_EQ(60, util::clamp_fps(60, 30, 120));
  }

  TEST(ClampFps, RequestBelowMinimumClampsUp) {
    EXPECT_EQ(30, util::clamp_fps(10, 30, 120));
  }

  TEST(ClampFps, RequestAboveMaximumClampsDown) {
    EXPECT_EQ(120, util::clamp_fps(240, 30, 120));
  }

  TEST(ClampFps, InvertedBoundsFallBackToMinimum) {
    EXPECT_EQ(30, util::clamp_fps(60, 30, 10));
  }

  TEST(ClampFps, BoundsAreInclusive) {
    EXPECT_EQ(30, util::clamp_fps(30, 30, 120));
    EXPECT_EQ(120, util::clamp_fps(120, 30, 120));
  }
}  // namespace
