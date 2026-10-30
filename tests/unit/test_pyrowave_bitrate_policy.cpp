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

TEST(PyrowaveBitratePolicy, ConfiguredBitrateUsesTheAdjustedVideoValue) {
  // 150 Mbps selected, adjusted for 20% FEC, HQ stereo audio and overhead.
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(150'000, 118'988, 200'000), 118'988);
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(150'000, 118'988, 0), 118'988);
}

TEST(PyrowaveBitratePolicy, PositiveRequestAdjustedToNothingSelectsTheFloor) {
  // configuredBitrateKbps=1 adjusts to 0 with 20% FEC; it is still a request.
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(1, 0, 200'000), kFloor);
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(2, -3, 0), kFloor);
  EXPECT_EQ(
    pyrowave::session_bitrate_kbps(pyrowave::client_video_bitrate_kbps(1, 0, 200'000), kHost, kFloor),
    kFloor
  );
}

TEST(PyrowaveBitratePolicy, MissingConfiguredBitrateFallsBackToMaximumBitrate) {
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(0, 0, 80'000), 80'000);
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(-5, -5, 80'000), 80'000);
  // A maximum beyond INT_MAX is not wrapped before the cap.
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(0, 0, 5'000'000'000), 5'000'000'000);
}

TEST(PyrowaveBitratePolicy, NoPositiveClientFieldMeansNoClientValue) {
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(0, 0, 0), 0);
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(-1, -1, -1), 0);
  EXPECT_EQ(pyrowave::client_video_bitrate_kbps(0, 0, std::numeric_limits<std::int64_t>::min()), 0);
  EXPECT_EQ(
    pyrowave::session_bitrate_kbps(pyrowave::client_video_bitrate_kbps(0, 0, 0), kHost, kFloor),
    kHost
  );
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
