/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <src/video.h>
#include <string_view>
#ifdef _WIN32
  #include "src/platform/windows/misc.h"
#endif

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    const char *run_encoders = std::getenv("SUNSHINE_RUN_ENCODER_TESTS");
    if (!run_encoders || std::string_view(run_encoders).empty() || std::string_view(run_encoders) == "0") {
      GTEST_SKIP() << "Encoder tests disabled (set SUNSHINE_RUN_ENCODER_TESTS=1 to enable)";
    }

    if (encoder.name != "software") {
      const char *run_hardware = std::getenv("SUNSHINE_RUN_HARDWARE_ENCODER_TESTS");
      if (!run_hardware || std::string_view(run_hardware).empty() || std::string_view(run_hardware) == "0") {
        GTEST_SKIP() << "Hardware encoder tests disabled (set SUNSHINE_RUN_HARDWARE_ENCODER_TESTS=1 to enable)";
      }
    }

#ifdef _WIN32
    if (encoder.name == "nvenc" && !platf::has_nvidia_gpu()) {
      GTEST_SKIP() << "NVIDIA GPU not detected";
    }
#endif
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#if defined(__linux__) || defined(__FreeBSD__)
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

struct FramerateX100Test: testing::TestWithParam<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

namespace {
  // The parent exports its probe result and an isolated video worker imports
  // it. A codec that failed probing must carry no capability bits, or the
  // import sees a 4:4:4/HDR bit on an unsupported codec and refuses every
  // session, whatever codec the client asked for (issue #209).
  struct EncoderSnapshotTest: testing::Test {
    std::array<std::bitset<video::encoder_t::MAX_FLAGS>, 3> saved_capabilities {
      video::software.h264.capabilities,
      video::software.hevc.capabilities,
      video::software.av1.capabilities,
    };
    int saved_hevc_mode = video::active_hevc_mode;
    int saved_av1_mode = video::active_av1_mode;

    void TearDown() override {
      video::software.h264.capabilities = saved_capabilities[0];
      video::software.hevc.capabilities = saved_capabilities[1];
      video::software.av1.capabilities = saved_capabilities[2];
      video::active_hevc_mode = saved_hevc_mode;
      video::active_av1_mode = saved_av1_mode;
    }

    // H.264 and HEVC passed with 4:4:4; AV1 failed its validation.
    static video::encoder_probe_snapshot_t av1_failed(std::uint32_t av1_capabilities) {
      constexpr auto passed = std::uint32_t {1} << video::encoder_t::PASSED;
      constexpr auto yuv444 = std::uint32_t {1} << video::encoder_t::YUV444;
      video::encoder_probe_snapshot_t snapshot {};
      const std::string_view name = video::software.name;
      std::copy(name.begin(), name.end(), snapshot.encoder_name.begin());
      snapshot.active_hevc_mode = 2;
      snapshot.active_av1_mode = 1;
      snapshot.codec_capabilities = {passed | yuv444, passed | yuv444, av1_capabilities};
      snapshot.yuv444_for_codec = {1, 1, 0};
      snapshot.supported_codec = {1, 1, 0};
      return snapshot;
    }
  };
}  // namespace

TEST_F(EncoderSnapshotTest, RejectsStaleBitsOnAFailedCodec) {
  constexpr auto all_but_passed = ((std::uint32_t {1} << video::encoder_t::MAX_FLAGS) - 1) &
                                  ~(std::uint32_t {1} << video::encoder_t::PASSED);
  EXPECT_FALSE(video::import_encoder_probe_snapshot(av1_failed(all_but_passed)));
}

TEST_F(EncoderSnapshotTest, AFailedCodecWithNoBitsRoundTrips) {
  const auto sent = av1_failed(0);
  ASSERT_TRUE(video::import_encoder_probe_snapshot(sent));

  video::encoder_probe_snapshot_t received {};
  ASSERT_TRUE(video::export_encoder_probe_snapshot(received));
  EXPECT_EQ(sent.encoder_name, received.encoder_name);
  EXPECT_EQ(sent.codec_capabilities, received.codec_capabilities);
  EXPECT_EQ(sent.yuv444_for_codec, received.yuv444_for_codec);
  EXPECT_EQ(sent.supported_codec, received.supported_codec);
  EXPECT_EQ(sent.active_av1_mode, received.active_av1_mode);
}

