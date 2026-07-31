/**
 * @file tests/unit/test_yuv444_fallback.cpp
 * @brief Unit tests for the YUV 4:4:4 -> 4:2:0 in-flight fallback.
 *
 * The fallback exists for hardware that advertises 4:4:4 and then refuses it. It
 * writes chromaSamplingType = 0 on the SESSION-LIFETIME config and tells the
 * client, and nothing ever puts 4:4:4 back — which was harmless only because the
 * session used to die on the same failure and the client renegotiated 4:4:4 on
 * reconnect.
 *
 * Holding the session open across a bounded GPU outage removes that. During an
 * outage every encoder build fails, 4:4:4 and 4:2:0 alike, because the GPU is
 * down — so the FIRST failure of a multi-minute outage is guaranteed to trip the
 * fallback, and the session then survives and streams 4:2:0 forever after an
 * event the system fully recovered from. A transient must not cost a capability.
 *
 * These tests pin the two rules that stop it, and that the real fallback still
 * works:
 *  - the recovery verdict is consulted BEFORE any mutation, so a display that is
 *    coming back can never cost the session its chroma;
 *  - the downgrade is provisional until a 4:2:0 retry proves it helped, which
 *    covers the race the verdict alone cannot: the verdict is published by the
 *    capture thread and read by the encoder thread, so the encoder can reach its
 *    failure a beat before the capture loop has sampled the ring.
 */
#include "../tests_common.h"
#include "src/yuv444_fallback.h"

using video::yuv444_fallback_e;
using video::yuv444_fallback_t;

namespace {
  constexpr int kYuv420 = 0;
  constexpr int kYuv444 = 1;

  constexpr bool recovering = true;
  constexpr bool not_recovering = false;
}  // namespace

TEST(Yuv444Fallback, ATransientOutageCannotCostTheSessionIts444) {
  // The regression. The capture side is holding the session open across a GPU
  // outage; every encoder build in that window fails for the same non-chroma
  // reason, and the first of them used to downgrade the session for good.
  int chroma = kYuv444;
  yuv444_fallback_t fallback {chroma};

  EXPECT_EQ(fallback.attempt(recovering), yuv444_fallback_e::keep_444);
  EXPECT_EQ(chroma, kYuv444) << "a recovered outage left the session at 4:2:0 for the rest of its life";
  EXPECT_FALSE(fallback.provisional());
}

TEST(Yuv444Fallback, A444SessionSurvivesAWholeOutageAndComesBackAt444) {
  // The whole point, over the shape of a real outage: the encoder retries
  // against the same session config for minutes, then the display returns.
  int chroma = kYuv444;

  for (int attempt = 0; attempt < 500; ++attempt) {
    yuv444_fallback_t fallback {chroma};
    ASSERT_EQ(fallback.attempt(recovering), yuv444_fallback_e::keep_444);
  }
  EXPECT_EQ(chroma, kYuv444);

  // Display back, encoder builds at 4:4:4 — nothing to fall back from, and the
  // client was never told the chroma changed.
  yuv444_fallback_t after {chroma};
  EXPECT_EQ(after.attempt(not_recovering), yuv444_fallback_e::try_420);
  EXPECT_TRUE(after.settle(true))
    << "this is the genuine hardware case and it must still commit";
  EXPECT_EQ(chroma, kYuv420);
}

TEST(Yuv444Fallback, HardwareThatRefuses444StillGetsDowngraded) {
  // A TV that cannot accept 4:4:4: the display is not recovering, 4:4:4 fails,
  // 4:2:0 works. Exactly today's behaviour, which the fix must not remove.
  int chroma = kYuv444;
  yuv444_fallback_t fallback {chroma};

  ASSERT_EQ(fallback.attempt(not_recovering), yuv444_fallback_e::try_420);
  EXPECT_EQ(chroma, kYuv420) << "the retry has to happen at 4:2:0 to mean anything";
  EXPECT_TRUE(fallback.provisional());

  EXPECT_TRUE(fallback.settle(true)) << "the caller must tell the client the chroma changed";
  EXPECT_EQ(chroma, kYuv420);
  EXPECT_FALSE(fallback.provisional());
}

TEST(Yuv444Fallback, ADowngradeThatDidNotHelpIsRevoked) {
  // Closes the race the verdict alone cannot: the recovery verdict is published
  // by the capture thread and read by the encoder thread, so an encoder that
  // fails at the very start of an outage can see "not recovering" and downgrade
  // before the capture loop has sampled the ring. A 4:2:0 retry that fails too
  // settles it after the fact — the failure was never about chroma.
  int chroma = kYuv444;
  yuv444_fallback_t fallback {chroma};

  ASSERT_EQ(fallback.attempt(not_recovering), yuv444_fallback_e::try_420);
  ASSERT_EQ(chroma, kYuv420);

  EXPECT_FALSE(fallback.settle(false)) << "the client must not be told about a downgrade that was undone";
  EXPECT_EQ(chroma, kYuv444) << "4:2:0 failed too, so the failure was never about chroma";
}

TEST(Yuv444Fallback, AnAlready420SessionIsUntouched) {
  int chroma = kYuv420;
  yuv444_fallback_t fallback {chroma};

  EXPECT_EQ(fallback.attempt(not_recovering), yuv444_fallback_e::not_applicable);
  EXPECT_EQ(fallback.attempt(recovering), yuv444_fallback_e::not_applicable);
  EXPECT_EQ(chroma, kYuv420);
  EXPECT_FALSE(fallback.settle(true)) << "nothing was downgraded, so nothing is to be announced";
}

TEST(Yuv444Fallback, SettlingWithoutAnAttemptAnnouncesNothing) {
  // The keep_444 path returns without mutating anything; a caller that settles
  // anyway must not raise a downgrade the session never had.
  int chroma = kYuv444;
  yuv444_fallback_t fallback {chroma};

  ASSERT_EQ(fallback.attempt(recovering), yuv444_fallback_e::keep_444);
  EXPECT_FALSE(fallback.settle(true));
  EXPECT_EQ(chroma, kYuv444);
}
