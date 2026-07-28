/**
 * @file tests/unit/test_tdr_lifeboat.cpp
 * @brief The TDR topology lifeboat's pure attempt decision.
 *
 * Covers the gate that turns a stream of tdr:: detections into at most one
 * lifeboat attempt per incident (POSTMORTEM-2026-07-27: one GPU hang was
 * re-detected ~22 times across several subsystems — the lifeboat must fire
 * on the first detection and stay quiet for the rest). Only the decision
 * logic is tested here; actual topology changes go through the display
 * helper and are exercised on real hardware, not in unit tests.
 */

// standard includes
#include <chrono>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/platform/windows/tdr_lifeboat.h"

namespace {
  using namespace std::chrono_literals;
  using std::chrono::steady_clock;

  constexpr tdr::source_t kAllSources[] = {
    tdr::source_t::encoder_d3d11,
    tdr::source_t::dd_test_d3d11,
    tdr::source_t::virtual_display_enumerate,
    tdr::source_t::query_display_config,
    tdr::source_t::encoder_stall,
    tdr::source_t::display_stack_down,
  };

  TEST(TdrLifeboat, AnyTdrClassSourceTriggersAFreshGate) {
    // Every detector qualifies: the earliest (encoder_stall, ~100 ms into
    // the hang) is the most valuable, but even a late one deserves the
    // single bounded attempt.
    for (const auto source : kAllSources) {
      tdr_lifeboat::attempt_gate gate;
      EXPECT_TRUE(gate.should_attempt(source, steady_clock::now()))
        << "source " << static_cast<int>(source) << " must trigger on a fresh gate";
    }
  }

  TEST(TdrLifeboat, UnknownSourceValueNeverTriggers) {
    tdr_lifeboat::attempt_gate gate;
    EXPECT_FALSE(gate.should_attempt(static_cast<tdr::source_t>(0), steady_clock::now()));
    EXPECT_FALSE(gate.should_attempt(static_cast<tdr::source_t>(999), steady_clock::now()));
  }

  TEST(TdrLifeboat, SecondCallWithinCooldownIsSuppressed) {
    tdr_lifeboat::attempt_gate gate;
    const auto t0 = steady_clock::now();

    ASSERT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0));

    // The same hang cascading through other detectors moments later, and
    // being re-detected by the next client attempt ~33 s in.
    EXPECT_FALSE(gate.should_attempt(tdr::source_t::encoder_d3d11, t0 + 150ms));
    EXPECT_FALSE(gate.should_attempt(tdr::source_t::query_display_config, t0 + 33s));
    EXPECT_FALSE(gate.should_attempt(tdr::source_t::encoder_stall, t0 + 4min + 59s));
  }

  TEST(TdrLifeboat, CallAfterCooldownIsAllowed) {
    tdr_lifeboat::attempt_gate gate;
    const auto t0 = steady_clock::now();

    ASSERT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0));
    EXPECT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0 + 5min))
      << "a detection at exactly the cooldown boundary is a new incident";
  }

  TEST(TdrLifeboat, SuppressedCallsDoNotExtendTheCooldown) {
    // A wedge that keeps re-firing (~33 s cadence in the field) must not
    // starve the next attempt: denials leave the armed timestamp alone.
    tdr_lifeboat::attempt_gate gate;
    const auto t0 = steady_clock::now();

    ASSERT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0));
    for (auto t = t0 + 33s; t < t0 + 5min; t += 33s) {
      EXPECT_FALSE(gate.should_attempt(tdr::source_t::encoder_d3d11, t));
    }
    EXPECT_TRUE(gate.should_attempt(tdr::source_t::encoder_d3d11, t0 + 5min + 1s));
  }

  TEST(TdrLifeboat, TrueReturnArmsTheCooldownImmediately) {
    // The gate arms on the decision, not on attempt completion, so a
    // lifeboat attempt that itself provokes tdr:: events (enumeration
    // failing on a dead stack) cannot re-enter and spawn another attempt.
    tdr_lifeboat::attempt_gate gate;
    const auto t0 = steady_clock::now();

    ASSERT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0));
    EXPECT_FALSE(gate.should_attempt(tdr::source_t::virtual_display_enumerate, t0))
      << "an event at the same instant must fold into the armed attempt";
  }

  TEST(TdrLifeboat, CustomCooldownIsHonored) {
    tdr_lifeboat::attempt_gate gate {std::chrono::seconds {10}};
    const auto t0 = steady_clock::now();

    ASSERT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0));
    EXPECT_FALSE(gate.should_attempt(tdr::source_t::encoder_stall, t0 + 9s));
    EXPECT_TRUE(gate.should_attempt(tdr::source_t::encoder_stall, t0 + 10s));
  }
}  // namespace