namespace {
  // A stop that arrives while the host is still probing encoders must end the
  // probe at its next check instead of finishing the codec matrix (about a
  // second per GPU encode session). The probe sets every capability bit before
  // it validates and only corrects downward, so an abandoned pass has to put
  // back what the bits held, not leave them claiming support. Each test runs on
  // a fresh mailbox with the process-wide shutdown event already raised.
  struct EncoderProbeStopTest: testing::Test {
    safe::mail_t saved_mail = mail::man;
    // The mailbox holds its events by weak_ptr, so an event nobody owns is
    // recreated, unraised, on the next lookup. main() keeps its shutdown_event
    // local alive for the same reason; the test has to keep this one alive too.
    safe::mail_raw_t::event_t<bool> shutdown_event;
    std::array<std::bitset<video::encoder_t::MAX_FLAGS>, 3> saved_capabilities {
      video::software.h264.capabilities,
      video::software.hevc.capabilities,
      video::software.av1.capabilities,
    };
    int saved_hevc_mode = video::active_hevc_mode;
    int saved_av1_mode = video::active_av1_mode;

    void SetUp() override {
      // Neither all clear nor all set, so both a fail-open set() left in place
      // and a clear_capabilities() on the way out change what the tests compare.
      video::software.h264.capabilities.reset();
      video::software.hevc.capabilities.reset();
      video::software.av1.capabilities.reset();
      video::software.h264.capabilities.set(video::encoder_t::PASSED);
      video::software.hevc.capabilities.set(video::encoder_t::PASSED);
      video::software.hevc.capabilities.set(video::encoder_t::YUV444);
      video::software.av1.capabilities.set(video::encoder_t::DYNAMIC_RANGE);

      mail::man = std::make_shared<safe::mail_raw_t>();
      shutdown_event = mail::man->event<bool>(mail::shutdown);
      shutdown_event->raise(true);
    }

    void TearDown() override {
      shutdown_event.reset();
      mail::man = saved_mail;
      video::software.h264.capabilities = saved_capabilities[0];
      video::software.hevc.capabilities = saved_capabilities[1];
      video::software.av1.capabilities = saved_capabilities[2];
      video::active_hevc_mode = saved_hevc_mode;
      video::active_av1_mode = saved_av1_mode;
    }
  };
}  // namespace

TEST_F(EncoderProbeStopTest, ValidateEncoderAbandonsTheProbeAndKeepsTheCapabilityBits) {
  const auto h264_before = video::software.h264.capabilities;
  const auto hevc_before = video::software.hevc.capabilities;
  const auto av1_before = video::software.av1.capabilities;

  EXPECT_FALSE(video::validate_encoder(video::software, false));

  EXPECT_EQ(h264_before, video::software.h264.capabilities);
  EXPECT_EQ(hevc_before, video::software.hevc.capabilities);
  EXPECT_EQ(av1_before, video::software.av1.capabilities);
}

TEST_F(EncoderProbeStopTest, ProbeEncodersReturnsAFailureWithoutProbing) {
  const bool attempted_before = video::has_attempted_encoder_probe();

  EXPECT_NE(0, video::probe_encoders());

  // An abandoned probe is not an attempted one, and it leaves the codec modes
  // and the capability bits as they were.
  EXPECT_EQ(attempted_before, video::has_attempted_encoder_probe());
  EXPECT_EQ(saved_hevc_mode, video::active_hevc_mode);
  EXPECT_EQ(saved_av1_mode, video::active_av1_mode);
  EXPECT_TRUE(video::software.h264.capabilities.test(video::encoder_t::PASSED));
  EXPECT_FALSE(video::software.h264.capabilities.test(video::encoder_t::YUV444));
}
