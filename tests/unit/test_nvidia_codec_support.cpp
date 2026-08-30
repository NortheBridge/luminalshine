/**
 * @file tests/unit/test_nvidia_codec_support.cpp
 * @brief Test the NVIDIA AV1-encode generation gate.
 *
 * The gate keeps a codec the GPU is known not to have out of the encoder
 * probe. It is a fast path, not a safety guard -- NVENC is still asked
 * directly via nvEncGetEncodeGUIDs() before any encoder is created -- so
 * these tests pin the boundary and, just as importantly, pin which way it
 * fails when it is wrong.
 */

// standard includes
#include <cstdint>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/platform/windows/nvidia_codec_support.h"

namespace {

  using platf::nvidia::kFirstAv1EncodeDeviceId;
  using platf::nvidia::supports_av1_encode;

  TEST(NvidiaCodecSupport, AmpereDoesNotAdvertiseAv1Encode) {
    // Issue #147 was reported on the RTX 3080 Ti. Ampere has AV1 decode only.
    EXPECT_FALSE(supports_av1_encode(0x2208));  // RTX 3080 Ti (GA102)
    EXPECT_FALSE(supports_av1_encode(0x2204));  // RTX 3090   (GA102)
    EXPECT_FALSE(supports_av1_encode(0x2484));  // RTX 3070   (GA104)
    EXPECT_FALSE(supports_av1_encode(0x2503));  // RTX 3060   (GA106)
    EXPECT_FALSE(supports_av1_encode(0x25FB));  // RTX A500 Embedded — highest Ampere display part
  }

  TEST(NvidiaCodecSupport, AdaAdvertisesAv1Encode) {
    EXPECT_TRUE(supports_av1_encode(0x2684));  // RTX 4090 (AD102)
    EXPECT_TRUE(supports_av1_encode(0x2704));  // RTX 4080 (AD103)
    EXPECT_TRUE(supports_av1_encode(0x2782));  // RTX 4070 Ti (AD104)
  }

  TEST(NvidiaCodecSupport, AdaWorkstationAndDatacenterPartsAdvertiseAv1Encode) {
    // These sit just above the boundary and are easy to strand by mistake.
    EXPECT_TRUE(supports_av1_encode(0x26B1));  // RTX 6000 Ada
    EXPECT_TRUE(supports_av1_encode(0x26B5));  // L40
    EXPECT_TRUE(supports_av1_encode(0x27B8));  // L4
    EXPECT_TRUE(supports_av1_encode(0x27B2));  // RTX 4000 Ada
  }

  TEST(NvidiaCodecSupport, AdaLaptopPartsWithAmpereBrandingAdvertiseAv1Encode) {
    // "RTX 3050 A Laptop" is 30-series *branding* on Ada AD106/AD107 silicon
    // and does have AV1 encode. A marketing-name check would get these wrong;
    // the device ID gets them right.
    EXPECT_TRUE(supports_av1_encode(0x2822));
    EXPECT_TRUE(supports_av1_encode(0x28A3));
  }

  TEST(NvidiaCodecSupport, OlderGenerationsDoNotAdvertiseAv1Encode) {
    EXPECT_FALSE(supports_av1_encode(0x1E04));  // RTX 2080 Ti (Turing TU102)
    EXPECT_FALSE(supports_av1_encode(0x2182));  // GTX 1660 Ti (Turing TU116)
    EXPECT_FALSE(supports_av1_encode(0x1B80));  // GTX 1080    (Pascal GP104)
    EXPECT_FALSE(supports_av1_encode(0x13C0));  // GTX 980     (Maxwell GM204)
  }

  TEST(NvidiaCodecSupport, BoundaryIsExactAndTight) {
    // The gap below the boundary is a single device ID: 0x2600-0x267F is
    // unallocated, the highest Ampere part is 0x25FB and the lowest Ada part
    // is 0x2681 (TITAN Ada). If either edge ever moves, this fails first.
    EXPECT_EQ(kFirstAv1EncodeDeviceId, 0x2680u);
    EXPECT_FALSE(supports_av1_encode(kFirstAv1EncodeDeviceId - 1));
    EXPECT_TRUE(supports_av1_encode(kFirstAv1EncodeDeviceId));
    EXPECT_TRUE(supports_av1_encode(0x2681));  // TITAN Ada — lowest known Ada
  }

  TEST(NvidiaCodecSupport, UnknownFutureDeviceIdsFailOpen) {
    // Deliberate: an unrecognised high ID is treated as AV1-capable so a GPU
    // released after this code was written is not denied a codec it has. If
    // the guess is wrong the NVENC codec-GUID query rejects it and the probe
    // moves on, which since the create_encoder teardown fix costs nothing.
    //
    // The known cost of failing open, all unreachable through DXGI display
    // adapter enumeration in practice: datacenter parts carrying no NVENC at
    // all, and Jetson Thor, whose AV1 encoder was removed in hardware.
    EXPECT_TRUE(supports_av1_encode(0x2C02));  // RTX 5080 (GB203) — correct
    EXPECT_TRUE(supports_av1_encode(0x2B85));  // RTX 5090 (GB202) — correct
    EXPECT_TRUE(supports_av1_encode(0x2901));  // B200-class — no NVENC; accepted
    EXPECT_TRUE(supports_av1_encode(0x2B00));  // Jetson AGX Thor — accepted
    EXPECT_TRUE(supports_av1_encode(0xFFFFFFFFu));
  }

  TEST(NvidiaCodecSupport, IsUsableInConstantExpressions) {
    // The gate is consulted on every probe pass; keeping it constexpr keeps
    // it free and keeps it testable without a GPU.
    static_assert(!supports_av1_encode(0x2208), "Ampere must not claim AV1 encode");
    static_assert(supports_av1_encode(0x2684), "Ada must claim AV1 encode");
    SUCCEED();
  }

}  // namespace
