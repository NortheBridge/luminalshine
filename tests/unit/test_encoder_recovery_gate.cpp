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

#include <algorithm>
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

// --- The hold has to keep the CLIENT as well as the host session ----------

/**
 * The second half of the regression the gate above exists for.
 *
 * Holding the host's session open is worth nothing if the client leaves. A
 * Moonlight client that receives no video for a few seconds tears the session
 * down from its end, and the outages this whole mechanism exists to survive run
 * for MINUTES — so an encoder that answers a hold by emitting nothing saves the
 * host's session and loses the client's, which is worse than the teardown it
 * replaced.
 *
 * The sync encode path is where that bites: its push-image callback IS the
 * capture thread, so there is nothing to park, and skipping the encode outright
 * — the obvious way to keep failing encodes off a resetting GPU — is exactly
 * "emit nothing for the whole window". These pin the cadence that replaces it.
 */
TEST(RecoveryKeepalive, AHoldKeepsEmittingVideoForItsWholeLength) {
  // Three minutes of holding, polled at the capture loop's 10 ms rate — the
  // shape of a real recovery on the sync path.
  video::recovery_keepalive_t keepalive;
  keepalive.set_interval(500ms);

  int emitted = 0;
  std::chrono::nanoseconds longest_gap {0};
  auto last = at(0ms);

  for (auto t = 0ms; t < 3min; t += 10ms) {
    if (!keepalive.due(true, at(t))) {
      continue;
    }
    keepalive.note_emitted(at(t));
    longest_gap = std::max(longest_gap, at(t) - last);
    last = at(t);
    ++emitted;
  }

  EXPECT_GT(emitted, 0) << "the client received no video at all for the whole outage";
  EXPECT_NEAR(emitted, static_cast<int>(3min / 500ms), 2);
  EXPECT_LE(longest_gap, 1s) << "a gap this long is what the client's own timeout fires on";
}

TEST(RecoveryKeepalive, TheFirstTickOfAHoldIsDueImmediately) {
  // The client is already counting down when the hold starts; it should hear
  // from the host on the same tick, not an interval into the outage.
  video::recovery_keepalive_t keepalive;
  keepalive.set_interval(1s);

  EXPECT_TRUE(keepalive.due(true, at(0ms)));
}

TEST(RecoveryKeepalive, NothingIsEmittedWhenTheEncoderIsNotHolding) {
  // Normal streaming is untouched: the keepalive only ever runs in place of an
  // encode the hold would otherwise have skipped.
  video::recovery_keepalive_t keepalive;

  EXPECT_FALSE(keepalive.due(false, at(0ms)));
  EXPECT_FALSE(keepalive.due(false, at(1h)));
}

TEST(RecoveryKeepalive, TheCadenceIsPacedNotFreeRun) {
  // The callback is invoked every 10 ms during a hold. Emitting on every one of
  // them would aim ~100 encodes a second at a GPU that may still be resetting,
  // to re-send a picture that cannot change — there is no new capture.
  video::recovery_keepalive_t keepalive;
  keepalive.set_interval(500ms);

  ASSERT_TRUE(keepalive.due(true, at(0ms)));
  keepalive.note_emitted(at(0ms));

  EXPECT_FALSE(keepalive.due(true, at(10ms)));
  EXPECT_FALSE(keepalive.due(true, at(499ms)));
  EXPECT_TRUE(keepalive.due(true, at(500ms)));
}

TEST(RecoveryKeepalive, TheSessionsCadenceIsClampedAtBothEnds) {
  video::recovery_keepalive_t keepalive;

  keepalive.set_interval(16ms);  // a 60 fps session's minimum-FPS frame time
  EXPECT_EQ(keepalive.interval(), keepalive.config().fastest)
    << "a full-rate keepalive hammers a recovering GPU for minutes";

  keepalive.set_interval(30s);  // a session configured for a very low floor
  EXPECT_EQ(keepalive.interval(), keepalive.config().slowest)
    << "a keepalive slower than the client's no-video timeout is not a keepalive";

  keepalive.set_interval(400ms);
  EXPECT_EQ(keepalive.interval(), 400ms);
}

TEST(RecoveryKeepalive, AFailingKeepaliveIsPacedAndReportedSparingly) {
  // Every keepalive of a hold can fail — the GPU is down. The cadence must not
  // change (the moment the encoder can produce a frame again, the client gets
  // one) and the log must not fill with one line per attempt.
  video::recovery_keepalive_t keepalive;
  keepalive.set_interval(500ms);

  int reported = 0;
  for (auto t = 0ms; t < 3min; t += 10ms) {
    if (!keepalive.due(true, at(t))) {
      continue;
    }
    keepalive.note_emitted(at(t));  // the attempt happened; it just failed
    if (keepalive.should_report_failure(at(t))) {
      ++reported;
    }
  }

  EXPECT_GT(reported, 0) << "a multi-minute failing hold left no evidence in the log";
  EXPECT_LE(reported, static_cast<int>(3min / keepalive.config().failure_log_interval) + 1);
}

TEST(RecoveryKeepalive, ARealFrameResetsThePacerSoTheNextHoldEmitsAtOnce) {
  video::recovery_keepalive_t keepalive;
  keepalive.set_interval(1s);

  ASSERT_TRUE(keepalive.due(true, at(0ms)));
  keepalive.note_emitted(at(0ms));
  ASSERT_FALSE(keepalive.due(true, at(100ms)));

  keepalive.note_progress();
  EXPECT_TRUE(keepalive.due(true, at(100ms)));
}
