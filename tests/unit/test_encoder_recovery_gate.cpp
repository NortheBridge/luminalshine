/**
 * @file tests/unit/test_encoder_recovery_gate.cpp
 * @brief Unit tests for the encoder-side half of display-recovery handling.
 *
 * Teaching the CAPTURE loop to ride out a bounded GPU outage instead of
 * reinitializing is only half a fix, and on its own it is a regression. The
 * reinit used to park the encoder thread for the length of the outage;
 * deferring it leaves that thread RUNNING, and every failure it meets while the
 * GPU is down — a failed encode on the sync path, a failed encode-device
 * creation on the async one — takes the ordinary path, which ends the client's
 * session. The outage still kills the stream, just from the other thread.
 *
 * These tests pin the contract that stops that:
 *  - while the capture source reports recovery, the encoder HOLDS the session;
 *  - the hold is bounded, and when the bound expires today's failure handling
 *    runs unchanged;
 *  - a source that is not recovering gets today's failure handling on the first
 *    observation, so every capture path that never publishes recovery (all of
 *    them but the LuminalVGD ring) is untouched;
 *  - the bound measures the OUTAGE, not wall-clock time: a source that flaps in
 *    and out of recovery cannot refresh it, and only real encoder progress
 *    clears it.
 */
#include "../tests_common.h"
#include "src/encoder_recovery_gate.h"

#include <chrono>

using video::encoder_recovery_action_e;
using video::encoder_recovery_gate_config_t;
using video::encoder_recovery_gate_t;
using namespace std::chrono_literals;

namespace {
  encoder_recovery_gate_t::time_point at(std::chrono::nanoseconds offset) {
    return encoder_recovery_gate_t::time_point {} + offset;
  }

  constexpr bool recovering = true;
  constexpr bool not_recovering = false;
}  // namespace

TEST(DisplayRecoveryGate, ADisplayThatIsNotRecoveringGetsTodaysFailureHandling) {
  // Every capture backend but the LuminalVGD ring publishes nothing, so this is
  // the path they all take: an encoder failure ends the session exactly as it
  // does today, on the first observation, with no waiting introduced anywhere.
  encoder_recovery_gate_t gate;

  EXPECT_EQ(gate.evaluate(not_recovering, at(0s)), encoder_recovery_action_e::fail);
  EXPECT_EQ(gate.evaluate(not_recovering, at(1h)), encoder_recovery_action_e::fail);
  EXPECT_EQ(gate.held_for(), 0ns);
}

TEST(DisplayRecoveryGate, ARecoveringDisplayHoldsTheSessionInsteadOfEndingIt) {
  // The regression this exists for: the capture loop defers its reinit, so the
  // encoder thread is awake across the outage, and its first failure would
  // otherwise raise the session's shutdown event.
  encoder_recovery_gate_t gate;

  EXPECT_EQ(gate.evaluate(recovering, at(0s)), encoder_recovery_action_e::hold);
}

TEST(DisplayRecoveryGate, HoldSurvivesAMultiMinuteOutage) {
  // The driver rides a GPU outage out for minutes with its monitor still
  // arrived. Polled the way the encoder's wait loop polls it (50x/s), the gate
  // must keep saying hold for all of it.
  encoder_recovery_gate_t gate;

  for (auto t = 0ms; t < 5min; t += 20ms) {
    ASSERT_EQ(gate.evaluate(recovering, at(t)), encoder_recovery_action_e::hold)
      << "gave up at " << std::chrono::duration_cast<std::chrono::seconds>(t).count() << "s";
  }
  EXPECT_GE(gate.held_for(), 5min - 20ms);
}

TEST(DisplayRecoveryGate, HoldIsBoundedAndFallsBackToTodaysFailureHandling) {
  encoder_recovery_gate_t gate;

  // Above the capture loop's own deferral ceiling, which is above the ring
  // reader's budget, which is above the driver's: the give-up that actually
  // fires is always the most specific one, i.e. the one with the diagnosis.
  EXPECT_GT(gate.config().hold_ceiling, 15min);

  EXPECT_EQ(gate.evaluate(recovering, at(0s)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(15min)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(gate.config().hold_ceiling + 1s)),
            encoder_recovery_action_e::fail)
    << "a publisher stuck reporting recovery must not wedge an encoder thread";
}

TEST(DisplayRecoveryGate, TheCeilingMeasuresTheOutageNotWallClockTime) {
  // Hours of healthy streaming between two short outages must not consume the
  // budget of either: only time actually spent holding counts.
  encoder_recovery_gate_t gate;

  EXPECT_EQ(gate.evaluate(not_recovering, at(0s)), encoder_recovery_action_e::fail);
  EXPECT_EQ(gate.evaluate(not_recovering, at(3h)), encoder_recovery_action_e::fail);
  EXPECT_EQ(gate.held_for(), 0ns);

  EXPECT_EQ(gate.evaluate(recovering, at(3h)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(3h + 30s)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.held_for(), 30s);
}

TEST(DisplayRecoveryGate, FlappingRecoveryCannotRefreshTheCeiling) {
  // A source that blinks between recovering and not must still exhaust the
  // ceiling. Charging each interval to the state it was actually spent in is
  // what makes that true — an implementation that reset on every blink would
  // hold forever at this sampling pattern.
  encoder_recovery_gate_t gate;

  bool state = recovering;
  auto t = 0ms;
  for (; t < 60min; t += 100ms) {
    if (gate.evaluate(state, at(t)) == encoder_recovery_action_e::fail && state == recovering) {
      break;
    }
    state = !state;
  }

  EXPECT_LT(t, 60min) << "flapping recovery held the session open past every bound";
  EXPECT_GT(gate.held_for(), gate.config().hold_ceiling);
}

TEST(DisplayRecoveryGate, ProgressClearsTheCeilingForALaterOutage) {
  // A frame encoded, or an encode device built against the display, means the
  // outage the gate was holding for is over. The next one starts fresh.
  encoder_recovery_gate_t gate;

  EXPECT_EQ(gate.evaluate(recovering, at(0s)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(15min)), encoder_recovery_action_e::hold);
  ASSERT_GT(gate.held_for(), 0ns);

  gate.note_progress();
  EXPECT_EQ(gate.held_for(), 0ns);

  // A second outage half an hour later gets a full ceiling of its own, not the
  // remains of the first one's.
  EXPECT_EQ(gate.evaluate(recovering, at(45min)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(60min)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.held_for(), 15min);
}

TEST(DisplayRecoveryGate, ProgressWhileNotRecoveringIsHarmless) {
  encoder_recovery_gate_t gate;

  gate.note_progress();
  EXPECT_EQ(gate.evaluate(not_recovering, at(0s)), encoder_recovery_action_e::fail);
  EXPECT_EQ(gate.evaluate(recovering, at(1s)), encoder_recovery_action_e::hold);
}

TEST(DisplayRecoveryGate, ConfigIsHonoured) {
  encoder_recovery_gate_config_t cfg;
  cfg.hold_ceiling = 30s;
  encoder_recovery_gate_t gate {cfg};

  EXPECT_EQ(gate.evaluate(recovering, at(0s)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(20s)), encoder_recovery_action_e::hold);
  EXPECT_EQ(gate.evaluate(recovering, at(31s)), encoder_recovery_action_e::fail);
}
