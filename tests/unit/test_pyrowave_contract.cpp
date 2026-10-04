#include "src/pyrowave/contract.h"
#include "src/video_packet_qos.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <thread>

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
  const auto hard_max_bytes = pyrowave::max_frame_bytes(1392);
  const auto max_bytes = pyrowave::max_target_bytes(1392);
  EXPECT_LT(hard_max_bytes, 1392ull * 4092);
  EXPECT_EQ(max_bytes, hard_max_bytes - hard_max_bytes / 4);
  // With fps=125, each Kbps is exactly one frame byte: exercise the boundary
  // without round-off obscuring a one-byte overrun of the packetizer budget.
  EXPECT_TRUE(pyrowave::valid_session(1920, 1080, 125, static_cast<int>(max_bytes), 1392));
  EXPECT_FALSE(pyrowave::valid_session(1920, 1080, 125, static_cast<int>(max_bytes + 1), 1392));
}

TEST(PyrowaveContract, ProfileBitsMatchPinnedClientAndRespect444Policy) {
  EXPECT_EQ(pyrowave::kServerSdr420, 0x00800000u);
  EXPECT_EQ(pyrowave::kServerSdr444, 0x01000000u);
  EXPECT_EQ(pyrowave::kServerHdr420, 0x02000000u);
  EXPECT_EQ(pyrowave::kServerHdr444, 0x04000000u);

  pyrowave::profile_mask_t profiles = 0;
  profiles |= pyrowave::profile_bit(pyrowave::profile_e::sdr420);
  profiles |= pyrowave::profile_bit(pyrowave::profile_e::sdr444);
  profiles |= pyrowave::profile_bit(pyrowave::profile_e::hdr420);
  profiles |= pyrowave::profile_bit(pyrowave::profile_e::hdr444);

  EXPECT_TRUE(pyrowave::profile_mask_supports(profiles, false, false));
  EXPECT_TRUE(pyrowave::profile_mask_supports(profiles, false, true));
  EXPECT_TRUE(pyrowave::profile_mask_supports(profiles, true, false));
  EXPECT_TRUE(pyrowave::profile_mask_supports(profiles, true, true));
  EXPECT_EQ(
    pyrowave::server_codec_mask_for_profiles(profiles, false),
    pyrowave::kServerSdr420 | pyrowave::kServerHdr420
  );
  EXPECT_EQ(
    pyrowave::server_codec_mask_for_profiles(profiles, true),
    pyrowave::kServerSdr420 | pyrowave::kServerSdr444 |
      pyrowave::kServerHdr420 | pyrowave::kServerHdr444
  );
}

TEST(PyrowaveContract, ServerMaskNeverInventsAnUnprobedProfile) {
  const auto profiles = pyrowave::profile_bit(pyrowave::profile_e::sdr420) |
                        pyrowave::profile_bit(pyrowave::profile_e::hdr444);
  EXPECT_EQ(pyrowave::server_codec_mask_for_profiles(profiles, false), pyrowave::kServerSdr420);
  EXPECT_EQ(
    pyrowave::server_codec_mask_for_profiles(profiles, true),
    pyrowave::kServerSdr420 | pyrowave::kServerHdr444
  );
  EXPECT_FALSE(pyrowave::profile_mask_supports(profiles, true, false));
  EXPECT_FALSE(pyrowave::profile_mask_supports(profiles, false, true));
}

TEST(PyrowaveContract, OptionalWireBooleansAreStrict) {
  ASSERT_EQ(pyrowave::parse_wire_bool("0"), std::optional<bool> {false});
  ASSERT_EQ(pyrowave::parse_wire_bool("1"), std::optional<bool> {true});
  for (const auto value : {"", "00", "01", "2", "-1", "+1", "true", "false", " 1", "1 "}) {
    EXPECT_FALSE(pyrowave::parse_wire_bool(value).has_value()) << value;
  }
}

