#include <gtest/gtest.h>
#include <src/platform/windows/vhf_gamepad_maintenance.h>

using platf::vhf_gamepad::ds4_maintenance_t;
using namespace std::chrono_literals;

TEST(VhfDs4Maintenance, MaintainsReportCadenceWithoutDuplicatingFreshState) {
  ds4_maintenance_t state;
  const auto start = ds4_maintenance_t::clock::time_point {};
  state.reset(start);
  EXPECT_FALSE(state.poll(start + 99ms).keepalive);
  EXPECT_TRUE(state.poll(start + 100ms).keepalive);
  EXPECT_FALSE(state.poll(start + 101ms).keepalive);
  state.report_sent(start + 150ms);
  EXPECT_FALSE(state.poll(start + 249ms).keepalive);
  EXPECT_TRUE(state.poll(start + 250ms).keepalive);
}

TEST(VhfDs4Maintenance, ClearsStoppedGyroOnceAndRestartsOnFreshMotion) {
  ds4_maintenance_t state;
  const auto start = ds4_maintenance_t::clock::time_point {};
  state.reset(start);
  EXPECT_FALSE(state.poll(start + 750ms).clear_gyro);
  EXPECT_TRUE(state.poll(start + 751ms).clear_gyro);
  EXPECT_FALSE(state.poll(start + 800ms).clear_gyro);
  state.gyro_received(start + 1s);
  EXPECT_FALSE(state.poll(start + 1750ms).clear_gyro);
  EXPECT_TRUE(state.poll(start + 1751ms).clear_gyro);
}

TEST(VhfDs4Maintenance, RetriesMotionRequestsWithFiveSecondCooldown) {
  ds4_maintenance_t state;
  const auto start = ds4_maintenance_t::clock::time_point {};
  state.reset(start);
  EXPECT_FALSE(state.poll(start + 5s).request_motion);
  EXPECT_TRUE(state.poll(start + 5001ms).request_motion);
  EXPECT_FALSE(state.poll(start + 6s).request_motion);
  EXPECT_TRUE(state.poll(start + 10002ms).request_motion);
  state.gyro_received(start + 15s);
  EXPECT_FALSE(state.poll(start + 16s).request_motion);
}

TEST(VhfDs4Maintenance, RecoveryResetsTheOldDeviceTimers) {
  ds4_maintenance_t state;
  const auto start = ds4_maintenance_t::clock::time_point {};
  state.reset(start);
  EXPECT_TRUE(state.poll(start + 6s).request_motion);
  state.reset(start + 7s);
  const auto actions = state.poll(start + 7050ms);
  EXPECT_FALSE(actions.keepalive);
  EXPECT_FALSE(actions.clear_gyro);
  EXPECT_FALSE(actions.request_motion);
}
