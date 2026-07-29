/**
 * @file tests/unit/test_nvenc_encode_wait.cpp
 * @brief The OS-anchored encode-wait deadline.
 *
 * Regression coverage for the false-TDR incident: a single 100 ms NVENC
 * completion-event timeout was reported as "GPU TDR escalated to WDDM stack
 * failure" and terminated the live session. Five field escalations were
 * examined and all five were false — stalls of 100-105 ms, hresult 0x0,
 * zero GPU resets in telemetry, and no nvlddmkm/dxgkrnl/4101 events.
 *
 * The deadline is now derived from Windows' own TDR constants rather than
 * tuned per GPU, because the wait is dominated by cross-engine queueing
 * behind the game's render work, not by encoder throughput. These tests pin
 * the derivation and its clamps; they are pure and need no GPU.
 */

// standard includes
#include <cstdint>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/nvenc/nvenc_config.h"

namespace {

  TEST(NvencEncodeWaitDeadline, StockWindowsDefaultsYieldSevenSeconds) {
    // TdrDelay 2 s + TdrDdiDelay 5 s — the documented Microsoft defaults,
    // which apply whenever the registry values are absent (the usual case).
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(2, 5), 7000u);
  }

  TEST(NvencEncodeWaitDeadline, NeverDropsBelowTheObservedLegitimateMax) {
    // The floor exists because a machine with TdrDelay=1 must still not
    // produce a deadline under the largest legitimate wait seen in the
    // field (3.9 s on an RTX 5080 with zero GPU resets). 3000 ms is the
    // floor; anything summing below it clamps up.
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(1, 1), 3000u);
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(0, 0), 3000u);
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(1, 2), 3000u);
  }

  TEST(NvencEncodeWaitDeadline, CapsPathologicalRegistryValues) {
    // TdrDelay=60 is common overclocking/compute advice and ships on some
    // OEM images; it must not become a minute-long encode-thread block.
    // The 10 s cap is still safe against the client, which has no
    // mid-stream video deadline.
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(60, 5), 10000u);
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(10, 10), 10000u);
    // Boundary: exactly the cap is kept, not clamped away.
    EXPECT_EQ(nvenc::encode_wait_deadline_from_tdr(5, 5), 10000u);
  }

  TEST(NvencEncodeWaitDeadline, IsMonotonicInBothInputs) {
    // A more permissive OS setting must never produce a tighter deadline.
    for (std::uint32_t d = 0; d <= 12; ++d) {
      for (std::uint32_t ddi = 0; ddi <= 12; ++ddi) {
        const auto here = nvenc::encode_wait_deadline_from_tdr(d, ddi);
        EXPECT_GE(nvenc::encode_wait_deadline_from_tdr(d + 1, ddi), here);
        EXPECT_GE(nvenc::encode_wait_deadline_from_tdr(d, ddi + 1), here);
      }
    }
  }

  TEST(NvencEncodeWaitDeadline, AlwaysLandsInsideTheDocumentedEnvelope) {
    // Whatever the registry says, the result must stay within [3 s, 10 s]:
    // above the legitimate-wait tail, below the point where blocking the
    // encode thread becomes its own failure mode.
    for (std::uint32_t d = 0; d <= 120; d += 7) {
      for (std::uint32_t ddi = 0; ddi <= 120; ddi += 11) {
        const auto ms = nvenc::encode_wait_deadline_from_tdr(d, ddi);
        EXPECT_GE(ms, 3000u);
        EXPECT_LE(ms, 10000u);
      }
    }
  }

  TEST(NvencEncodeWaitDeadline, DefaultConfigMatchesStockWindows) {
    // The struct default is what non-Windows callers and any path that
    // skips the registry read will use; it must equal the stock-Windows
    // derivation rather than drifting on its own.
    const nvenc::nvenc_config cfg {};
    EXPECT_EQ(cfg.encode_wait_timeout_ms, nvenc::encode_wait_deadline_from_tdr(2, 5));
    // The slice must divide the work finely enough to stay responsive, and
    // the soft warn must sit strictly between a slice and the deadline so
    // it can actually fire before the hard deadline is reached.
    EXPECT_LT(cfg.encode_wait_poll_slice_ms, cfg.encode_stall_warn_ms);
    EXPECT_LT(cfg.encode_stall_warn_ms, cfg.encode_wait_timeout_ms);
  }

  TEST(NvencEncodeWaitDeadline, DeadlineExceedsTheStallsThatUsedToKillSessions) {
    // Every observed false escalation stalled 100-105 ms. The new deadline
    // must be far above that band, so none of them would reach it — and
    // even reaching it no longer terminates a session without the device
    // corroborating (see nvenc_base.cpp's corroboration gate).
    const auto deadline = nvenc::encode_wait_deadline_from_tdr(2, 5);
    for (const std::uint32_t observed_stall_ms : {100u, 102u, 104u, 105u}) {
      EXPECT_GT(deadline, observed_stall_ms * 10);
    }
  }

}  // namespace