TEST(PyrowaveContract, ParsesExactPackedBigEndianFrameFecStatus) {
  std::array<char, pyrowave::kFrameFecStatusWireSize> wire {};
  const auto put16 = [&](std::size_t offset, std::uint16_t value) {
    wire[offset] = static_cast<char>(value >> 8);
    wire[offset + 1] = static_cast<char>(value);
  };
  wire[0] = 0x01;
  wire[1] = 0x23;
  wire[2] = 0x45;
  wire[3] = 0x67;
  put16(4, 0x89AB);
  put16(6, 0x89A0);
  put16(8, 2);
  put16(10, 10);
  put16(12, 3);
  put16(14, 9);
  put16(16, 2);
  wire[18] = 20;
  wire[19] = 1;
  wire[20] = 2;

  const auto parsed = pyrowave::parse_frame_fec_status(
    std::string_view {wire.data(), wire.size()});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->frame_index, 0x01234567u);
  EXPECT_EQ(parsed->highest_received_sequence, 0x89ABu);
  EXPECT_EQ(parsed->next_contiguous_sequence, 0x89A0u);
  EXPECT_EQ(parsed->missing_before_highest, 2);
  EXPECT_EQ(parsed->total_data_packets, 10);
  EXPECT_EQ(parsed->total_parity_packets, 3);
  EXPECT_EQ(parsed->received_data_packets, 9);
  EXPECT_EQ(parsed->received_parity_packets, 2);
  EXPECT_EQ(parsed->fec_percentage, 20);
  EXPECT_EQ(parsed->block_index, 1);
  EXPECT_EQ(parsed->block_count, 2);
  EXPECT_TRUE(parsed->has_loss());
  EXPECT_FALSE(parsed->unrecoverable());
}

TEST(PyrowaveContract, RejectsMalformedFrameFecStatusAndClassifiesUnrecoverableLoss) {
  std::array<char, pyrowave::kFrameFecStatusWireSize> wire {};
  const auto put16 = [&](std::size_t offset, std::uint16_t value) {
    wire[offset] = static_cast<char>(value >> 8);
    wire[offset + 1] = static_cast<char>(value);
  };
  put16(10, 10);
  put16(12, 4);
  put16(14, 5);
  put16(16, 3);
  wire[20] = 1;
  const auto payload = [&]() { return std::string_view {wire.data(), wire.size()}; };

  auto parsed = pyrowave::parse_frame_fec_status(payload());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->has_loss());
  EXPECT_TRUE(parsed->unrecoverable());

  pyrowave::frame_fec_status_t clean {
    .total_data_packets = 10,
    .total_parity_packets = 4,
    .received_data_packets = 10,
    .received_parity_packets = 4,
    .block_count = 1,
  };
  EXPECT_FALSE(clean.has_loss());
  EXPECT_FALSE(clean.unrecoverable());
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload().substr(0, wire.size() - 1)));

  put16(14, 11);  // received data exceeds the sender's total
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
  put16(14, 10);
  put16(16, 5);  // received parity exceeds the sender's total
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
  put16(16, 4);
  put16(10, 0);  // a FEC block must contain data
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
  put16(10, 10);
  wire[20] = 0;
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
  wire[20] = 5;
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
  wire[20] = 2;
  wire[19] = 2;
  EXPECT_FALSE(pyrowave::parse_frame_fec_status(payload()));
}

