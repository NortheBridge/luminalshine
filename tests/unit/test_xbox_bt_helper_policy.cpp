#include <gtest/gtest.h>

#include <vector>

#include "tools/xbox_bt_helper_policy.h"

TEST(XboxBluetoothHelper, RequiresAnActualBluetoothAncestor) {
  using xbox_bt_helper::is_bluetooth_instance;
  EXPECT_TRUE(is_bluetooth_instance(L"BTHENUM\\DEV_123"));
  EXPECT_TRUE(is_bluetooth_instance(L"BTHLEDEVICE\\DEV_123"));
  EXPECT_FALSE(is_bluetooth_instance(L"HID\\VID_045E&PID_0B13"));
  EXPECT_FALSE(is_bluetooth_instance(L"ROOT\\VIBESHINEVIRTUALGAMEPAD"));
  EXPECT_FALSE(is_bluetooth_instance(L"BTHENUM_OTHER\\DEV_123"));
}

TEST(XboxBluetoothHelper, BoundsAllocationHoldTime) {
  EXPECT_EQ(xbox_bt_helper::bounded_hold_ms(0), 1u);
  EXPECT_EQ(xbox_bt_helper::bounded_hold_ms(250), 250u);
  EXPECT_EQ(xbox_bt_helper::bounded_hold_ms(100000), 1000u);
}

TEST(XboxBluetoothHelper, ClaimsHighestFreeSlotWithoutReusingBusySlots) {
  std::vector<unsigned> attempts;
  const auto slot = xbox_bt_helper::claim_free_slot(32, 16, [&](unsigned id) {
    attempts.push_back(id);
    return id == 13;
  });
  ASSERT_TRUE(slot);
  EXPECT_EQ(*slot, 13u);
  EXPECT_EQ(attempts, (std::vector<unsigned> {15, 14, 13}));
}

TEST(XboxBluetoothHelper, SkipsNudgeWhenDriverIsFullOrHasNoSlots) {
  std::vector<unsigned> attempts;
  EXPECT_FALSE(xbox_bt_helper::claim_free_slot(2, 16, [&](unsigned id) {
    attempts.push_back(id);
    return false;
  }));
  EXPECT_EQ(attempts, (std::vector<unsigned> {1, 0}));
  EXPECT_FALSE(xbox_bt_helper::claim_free_slot(0, 16, [](unsigned) {
    ADD_FAILURE() << "No allocation should be attempted";
    return true;
  }));
}
