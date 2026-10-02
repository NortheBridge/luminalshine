/**
 * @file tests/unit/test_exact_device_window_router_policy.cpp
 * @brief Regression coverage for exact-device window routing policy.
 */
#include "src/platform/windows/exact_device_window_router_policy.h"

#include <array>
#include <gtest/gtest.h>

namespace {
  using namespace platf::window_router_policy;

  TEST(ExactDeviceWindowRouter, EmptyOrUnknownTargetNeverFallsBack) {
    const std::array devices {
      device_identity_t {"physical-primary", R"(\\.\DISPLAY1)", true, false},
      device_identity_t {"vgd-client-a", R"(\\.\DISPLAY7)", true, true},
    };

    EXPECT_FALSE(select_exact_active_virtual_display(devices, ""));
    EXPECT_FALSE(select_exact_active_virtual_display(devices, "vgd-client-b"));
    EXPECT_FALSE(select_exact_active_virtual_display(devices, "physical-primary"));
  }

  TEST(ExactDeviceWindowRouter, SelectsExactVgdEvenWhenPhysicalDisplayIsPrimary) {
    const std::array devices {
      device_identity_t {"physical-primary", R"(\\.\DISPLAY1)", true, false},
      device_identity_t {"VGD-CLIENT-A", R"(\\.\DISPLAY7)", true, true},
      device_identity_t {"vgd-client-b", R"(\\.\DISPLAY8)", true, true},
    };

    EXPECT_EQ(select_exact_active_virtual_display(devices, "vgd-client-a"), R"(\\.\DISPLAY7)");
  }

  TEST(ExactDeviceWindowRouter, InactiveExactVgdIsNotAValidRoutingTarget) {
    const std::array devices {
      device_identity_t {"vgd-client-a", "", false, true},
    };
    EXPECT_FALSE(select_exact_active_virtual_display(devices, "vgd-client-a"));
  }

  TEST(ExactDeviceWindowRouter, AnchoredExclusiveUsesItsMarkerAndExactIdentity) {
    EXPECT_TRUE(should_arm_exact_router(true, "VGD-CLIENT-A", "vgd-client-a"));
    EXPECT_FALSE(should_arm_exact_router(false, "vgd-client-a", "vgd-client-a"));
    EXPECT_FALSE(should_arm_exact_router(true, "", "vgd-client-a"));
    EXPECT_FALSE(should_arm_exact_router(true, "vgd-client-a", ""));
    EXPECT_FALSE(should_arm_exact_router(true, "vgd-client-a", "vgd-client-b"));
  }

  TEST(ExactDeviceWindowRouter, RoutingGeometryPreservesOffsetAndClampsToTarget) {
    const rect_t source {0, 0, 1920, 1080};
    const rect_t target {10000, -1000, 13840, 1160};
    EXPECT_EQ(route_rect(source, target, {100, 80, 1100, 780}, false), (rect_t {10100, -920, 11100, -220}));
    EXPECT_EQ(route_rect(source, target, {3800, 1800, 4600, 2400}, false), (rect_t {13040, 560, 13840, 1160}));
    EXPECT_EQ(route_rect(source, target, {0, 0, 1920, 1080}, true), target);
  }

  TEST(ExactDeviceWindowRouter, ExcludesShellAndNonApplicationSurfaces) {
    window_traits_t normal {true, false, true, false, false, false, false};
    EXPECT_TRUE(should_route_window(normal));
    normal.shell_surface = true;
    EXPECT_FALSE(should_route_window(normal));
    normal.shell_surface = false;
    normal.tool_window = true;
    EXPECT_FALSE(should_route_window(normal));
    normal.tool_window = false;
    normal.no_activate = true;
    EXPECT_FALSE(should_route_window(normal));
  }

  TEST(ExactDeviceWindowRouter, CoalescesEventsAndRejectsRetiredGenerations) {
    pending_window_set_t pending {2};
    const routing_token_t current {7, 3};
    EXPECT_TRUE(pending.enqueue(10, current));
    EXPECT_FALSE(pending.enqueue(10, current));
    EXPECT_TRUE(pending.enqueue(11, current));
    EXPECT_FALSE(pending.enqueue(12, current));
    EXPECT_FALSE(pending.take_if_current(10, {8, 3}));
    EXPECT_TRUE(pending.take_if_current(11, current));

    EXPECT_TRUE(pending.enqueue(12, {8, 3}));
    pending.clear();
    EXPECT_FALSE(pending.take_if_current(12, {8, 3}));
  }

  TEST(ExactDeviceWindowRouter, RejectsRetiredTopologyTokens) {
    EXPECT_TRUE(token_is_current({7, 3}, {7, 3}));
    EXPECT_FALSE(token_is_current({7, 2}, {7, 3}));
    EXPECT_FALSE(token_is_current({6, 3}, {7, 3}));
    EXPECT_FALSE(token_is_current({}, {7, 3}));
  }

  TEST(ExactDeviceWindowRouter, DelayedReapplyRequiresTheSameLiveSession) {
    EXPECT_TRUE(may_run_delayed_reapply(7, 7, 3, 3, false, false));
    EXPECT_FALSE(may_run_delayed_reapply(7, 8, 3, 3, false, false));
    EXPECT_FALSE(may_run_delayed_reapply(7, 7, 3, 4, false, false));
    EXPECT_FALSE(may_run_delayed_reapply(7, 7, 3, 3, true, false));
    EXPECT_FALSE(may_run_delayed_reapply(7, 7, 3, 3, false, true));
  }
}  // namespace
