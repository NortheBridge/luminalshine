#include "src/pyrowave/contract.h"
#include "src/video_packet_qos.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

TEST(PyrowaveContract, AutomaticBitrateUses700MbpsWithoutChangingUnits) {
  EXPECT_EQ(pyrowave::bitrate_kbps(0), 700'000u);
  EXPECT_EQ(pyrowave::bitrate_kbps(700), 700'000u);
  EXPECT_EQ(pyrowave::bitrate_kbps(1), 1'000u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(700'000, 60), 1'458'333u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(700'000, 120), 729'166u);
}

TEST(PyrowaveContract, TenGbpsBudgetDoesNotOverflow32BitBitrateArithmetic) {
  const auto kbps = pyrowave::bitrate_kbps(10'000);
  EXPECT_EQ(kbps, 10'000'000u);
  EXPECT_EQ(kbps * 1'000u, 10'000'000'000ull);
  EXPECT_EQ(pyrowave::frame_budget_bytes(static_cast<int>(kbps), 60), 20'833'333u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(static_cast<int>(kbps), 1), 1'250'000'000u);
}

TEST(PyrowaveContract, InvalidInputsNeverProduceUsableBudgets) {
  for (int mbps : {-1, 10'001, std::numeric_limits<int>::max()}) {
    EXPECT_EQ(pyrowave::bitrate_kbps(mbps), 0u);
  }
  EXPECT_EQ(pyrowave::frame_budget_bytes(0, 60), 0u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(-1, 60), 0u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(700'000, 0), 0u);
  EXPECT_EQ(pyrowave::frame_budget_bytes(700'000, -1), 0u);
  EXPECT_EQ(pyrowave::max_frame_bytes(0), 0u);
  EXPECT_EQ(pyrowave::max_frame_bytes(-1), 0u);
}

TEST(PyrowaveContract, DefaultFits1080pAnd4kAt60And120Fps) {
  for (int fps : {60, 120}) {
    EXPECT_TRUE(pyrowave::valid_session(1920, 1080, fps, 700'000, 1392));
    EXPECT_TRUE(pyrowave::valid_session(3840, 2160, fps, 700'000, 1392));
  }
}

TEST(PyrowaveContract, RejectsModesThatCannotFitLegacyTransport) {
  EXPECT_FALSE(pyrowave::valid_session(3840, 2160, 60, 10'000'000, 1392));
  EXPECT_FALSE(pyrowave::valid_session(1920, 1080, 1, 700'000, 1392));
  EXPECT_GT(pyrowave::max_frame_bytes(1392), pyrowave::max_frame_bytes(1024));
  const auto max_bytes = pyrowave::max_frame_bytes(1392);
  EXPECT_LT(max_bytes, 1392ull * 4092);
  // With fps=125, each Kbps is exactly one frame byte: exercise the boundary
  // without round-off obscuring a one-byte overrun of the packetizer budget.
  EXPECT_TRUE(pyrowave::valid_session(1920, 1080, 125, static_cast<int>(max_bytes), 1392));
  EXPECT_FALSE(pyrowave::valid_session(1920, 1080, 125, static_cast<int>(max_bytes + 1), 1392));
}

TEST(PyrowaveContract, RejectsMalformedSessionDimensionsRatesAndPackets) {
  for (int width : {-1, 0, 126, 129, 8194}) {
    EXPECT_FALSE(pyrowave::valid_session(width, 1080, 60, 700'000, 1392));
  }
  for (int height : {-1, 0, 126, 129, 8194}) {
    EXPECT_FALSE(pyrowave::valid_session(1920, height, 60, 700'000, 1392));
  }
  for (int fps : {-1, 0, 241, std::numeric_limits<int>::max()}) {
    EXPECT_FALSE(pyrowave::valid_session(1920, 1080, fps, 700'000, 1392));
  }
  for (int kbps : {-1, 0, 10'000'001, std::numeric_limits<int>::max()}) {
    EXPECT_FALSE(pyrowave::valid_session(1920, 1080, 60, kbps, 1392));
  }
  for (int packet_size : {-1, 0, 1, 255, 2049}) {
    EXPECT_FALSE(pyrowave::valid_session(1920, 1080, 60, 700'000, packet_size));
  }
  EXPECT_TRUE(pyrowave::valid_session(128, 128, 240, 700'000, 1392));
  EXPECT_TRUE(pyrowave::valid_session(8192, 8192, 240, 700'000, 1392));
}

TEST(PyrowavePacing, ExplicitHighRateBudgetDrains700MbpsWithFec) {
  const auto legacy = stream::video_qos::pacing_budget(700'000, 20, 1408, 1376);
  EXPECT_EQ(legacy.drain_bitrate_bps, 800'000'000.0L);
  EXPECT_GT(legacy.average_wire_bitrate_bps, legacy.drain_bitrate_bps);

  const auto pyrowave = stream::video_qos::pacing_budget(700'000, 20, 1408, 1376, 15'000'000'000.0L);
  EXPECT_GT(pyrowave.drain_bitrate_bps, pyrowave.average_wire_bitrate_bps);
  EXPECT_EQ(pyrowave.drain_bitrate_bps, pyrowave.average_wire_bitrate_bps * stream::video_qos::kWirePacingHeadroom);
  EXPECT_LT(pyrowave.packet_interval, legacy.packet_interval);
}

TEST(PyrowavePacing, OrdinaryCodecBudgetRetainsLegacyDefault) {
  const auto ordinary = stream::video_qos::pacing_budget(65'388, 20, 1408, 1376);
  const auto explicit_legacy = stream::video_qos::pacing_budget(65'388, 20, 1408, 1376, 800'000'000.0L);
  EXPECT_EQ(ordinary.drain_bitrate_bps, explicit_legacy.drain_bitrate_bps);
  EXPECT_EQ(ordinary.packet_interval, explicit_legacy.packet_interval);
  EXPECT_EQ(ordinary.packets_per_quantum, explicit_legacy.packets_per_quantum);
}
