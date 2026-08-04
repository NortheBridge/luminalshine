#include <gtest/gtest.h>

#include "src/requested_display_mode.h"

namespace {
  using display_mode_policy::Mode;
  using display_mode_policy::should_reconcile;

  TEST(RequestedDisplayMode, RtspCorrectsAStaleHttpModeForVgd) {
    EXPECT_TRUE(should_reconcile(true, false, Mode {1280, 720, 60}, Mode {3840, 2160, 120}));
  }

  TEST(RequestedDisplayMode, ExplicitPerClientOverrideRemainsAuthoritative) {
    EXPECT_FALSE(should_reconcile(true, true, Mode {1280, 720, 60}, Mode {3840, 2160, 120}));
  }

  TEST(RequestedDisplayMode, MatchingModesDoNotCycleTheMonitor) {
    EXPECT_FALSE(should_reconcile(true, false, Mode {3840, 2160, 120}, Mode {3840, 2160, 120}));
  }

  TEST(RequestedDisplayMode, InvalidLateModeCannotReplaceAValidLaunchMode) {
    EXPECT_FALSE(should_reconcile(true, false, Mode {3840, 2160, 120}, Mode {}));
  }
}
