#include "src/platform/windows/nvidia_codec_support.h"

#include <gtest/gtest.h>

TEST(NvidiaCodecSupport, AmpereDoesNotAdvertiseAv1Encode) {
  EXPECT_FALSE(platf::nvidia::supports_av1_encode(0x2208));  // RTX 3080 Ti
}

TEST(NvidiaCodecSupport, AdaAdvertisesAv1Encode) {
  EXPECT_TRUE(platf::nvidia::supports_av1_encode(0x2684));  // RTX 4090
  EXPECT_TRUE(platf::nvidia::supports_av1_encode(0x2704));  // RTX 4080
}

TEST(NvidiaCodecSupport, OlderGenerationsDoNotAdvertiseAv1Encode) {
  EXPECT_FALSE(platf::nvidia::supports_av1_encode(0x1E04));  // RTX 2080 Ti
}

TEST(NvidiaCodecSupport, FutureDeviceIdsRemainEligible) {
  EXPECT_TRUE(platf::nvidia::supports_av1_encode(0x2C02));
}