TEST(PyrowaveContract, AdaptiveBudgetIsEffectiveBoundedAndOverflowSafe) {
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(1'000, 1.0, 1.0, 10'000), 1'000u);
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(1'000, 0.5, 1.0, 10'000), 500u);
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(1'000, 1.0, 2.0, 10'000), 2'000u);
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(1'000, 0.5, 2.0, 10'000), 1'000u);
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(10'000, 1.0, 2.0, 12'345), 12'345u);
  EXPECT_EQ(
    pyrowave::adaptive_frame_budget_bytes(std::numeric_limits<std::uint64_t>::max(), 1.0, 2.0, 7'654'321),
    7'654'321u
  );
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(1'000, 1.0, 1.0, 0), 0u);
  EXPECT_EQ(pyrowave::adaptive_pacing_bitrate_kbps(700'000, 1.0), 700'000);
  EXPECT_EQ(pyrowave::adaptive_pacing_bitrate_kbps(700'000, 0.8), 560'000);
  EXPECT_EQ(pyrowave::adaptive_pacing_bitrate_kbps(700'000, 0.1), 350'000);
}

TEST(PyrowaveContract, AdaptiveFecBoostIsSessionLocalBoundedAndExpires) {
  int a = 0;
  int b = 0;
  a = pyrowave::next_adaptive_fec_boost(a);
  a = pyrowave::next_adaptive_fec_boost(a);
  EXPECT_EQ(a, 8);
  EXPECT_EQ(b, 0);
  EXPECT_EQ(pyrowave::effective_fec_percentage(10, a, 0), 18);
  EXPECT_EQ(pyrowave::effective_fec_percentage(10, a, 9'999), 18);
  EXPECT_EQ(pyrowave::effective_fec_percentage(10, a, 10'000), 10);
  for (int i = 0; i < 10; ++i) b = pyrowave::next_adaptive_fec_boost(b);
  EXPECT_EQ(b, 16);
  EXPECT_EQ(pyrowave::next_adaptive_fec_boost(std::numeric_limits<int>::max()), 16);
  EXPECT_EQ(pyrowave::effective_fec_percentage(20, b, 0), 25);
  // A host base above the adaptive ceiling is never reduced.
  EXPECT_EQ(pyrowave::effective_fec_percentage(30, b, 0), 30);
  EXPECT_EQ(pyrowave::effective_fec_percentage(std::numeric_limits<int>::max(), b, 0), std::numeric_limits<int>::max());
  EXPECT_EQ(pyrowave::effective_fec_percentage(20, 0, 0), 20);
}

TEST(PyrowaveContract, AdaptiveFecPreservesNegotiatedWireBudget) {
  EXPECT_EQ(pyrowave::fec_video_scale_percent(10, 10), 100);
  EXPECT_EQ(pyrowave::fec_video_scale_percent(10, 14), 96);
  EXPECT_EQ(pyrowave::fec_video_scale_percent(10, 25), 88);
  EXPECT_EQ(pyrowave::fec_video_scale_percent(0, 25), 80);
  EXPECT_EQ(pyrowave::fec_video_scale_percent(25, 10), 100);
  EXPECT_EQ(pyrowave::fec_video_scale_percent(-1, 25), 80);
  EXPECT_GE(pyrowave::fec_video_scale_percent(0, std::numeric_limits<int>::max()), 1);
  EXPECT_EQ(pyrowave::apply_video_scale_percent(700'000, 80), 560'000);
  EXPECT_EQ(pyrowave::adaptive_pacing_bitrate_kbps(
              pyrowave::apply_video_scale_percent(700'000, 80), 0.5),
            280'000);
  EXPECT_EQ(pyrowave::adaptive_frame_budget_bytes(
              pyrowave::apply_video_scale_percent(std::uint64_t {1'000}, 80), 0.5, 1.0, 10'000),
            400u);
  EXPECT_EQ(pyrowave::apply_video_scale_percent(std::numeric_limits<std::uint64_t>::max(), 100),
            std::numeric_limits<std::uint64_t>::max());

  const auto base_video = 700'000;
  const auto boosted_fec = 25;
  const auto scaled_video = pyrowave::apply_video_scale_percent(
    base_video, pyrowave::fec_video_scale_percent(10, boosted_fec));
  EXPECT_LE(std::int64_t {scaled_video} * (100 + boosted_fec),
            std::int64_t {base_video} * (100 + 10));
}

TEST(PyrowaveContract, FreshLossCannotBeErasedByConcurrentExpiry) {
  pyrowave::adaptive_fec_controller_t controller;
  for (int iteration = 0; iteration < 100; ++iteration) {
    controller.set_base(10);
    (void) controller.report_loss(0);
    std::atomic_bool start {false};
    std::thread expiry([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) controller.sample(10'000);
    });
    std::thread fresh_loss([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) controller.report_loss(10'000);
    });
    start.store(true, std::memory_order_release);
    expiry.join();
    fresh_loss.join();

    const auto final = controller.sample(10'000);
    EXPECT_GT(final.fec_percentage, 10);
    EXPECT_LT(final.video_scale_percent, 100);
  }
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
