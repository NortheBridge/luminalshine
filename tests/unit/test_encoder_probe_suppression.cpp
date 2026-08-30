/**
 * @file tests/unit/test_encoder_probe_suppression.cpp
 * @brief Per-encoder, per-codec probe-fault suppression.
 *
 * The behaviour this protects: a graphics-driver fault while probing ONE codec
 * used to cost the user the whole encoder on every probe pass, forever, because
 * the probe re-runs on each launch and resume and negative results are not
 * cached. Recording the faulted codec lets the next pass skip just that codec
 * and validate the rest, so an AV1-specific driver bug no longer means software
 * encoding for H.264 and HEVC too.
 *
 * The registry is deliberately pure so the policy is testable without a GPU,
 * a display, or a driver fault to trigger.
 */

// standard includes
#include <thread>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/encoder_probe_suppression.h"

namespace {

  using video::probe_suppression::codec_e;
  using video::probe_suppression::codec_name;
  using video::probe_suppression::is_suppressible;
  using video::probe_suppression::registry_t;

  TEST(EncoderProbeSuppression, NothingIsSuppressedInitially) {
    registry_t reg;
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::hevc));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::av1));
  }

  TEST(EncoderProbeSuppression, SuppressingOneCodecLeavesTheOthersProbeable) {
    // The whole point of the change: an AV1 fault must not cost H.264/HEVC.
    registry_t reg;
    reg.suppress("nvenc", codec_e::av1);

    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::hevc));
  }

  TEST(EncoderProbeSuppression, SuppressionIsScopedToOneEncoder) {
    // A driver bug in NVENC's AV1 path says nothing about AMF or QSV, which
    // are separate vendors' code — they must still be probed normally.
    registry_t reg;
    reg.suppress("nvenc", codec_e::av1);

    EXPECT_FALSE(reg.is_suppressed("amdvce", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("quicksync", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("software", codec_e::av1));
  }

  TEST(EncoderProbeSuppression, H264IsNeverSuppressible) {
    // Load-bearing, not conservatism. H.264 is mandatory, so suppressing it
    // buys no partial result — but it would cost everything: the probe's
    // shield also catches faults from shared setup (display acquisition, D3D
    // device creation, DXGI duplication) and, on Windows, ordinary C++
    // exceptions such as a std::bad_alloc from a wedged display stack. One
    // transient fault of that kind sweeping the candidate list would suppress
    // H.264 on every encoder INCLUDING the last-resort "software" entry,
    // leaving probe_encoders() nothing to return. Unlike the per-pass
    // encoder_list copy, this registry outlives the pass, so every launch and
    // resume would answer HTTP 503 until the service restarted.
    EXPECT_FALSE(is_suppressible(codec_e::h264));
    EXPECT_FALSE(is_suppressible(codec_e::unattributed));
    EXPECT_TRUE(is_suppressible(codec_e::hevc));
    EXPECT_TRUE(is_suppressible(codec_e::av1));

    registry_t reg;
    reg.suppress("nvenc", codec_e::h264);
    reg.suppress("software", codec_e::h264);
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
    EXPECT_FALSE(reg.is_suppressed("software", codec_e::h264));
  }

  TEST(EncoderProbeSuppression, TheSoftwareFallbackCanNeverBeDisabled) {
    // The last-resort encoder must survive any sequence of faults, or the host
    // has no encoder at all and refuses every connection.
    registry_t reg;
    for (const auto codec : {codec_e::h264, codec_e::hevc, codec_e::av1, codec_e::unattributed}) {
      reg.suppress("software", codec);
    }
    EXPECT_FALSE(reg.is_suppressed("software", codec_e::h264));
  }

  TEST(EncoderProbeSuppression, UnattributedFaultsSuppressNothing) {
    // Faults in shared setup (the SDR->HDR display reset between codec probes)
    // cannot be blamed on a codec. Guessing would disable a healthy one, so the
    // encoder is simply probed in full next pass.
    registry_t reg;
    reg.suppress("nvenc", codec_e::unattributed);

    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));  // also non-suppressible by policy
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::hevc));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::unattributed));
  }

  TEST(EncoderProbeSuppression, RepeatedFaultsAreIdempotent) {
    registry_t reg;
    reg.suppress("nvenc", codec_e::hevc);
    reg.suppress("nvenc", codec_e::hevc);

    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::hevc));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
  }

  TEST(EncoderProbeSuppression, CodecsAccumulateIndependently) {
    // A second fault on a different codec must not clear the first.
    registry_t reg;
    reg.suppress("nvenc", codec_e::av1);
    reg.suppress("nvenc", codec_e::hevc);

    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::av1));
    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::hevc));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
  }

  TEST(EncoderProbeSuppression, ClearDropsEverything) {
    registry_t reg;
    reg.suppress("nvenc", codec_e::av1);
    reg.suppress("amdvce", codec_e::hevc);

    reg.clear();

    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("amdvce", codec_e::hevc));
  }

  TEST(EncoderProbeSuppression, ProcessRegistryIsASingleInstance) {
    auto &a = video::probe_suppression::process_registry();
    auto &b = video::probe_suppression::process_registry();
    EXPECT_EQ(&a, &b);
  }

  TEST(EncoderProbeSuppression, ConcurrentAccessIsSafe) {
    // probe_encoders() can run concurrently with a stream start, so the
    // registry is consulted and written from more than one thread.
    registry_t reg;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&reg, i] {
        // Cycle through every enumerator, suppressible or not, so the
        // non-suppressible early-outs are exercised under contention too.
        const auto codec = static_cast<codec_e>(i % 4);
        for (int n = 0; n < 200; ++n) {
          reg.suppress("nvenc", codec);
          (void) reg.is_suppressed("nvenc", codec);
          (void) reg.is_suppressed("amdvce", codec);
        }
      });
    }
    for (auto &t : threads) {
      t.join();
    }

    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::hevc));
    EXPECT_TRUE(reg.is_suppressed("nvenc", codec_e::av1));
    EXPECT_FALSE(reg.is_suppressed("nvenc", codec_e::h264));
    EXPECT_FALSE(reg.is_suppressed("amdvce", codec_e::hevc));
  }

  TEST(EncoderProbeSuppression, CodecNamesAreLoggable) {
    EXPECT_STREQ(codec_name(codec_e::h264), "H.264");
    EXPECT_STREQ(codec_name(codec_e::hevc), "HEVC");
    EXPECT_STREQ(codec_name(codec_e::av1), "AV1");
    EXPECT_STREQ(codec_name(codec_e::unattributed), "an unattributed stage");
  }

}  // namespace
