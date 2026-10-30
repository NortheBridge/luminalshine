#include "src/pyrowave_bitrate_policy.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

namespace {
  constexpr std::int64_t kHost = 700'000;
  constexpr std::int64_t kFloor = pyrowave::kMinClientBitrateKbps;
}  // namespace

TEST(PyrowaveBitratePolicy, ClientBelowHostTargetLowersTheTarget) {
  EXPECT_EQ(pyrowave::session_bitrate_kbps(150'000, kHost, kFloor), 150'000);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(20'000, kHost, kFloor), 20'000);
}

TEST(PyrowaveBitratePolicy, HostTargetRemainsTheCeiling) {
  EXPECT_EQ(pyrowave::session_bitrate_kbps(700'000, kHost, kFloor), kHost);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(1'000'000, kHost, kFloor), kHost);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(10'000'000, 10'000'000, kFloor), 10'000'000);
}

TEST(PyrowaveBitratePolicy, MissingOrNonPositiveClientValueKeepsHostTarget) {
  EXPECT_EQ(pyrowave::session_bitrate_kbps(0, kHost, kFloor), kHost);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(-1, kHost, kFloor), kHost);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(std::numeric_limits<std::int64_t>::min(), kHost, kFloor), kHost);
}

TEST(PyrowaveBitratePolicy, TinyClientRequestIsRaisedToTheFloorNotTheHostTarget) {
  EXPECT_EQ(kFloor, 1'000);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(1, kHost, kFloor), kFloor);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(288, kHost, kFloor), kFloor);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(kFloor, kHost, kFloor), kFloor);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(kFloor + 1, kHost, kFloor), kFloor + 1);
}

TEST(PyrowaveBitratePolicy, FloorNeverLiftsTheTargetAboveTheHostCeiling) {
  EXPECT_EQ(pyrowave::session_bitrate_kbps(1, 500, kFloor), 500);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(0, 500, kFloor), 500);
}

TEST(PyrowaveBitratePolicy, InvalidHostTargetStaysInvalidSoSetupIsRejected) {
  // pyrowave::bitrate_kbps() reports an out-of-range host setting as 0, which
  // valid_session() rejects. A client value must not turn that into a session.
  EXPECT_EQ(pyrowave::session_bitrate_kbps(150'000, 0, kFloor), 0);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(0, 0, kFloor), 0);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(150'000, -5, kFloor), 0);
}

TEST(PyrowaveBitratePolicy, ExtremeValuesDoNotOverflowAndFitAnInt) {
  constexpr auto max64 = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(pyrowave::session_bitrate_kbps(max64, kHost, kFloor), kHost);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(max64, max64, kFloor), max64);
  EXPECT_EQ(pyrowave::session_bitrate_kbps(150'000, kHost, max64), kHost);
  // Larger than INT_MAX: a client value that a 32-bit cast would wrap still caps correctly.
  EXPECT_EQ(pyrowave::session_bitrate_kbps(5'000'000'000, kHost, kFloor), kHost);
  static_assert(pyrowave::session_bitrate_kbps(150'000, kHost, kFloor) == 150'000);
}
