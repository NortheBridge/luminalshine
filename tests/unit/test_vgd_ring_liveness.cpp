/**
 * @file tests/unit/test_vgd_ring_liveness.cpp
 * @brief Unit tests for the LuminalVGD ring liveness ladder.
 *
 * Driver build 16 stopped departing its IddCx monitor on a TDR: it tears down
 * the D3D device and swapchain, marks the frame ring REBUILDING, and keeps
 * ticking a device-independent heartbeat while it waits — for minutes — for the
 * GPU to come back. Before build 16 a ring sat in REBUILDING for ~10 ms.
 *
 * These tests pin the resulting contract:
 *  - REBUILDING with a LIVE heartbeat is RECOVERING: wait, keep the session,
 *    and defer (never cancel) a pending GPU-reset reinit.
 *  - REBUILDING with a STALE or ABSENT heartbeat is still dead on exactly
 *    today's timeline, and a GPU-reset edge outside a recovery window still
 *    reinitializes immediately (pre-build-16 drivers depend on both).
 *  - Every wait is bounded; when the bound expires the old failure handling runs.
 *  - The delivery-stall circuit breaker cannot survive a rebuild — the driver
 *    frees every slot without rewinding latest_sequence, so a frame published
 *    just before the outage is unclaimable forever and must not be counted as
 *    evidence that the reader is broken.
 */
#include "../tests_common.h"
#include "src/platform/windows/vgd_ring_liveness.h"

#include <chrono>
#include <optional>

using platf::dxgi::vgd_ring_decision_t;
using platf::dxgi::vgd_ring_liveness_config_t;
using platf::dxgi::vgd_ring_liveness_t;
using platf::dxgi::vgd_ring_reason_e;
using platf::dxgi::vgd_ring_sample_t;
using platf::dxgi::vgd_ring_verdict_e;
namespace vgd_ring_state = platf::dxgi::vgd_ring_state;
using namespace std::chrono_literals;

namespace {
  vgd_ring_liveness_t::time_point at(std::chrono::nanoseconds offset) {
    return vgd_ring_liveness_t::time_point {} + offset;
  }

  /// A healthy ACTIVE ring with a fresh heartbeat and nothing undelivered.
  vgd_ring_sample_t healthy(uint64_t sequence = 100) {
    vgd_ring_sample_t s;
    s.state = vgd_ring_state::active;
    s.generation = 7;
    s.latest_sequence = sequence;
    s.delivered_sequence = sequence;
    s.heartbeat_valid = true;
    s.heartbeat_age = 250ms;  // the driver's TDR tick cadence
    s.tdr_edge = false;
    return s;
  }

  /// build-16 duck: REBUILDING, monitor still arrived, heartbeat still beating.
  vgd_ring_sample_t recovering_sample(uint64_t latest, uint64_t delivered, uint32_t generation = 7) {
    auto s = healthy(latest);
    s.state = vgd_ring_state::rebuilding;
    s.generation = generation;
    s.delivered_sequence = delivered;
    return s;
  }
}  // namespace

TEST(VgdRingLiveness, HealthyRingIsConsumed) {
  vgd_ring_liveness_t live;

  const auto d = live.evaluate(healthy(), at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::consume);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::healthy);
}

TEST(VgdRingLiveness, DeadRingReinits) {
  vgd_ring_liveness_t live;

  auto s = healthy();
  s.state = vgd_ring_state::dead;

  const auto d = live.evaluate(s, at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::ring_dead);
}

// --- The build-16 discriminator -------------------------------------------

TEST(VgdRingLiveness, RebuildingWithLiveHeartbeatIsRecoveringNotDead) {
  vgd_ring_liveness_t live;

  const auto s = recovering_sample(100, 99);
  const auto d = live.evaluate(s, at(0s));

  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::recovering);
  EXPECT_TRUE(live.recovering(s, at(0s)));
}

TEST(VgdRingLiveness, RecoveringSurvivesManyMinutesOfRebuilding) {
  vgd_ring_liveness_t live;
  const auto s = recovering_sample(100, 99);

  // Sample it the way the capture loop does (~100x/s) across five minutes.
  for (auto t = 0s; t < 5min; t += 1s) {
    const auto d = live.evaluate(s, at(t));
    ASSERT_EQ(d.verdict, vgd_ring_verdict_e::wait) << "gave up at " << t.count() << "s";
    ASSERT_EQ(d.reason, vgd_ring_reason_e::recovering);
  }
  EXPECT_EQ(live.rebuilding_for(at(5min)), 5min);
  EXPECT_TRUE(live.recovering(s, at(5min)));
}

TEST(VgdRingLiveness, RecoveryIsBoundedAndFallsBackToTodaysFailureHandling) {
  vgd_ring_liveness_t live;
  const auto s = recovering_sample(100, 99);

  // The default budget sits above the driver's own 10-minute recovery budget
  // so the driver's give-up lands first; this is the backstop for a driver
  // that heartbeats forever without ever finishing.
  EXPECT_GT(live.config().recovery_budget, 10min);

  EXPECT_EQ(live.evaluate(s, at(0s)).verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(live.evaluate(s, at(10min)).verdict, vgd_ring_verdict_e::wait);

  const auto expired = live.evaluate(s, at(live.config().recovery_budget + 1s));
  EXPECT_EQ(expired.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(expired.reason, vgd_ring_reason_e::recovery_budget_expired);

  // And the const query inherits the bound, so deferrals elsewhere end too.
  EXPECT_FALSE(live.recovering(s, at(live.config().recovery_budget + 1s)));
}

// --- Genuine dead-driver detection stays on today's timeline ---------------

TEST(VgdRingLiveness, RebuildingWithStaleHeartbeatStillDiesOnTodaysTimeline) {
  vgd_ring_liveness_t live;

  auto s = recovering_sample(100, 99);
  s.heartbeat_age = 3s;  // past the 2 s stale threshold

  const auto stale = live.evaluate(s, at(0s));
  EXPECT_EQ(stale.verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(stale.reason, vgd_ring_reason_e::heartbeat_stale);
  EXPECT_FALSE(live.recovering(s, at(0s))) << "a stale heartbeat must never read as recovering";

  // Still inside the 10 s grace window.
  EXPECT_EQ(live.evaluate(s, at(9s)).verdict, vgd_ring_verdict_e::wait);

  const auto lost = live.evaluate(s, at(11s));
  EXPECT_EQ(lost.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(lost.reason, vgd_ring_reason_e::heartbeat_lost);
}

TEST(VgdRingLiveness, HeartbeatThatStopsMidRecoveryIsDetected) {
  vgd_ring_liveness_t live;

  // Two minutes of honest recovery...
  auto s = recovering_sample(100, 99);
  for (auto t = 0s; t < 2min; t += 10s) {
    ASSERT_EQ(live.evaluate(s, at(t)).reason, vgd_ring_reason_e::recovering);
  }

  // ...then the driver stops beating. Detection must not inherit the recovery
  // budget: it runs on the ordinary stale ladder from the moment it goes stale.
  s.heartbeat_age = 5s;
  EXPECT_EQ(live.evaluate(s, at(2min)).reason, vgd_ring_reason_e::heartbeat_stale);
  EXPECT_EQ(live.evaluate(s, at(2min + 9s)).verdict, vgd_ring_verdict_e::wait);

  const auto lost = live.evaluate(s, at(2min + 11s));
  EXPECT_EQ(lost.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(lost.reason, vgd_ring_reason_e::heartbeat_lost);
}

TEST(VgdRingLiveness, RebuildingWithoutAHeartbeatIsNeverReportedRecovering) {
  vgd_ring_liveness_t live;

  // A pre-heartbeat driver: nothing proves it is alive, so the wait is still
  // bounded and callers must not defer anything on it.
  auto s = recovering_sample(100, 99);
  s.heartbeat_valid = false;

  const auto d = live.evaluate(s, at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::rebuilding);
  EXPECT_FALSE(live.recovering(s, at(0s)));

  EXPECT_EQ(live.evaluate(s, at(live.config().recovery_budget + 1s)).verdict,
            vgd_ring_verdict_e::reinit);
}

// --- GPU-reset edge: deferred while recovering, immediate otherwise --------

TEST(VgdRingLiveness, GpuResetEdgeReinitsImmediatelyOnAnActiveRing) {
  vgd_ring_liveness_t live;

  auto s = healthy();
  s.tdr_edge = true;

  const auto d = live.evaluate(s, at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::gpu_reset);
}

TEST(VgdRingLiveness, GpuResetEdgeReinitsImmediatelyWhenTheHeartbeatIsStale) {
  // Pre-build-16 drivers depart the monitor on a TDR: the ring stops being
  // updated and the heartbeat goes stale. The reinit must NOT be delayed by
  // the stale-heartbeat grace window — it fires on the first sample, as today.
  vgd_ring_liveness_t live;

  auto s = recovering_sample(100, 99);
  s.heartbeat_age = 30s;
  s.tdr_edge = true;

  const auto d = live.evaluate(s, at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::gpu_reset);
}

TEST(VgdRingLiveness, GpuResetEdgeIsDeferredWhileRecoveringThenTaken) {
  vgd_ring_liveness_t live;

  auto ducked = recovering_sample(100, 99);
  ducked.tdr_edge = true;

  // Two minutes of REBUILDING with a live heartbeat: hold the capture.
  for (auto t = 0s; t < 2min; t += 5s) {
    const auto d = live.evaluate(ducked, at(t));
    ASSERT_EQ(d.verdict, vgd_ring_verdict_e::wait) << "gave up at " << t.count() << "s";
    ASSERT_EQ(d.reason, vgd_ring_reason_e::recovering);
  }

  // The GPU comes back and the driver republishes under a new generation. The
  // deferred reinit is taken now — when a fresh device can actually be built.
  auto back = healthy(140);
  back.generation = 8;
  back.delivered_sequence = 99;
  back.tdr_edge = true;

  const auto d = live.evaluate(back, at(2min));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::gpu_reset);
}

// --- Delivery-stall circuit breaker ---------------------------------------

TEST(VgdRingLiveness, DeliveryStallStillTripsOnAGenuinelyBrokenReader) {
  vgd_ring_liveness_t live;

  // The driver published seq 101; we never manage to claim it.
  auto s = healthy(101);
  s.delivered_sequence = 100;

  EXPECT_EQ(live.evaluate(s, at(0s)).verdict, vgd_ring_verdict_e::consume);
  EXPECT_EQ(live.evaluate(s, at(4s)).verdict, vgd_ring_verdict_e::consume);

  const auto broken = live.evaluate(s, at(6s));
  EXPECT_EQ(broken.verdict, vgd_ring_verdict_e::broken);
  EXPECT_EQ(broken.reason, vgd_ring_reason_e::delivery_stall);
}

TEST(VgdRingLiveness, DeliveringAFrameClearsTheStallEvidence) {
  vgd_ring_liveness_t live;

  auto s = healthy(101);
  s.delivered_sequence = 100;
  EXPECT_EQ(live.evaluate(s, at(0s)).verdict, vgd_ring_verdict_e::consume);

  live.note_delivered();
  s.delivered_sequence = 101;

  EXPECT_EQ(live.evaluate(s, at(30s)).verdict, vgd_ring_verdict_e::consume);
}

/**
 * The regression this whole change exists for.
 *
 * Before the fix the stall timer was armed while the ring was ACTIVE, and the
 * REBUILDING branch returned early past BOTH the check and its reset — so the
 * timer survived the entire outage and the first healthy sample afterwards
 * instantly exceeded the 5 s stall window, permanently blacklisting ring
 * capture for the session. The driver's rebuild frees every slot without
 * rewinding latest_sequence, so the precondition (an undelivered sequence that
 * can never be claimed) is guaranteed, not incidental.
 */
TEST(VgdRingLiveness, DeliveryStallLatchCannotSurviveARebuild) {
  vgd_ring_liveness_t live;

  // Driver published seq 101, we last delivered 100 — the timer arms.
  auto pre = healthy(101);
  pre.delivered_sequence = 100;
  ASSERT_EQ(live.evaluate(pre, at(0s)).verdict, vgd_ring_verdict_e::consume);

  // GPU outage: two minutes of REBUILDING with a live heartbeat.
  const auto ducked = recovering_sample(101, 100);
  for (auto t = 1s; t < 2min; t += 5s) {
    ASSERT_EQ(live.evaluate(ducked, at(t)).reason, vgd_ring_reason_e::recovering);
  }

  // Recovery completes under a new generation. Seq 101 is unclaimable forever
  // (its slot was freed), and on an idle desktop the driver publishes nothing
  // new — latest_sequence stays 101 while delivered stays 100.
  auto post = healthy(101);
  post.generation = 8;
  post.delivered_sequence = 100;

  EXPECT_EQ(live.evaluate(post, at(2min)).verdict, vgd_ring_verdict_e::consume);
  EXPECT_EQ(live.evaluate(post, at(2min + 30s)).verdict, vgd_ring_verdict_e::consume)
    << "a retired pre-rebuild sequence must never blacklist the session";
  EXPECT_EQ(live.retired_sequence(), 101u);
}

TEST(VgdRingLiveness, StallDetectionReArmsAfterARebuild) {
  vgd_ring_liveness_t live;

  ASSERT_EQ(live.evaluate(healthy(100), at(0s)).verdict, vgd_ring_verdict_e::consume);
  ASSERT_EQ(live.evaluate(recovering_sample(101, 100), at(1s)).reason, vgd_ring_reason_e::recovering);

  // New generation retires seq 101...
  auto post = healthy(101);
  post.generation = 8;
  post.delivered_sequence = 100;
  ASSERT_EQ(live.evaluate(post, at(2s)).verdict, vgd_ring_verdict_e::consume);

  // ...but a frame published UNDER the new generation is real evidence again.
  post.latest_sequence = 102;
  EXPECT_EQ(live.evaluate(post, at(3s)).verdict, vgd_ring_verdict_e::consume);
  const auto broken = live.evaluate(post, at(9s));
  EXPECT_EQ(broken.verdict, vgd_ring_verdict_e::broken);
  EXPECT_EQ(broken.reason, vgd_ring_reason_e::delivery_stall);
}

TEST(VgdRingLiveness, FirstObservationDoesNotRetireAnything) {
  // Capture can open on a ring that already has a frame published. That frame
  // IS claimable, so a reader that never manages it must still be caught.
  vgd_ring_liveness_t live;

  auto s = healthy(500);
  s.delivered_sequence = 0;

  EXPECT_EQ(live.evaluate(s, at(0s)).verdict, vgd_ring_verdict_e::consume);
  EXPECT_EQ(live.retired_sequence(), 0u);
  EXPECT_EQ(live.evaluate(s, at(6s)).verdict, vgd_ring_verdict_e::broken);
}

// --- Housekeeping ----------------------------------------------------------

TEST(VgdRingLiveness, ReasonIsReportedOnlyOnTransitions) {
  vgd_ring_liveness_t live;
  const auto s = recovering_sample(100, 99);

  EXPECT_TRUE(live.evaluate(s, at(0s)).first_report);
  EXPECT_FALSE(live.evaluate(s, at(1s)).first_report);
  EXPECT_FALSE(live.evaluate(s, at(60s)).first_report);

  // Back to healthy: a new reason, so a new report.
  EXPECT_TRUE(live.evaluate(healthy(), at(61s)).first_report);
}

/**
 * The capture loop's stale-DXGI-factory check defers on recovering() ALONE and
 * never reaches evaluate() while it is deferring. If recovering() did not share
 * (and start) the one recovery clock, that deferral would be unbounded — the
 * exact "waiting longer became hang forever" failure the change must not
 * introduce.
 */
TEST(VgdRingLiveness, RecoveringQueryIsSelfBounding) {
  vgd_ring_liveness_t live;
  const auto s = recovering_sample(100, 99);

  // First ask starts the clock.
  EXPECT_TRUE(live.recovering(s, at(0s)));
  EXPECT_TRUE(live.recovering(s, at(5min)));
  EXPECT_TRUE(live.recovering(s, at(10min)));
  EXPECT_EQ(live.rebuilding_for(at(10min)), 10min);

  // ...and stops it, with no help from evaluate().
  EXPECT_FALSE(live.recovering(s, at(live.config().recovery_budget + 1s)));
}

TEST(VgdRingLiveness, RecoveryClockIsSharedBetweenBothEntryPoints) {
  vgd_ring_liveness_t live;
  const auto s = recovering_sample(100, 99);

  // Clock started by the query...
  EXPECT_TRUE(live.recovering(s, at(0s)));
  // ...is the one the ladder honours: no restart, no doubled budget.
  EXPECT_EQ(live.evaluate(s, at(10min)).reason, vgd_ring_reason_e::recovering);
  EXPECT_EQ(live.evaluate(s, at(live.config().recovery_budget + 1s)).reason,
            vgd_ring_reason_e::recovery_budget_expired);
}

TEST(VgdRingLiveness, RebuildTimerResetsBetweenSeparateOutages) {
  vgd_ring_liveness_t live;
  const auto ducked = recovering_sample(100, 100);

  ASSERT_EQ(live.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_EQ(live.evaluate(ducked, at(9min)).reason, vgd_ring_reason_e::recovering);

  // Recovered.
  ASSERT_EQ(live.evaluate(healthy(), at(9min + 1s)).verdict, vgd_ring_verdict_e::consume);

  // A second outage half an hour later gets a full budget of its own, not the
  // remains of the first one's.
  EXPECT_EQ(live.evaluate(ducked, at(40min)).reason, vgd_ring_reason_e::recovering);
  EXPECT_EQ(live.evaluate(ducked, at(48min)).reason, vgd_ring_reason_e::recovering);
}

TEST(VgdRingLiveness, ConfigIsHonoured) {
  vgd_ring_liveness_config_t cfg;
  cfg.recovery_budget = 30s;
  cfg.delivery_stall = 1s;
  vgd_ring_liveness_t live {cfg};

  const auto ducked = recovering_sample(100, 99);
  EXPECT_EQ(live.evaluate(ducked, at(0s)).verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(live.evaluate(ducked, at(31s)).reason, vgd_ring_reason_e::recovery_budget_expired);
}

// --- A wait verdict must honour the caller's timeout -----------------------

/**
 * The base capture loop reads `capture_e::timeout` as "nothing this round" and
 * calls straight back after a 10 ms nap. A wait verdict that returns instantly
 * therefore free-runs the capture thread at ~100 Hz for the WHOLE multi-minute
 * recovery — a core burnt in a SYSTEM service to re-read a header the driver
 * touches four times a second.
 */
TEST(VgdRingLiveness, WaitVerdictConsumesTheCallersWholeTimeout) {
  std::chrono::nanoseconds waited {0};
  int slices = 0;

  while (true) {
    const auto slice = platf::dxgi::vgd_wait_slice(200ms, waited);
    if (slice <= 0ns) {
      break;
    }
    ASSERT_LE(slice, 5ms) << "too long a slice delays noticing that the ring came back";
    waited += slice;
    ASSERT_LT(++slices, 1000) << "wait slicing does not terminate";
  }

  EXPECT_EQ(waited, 200ms) << "a wait verdict must spend the caller's timeout, not return instantly";
  EXPECT_EQ(slices, 40) << "the ring is sampled at the caller's rate, not free-run";
}

TEST(VgdRingLiveness, WaitVerdictWithAZeroTimeoutStaysNonBlocking) {
  // The base loop's frame-pacing path calls snapshot() with timeout 0 and must
  // keep getting an immediate answer.
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(0ms, 0ns), 0ns);
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(0ms, 5ms), 0ns);
}

TEST(VgdRingLiveness, WaitSliceNeverOverrunsTheCallersTimeout) {
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(3ms, 0ns), 3ms);
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(200ms, 198ms), 2ms);
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(200ms, 200ms), 0ns);
  EXPECT_EQ(platf::dxgi::vgd_wait_slice(200ms, 500ms), 0ns);
}

// --- The recovery budget is AGGREGATE, not per reader ---------------------

/**
 * The ladder lives on the ring reader, and every capture reinit destroys and
 * rebuilds that reader. A per-object budget therefore restarts on each reinit,
 * so the wait that actually matters — rebuild, spend the budget, reinit, spend
 * it again — never ends. The owner carries the clock across instances.
 */
TEST(VgdRingLiveness, RecoveryBudgetIsAggregateAcrossReaderInstances) {
  const auto ducked = recovering_sample(100, 99);

  vgd_ring_liveness_t first;
  ASSERT_EQ(first.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_EQ(first.evaluate(ducked, at(10min)).reason, vgd_ring_reason_e::recovering);

  const auto carried = first.recovery_start();
  ASSERT_TRUE(carried.has_value()) << "an in-progress outage must expose its clock";

  // The reinit rebuilt the reader; the replacement adopts the running clock. The
  // handoff is immediate, which is what makes the clock a continuation.
  vgd_ring_liveness_t second;
  second.adopt_recovery_start(carried, at(10min));

  const auto expired = second.evaluate(ducked, at(first.config().recovery_budget + 1s));
  EXPECT_EQ(expired.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(expired.reason, vgd_ring_reason_e::recovery_budget_expired)
    << "a rebuilt reader inherited a fresh budget, so the aggregate wait is unbounded";
}

TEST(VgdRingLiveness, TheRecoveryClockIsClearedWhenTheOutageEnds) {
  vgd_ring_liveness_t live;

  ASSERT_EQ(live.evaluate(recovering_sample(100, 99), at(0s)).reason, vgd_ring_reason_e::recovering);
  EXPECT_TRUE(live.recovery_start().has_value());

  ASSERT_EQ(live.evaluate(healthy(), at(1s)).verdict, vgd_ring_verdict_e::consume);
  EXPECT_FALSE(live.recovery_start().has_value())
    << "a finished outage must not hand its spent budget to the next one";
}

TEST(VgdRingLiveness, ResettingTheClockIsExplicitAndDistinctFromAdoptingNothing) {
  // The owner abandons the clock when an exhausted budget hands the session to
  // the fallback path; without a way to say so, the next publish would write
  // the spent clock straight back into the store it was just cleared from.
  vgd_ring_liveness_t live;
  const auto ducked = recovering_sample(100, 99);

  ASSERT_EQ(live.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_TRUE(live.recovery_start().has_value());

  live.reset_recovery_clock();
  EXPECT_FALSE(live.recovery_start().has_value());

  // And the next outage gets a full budget, not the remains of the abandoned one.
  EXPECT_EQ(live.evaluate(ducked, at(30min)).reason, vgd_ring_reason_e::recovering);
  EXPECT_EQ(live.evaluate(ducked, at(40min)).reason, vgd_ring_reason_e::recovering);
}

TEST(VgdRingLiveness, AdoptingAnEmptyClockLeavesARunningOneAlone) {
  vgd_ring_liveness_t live;
  const auto ducked = recovering_sample(100, 99);

  ASSERT_EQ(live.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  live.adopt_recovery_start(std::nullopt, at(1s));

  EXPECT_EQ(live.evaluate(ducked, at(10min)).reason, vgd_ring_reason_e::recovering);
  EXPECT_EQ(live.evaluate(ducked, at(live.config().recovery_budget + 1s)).reason,
            vgd_ring_reason_e::recovery_budget_expired);
}

// --- A spent clock must never be charged to a ring that did not spend it ---

/**
 * The regression these pin.
 *
 * An expired recovery budget is not an ordinary reinit: it stores the ring's
 * session id in g_broken_session (display_vgd.cpp), which refuses ring capture
 * for the REST OF THE PROCESS and cannot be cleared. That is the right answer
 * for a ring that really has been rebuilding past the point the driver's own
 * budget should have given up — and a catastrophic one for a healthy ring.
 *
 * The clock is what decides which of those a reader is looking at, and it is
 * deliberately persisted across reader instances so one outage gets one budget.
 * So it has to stop being persisted the moment the outage ends, and it has to
 * stop being TRUSTED once nobody has been watching the ring — otherwise the
 * ordinary ~10 ms REBUILDING blip of a mode change, minutes or hours later, is
 * measured against a budget that ran out long ago and takes the ring out
 * permanently.
 */
TEST(VgdRingLiveness, RecoveryEndingClearsTheClockEvenWhenTheVerdictIsAReinit) {
  // The documented happy path: the outage ends, the ring goes ACTIVE, and the
  // GPU-reset edge deferred through the whole recovery is finally taken. That
  // reinit destroys the reader — so if it leaves the clock running, the clock
  // outlives the outage it measured.
  vgd_ring_liveness_t live;

  auto ducked = recovering_sample(100, 99);
  ducked.tdr_edge = true;
  ASSERT_EQ(live.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_TRUE(live.recovery_start().has_value());

  auto back = healthy(140);
  back.generation = 8;
  back.delivered_sequence = 99;
  back.tdr_edge = true;

  const auto d = live.evaluate(back, at(30s));
  ASSERT_EQ(d.reason, vgd_ring_reason_e::gpu_reset);
  EXPECT_FALSE(live.recovery_start().has_value())
    << "a recovered ring handed its spent clock to whatever reader comes next";
}

TEST(VgdRingLiveness, EveryReinitPathClearsTheClockOnARingThatStoppedRebuilding) {
  // Each of these returns before the ladder's old clearing point, so each one
  // could strand a running clock in the owner's store.
  const auto dead_ring = [] {
    auto s = healthy();
    s.state = vgd_ring_state::dead;
    return s;
  }();
  const auto stale_beat = [] {
    auto s = healthy();
    s.heartbeat_age = 30s;
    return s;
  }();

  for (const auto &after : {dead_ring, stale_beat}) {
    vgd_ring_liveness_t live;
    ASSERT_EQ(live.evaluate(recovering_sample(100, 99), at(0s)).reason, vgd_ring_reason_e::recovering);
    ASSERT_TRUE(live.recovery_start().has_value());

    (void) live.evaluate(after, at(30s));
    EXPECT_FALSE(live.recovery_start().has_value())
      << "a ring that stopped REBUILDING left its clock running";
  }
}

TEST(VgdRingLiveness, TheRecoveringQueryAlsoEndsTheOutage) {
  // is_recovering() can be the ONLY entry point a deferring capture loop
  // reaches, so it has to be able to end an outage as well as start one.
  vgd_ring_liveness_t live;

  ASSERT_TRUE(live.recovering(recovering_sample(100, 99), at(0s)));
  ASSERT_TRUE(live.recovery_start().has_value());

  EXPECT_FALSE(live.recovering(healthy(), at(1s)));
  EXPECT_FALSE(live.recovery_start().has_value())
    << "the query started the clock but could never stop it";
}

TEST(VgdRingLiveness, AStaleClockIsNotAdoptedAndCannotLatchAHealthyRing) {
  // A reader torn down mid-rebuild leaves its clock in the owner's session-keyed
  // store. If ring capture is not attempted again for a while — the session fell
  // back to WGC/DDA, the monitor was unplugged, the stream ended and restarted —
  // nobody has been watching that ring, and the clock is no longer evidence of
  // anything.
  vgd_ring_liveness_t stranded;
  ASSERT_EQ(stranded.evaluate(recovering_sample(100, 99), at(0s)).reason, vgd_ring_reason_e::recovering);
  const auto leftover = stranded.recovery_start();
  ASSERT_TRUE(leftover.has_value());

  // An hour later a fresh reader opens a perfectly healthy ring.
  vgd_ring_liveness_t live;
  live.adopt_recovery_start(leftover, at(1h));
  EXPECT_FALSE(live.recovery_start().has_value())
    << "a clock nobody has confirmed for an hour was adopted as an outage in progress";

  // Capture opens mid-blip: the ring's first observed state is an ordinary
  // ~10 ms REBUILDING — a mode change, a swapchain reassignment. Measured
  // against the adopted clock that is the whole budget gone at once, and
  // recovery_budget_expired is not an ordinary reinit: it stores the session in
  // g_broken_session, which refuses ring capture for the rest of the process and
  // cannot be cleared.
  const auto blip = live.evaluate(recovering_sample(100, 100), at(1h));
  EXPECT_EQ(blip.verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(blip.reason, vgd_ring_reason_e::recovering)
    << "a healthy ring was charged for an outage it never had and blacklisted for the session";

  // And the budget it did get is a full one, measured from the blip.
  EXPECT_EQ(live.evaluate(recovering_sample(100, 100), at(1h + 10min)).reason,
            vgd_ring_reason_e::recovering);

  // The blip ends; a healthy ring is consumed and the clock goes with it.
  EXPECT_EQ(live.evaluate(healthy(), at(1h + 10min + 1s)).verdict, vgd_ring_verdict_e::consume);
  EXPECT_FALSE(live.recovery_start().has_value());
}

TEST(VgdRingLiveness, AClockConfirmedWithinTheHandoffGraceIsStillAdopted) {
  // The aggregate budget's whole job: a reinit destroys and rebuilds the reader
  // MID-outage, and the replacement must not get a fresh budget. The handoff
  // takes a moment (destroy the reader, rebuild it, reopen the ring), so the
  // grace has to cover it comfortably.
  vgd_ring_liveness_t first;
  const auto ducked = recovering_sample(100, 99);
  ASSERT_EQ(first.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_EQ(first.evaluate(ducked, at(10min)).reason, vgd_ring_reason_e::recovering);

  vgd_ring_liveness_t second;
  second.adopt_recovery_start(first.recovery_start(), at(10min + 2s));
  ASSERT_TRUE(second.recovery_start().has_value()) << "a live handoff was refused";

  const auto expired = second.evaluate(ducked, at(second.config().recovery_budget + 1s));
  EXPECT_EQ(expired.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(expired.reason, vgd_ring_reason_e::recovery_budget_expired)
    << "a rebuilt reader inherited a fresh budget, so the aggregate wait is unbounded";
}

TEST(VgdRingLiveness, TheHandoffClockCarriesItsLastConfirmationNotJustItsStart) {
  // The staleness test is against the last CONFIRMATION, not the start —
  // otherwise a genuine ten-minute outage would fail its own handoff.
  vgd_ring_liveness_t live;
  const auto ducked = recovering_sample(100, 99);

  ASSERT_EQ(live.evaluate(ducked, at(0s)).reason, vgd_ring_reason_e::recovering);
  ASSERT_EQ(live.evaluate(ducked, at(10min)).reason, vgd_ring_reason_e::recovering);

  const auto clock = live.recovery_start();
  ASSERT_TRUE(clock.has_value());
  EXPECT_EQ(clock->started, at(0s));
  EXPECT_EQ(clock->last_seen, at(10min));
}

// --- Drivers that never duck in place keep today's timeline exactly --------

/**
 * A pre-build-16 driver departs its monitor on a TDR and its heartbeat simply
 * STOPS. For the 2 s staleness threshold that is indistinguishable from build
 * 16's still-beating one, so reading REBUILDING as recovering would buy those
 * drivers up to 2 s of deferral they never had — and the stale-DXGI-factory
 * reinit is exactly where that shows up as a behaviour change. The owner
 * vouches for the driver from its handshake build; nothing else can.
 */
TEST(VgdRingLiveness, AnUnvouchedDriverIsNeverReportedRecovering) {
  vgd_ring_liveness_config_t cfg;
  cfg.trust_rebuilding_heartbeat = false;
  vgd_ring_liveness_t live {cfg};

  const auto s = recovering_sample(100, 99);
  const auto d = live.evaluate(s, at(0s));

  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::wait);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::rebuilding)
    << "REBUILDING on an unvouched driver is the old plain wait, not recovery";
  EXPECT_FALSE(live.recovering(s, at(0s)))
    << "nothing may defer on a driver that does not duck in place";
}

TEST(VgdRingLiveness, AnUnvouchedDriverKeepsTodaysHeartbeatTimeline) {
  vgd_ring_liveness_config_t cfg;
  cfg.trust_rebuilding_heartbeat = false;
  vgd_ring_liveness_t live {cfg};

  auto s = recovering_sample(100, 99);
  s.heartbeat_age = 3s;  // past the 2 s stale threshold

  EXPECT_EQ(live.evaluate(s, at(0s)).reason, vgd_ring_reason_e::heartbeat_stale);
  EXPECT_EQ(live.evaluate(s, at(9s)).verdict, vgd_ring_verdict_e::wait);

  const auto lost = live.evaluate(s, at(11s));
  EXPECT_EQ(lost.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(lost.reason, vgd_ring_reason_e::heartbeat_lost);
}

TEST(VgdRingLiveness, AnUnvouchedDriverStillReinitsImmediatelyOnAGpuResetEdge) {
  // With a vouched driver this same sample defers the reinit (see
  // GpuResetEdgeIsDeferredWhileRecoveringThenTaken). Without one it must fire
  // on the first sample, which is what pre-build-16 drivers have always done.
  vgd_ring_liveness_config_t cfg;
  cfg.trust_rebuilding_heartbeat = false;
  vgd_ring_liveness_t live {cfg};

  auto s = recovering_sample(100, 99);
  s.tdr_edge = true;

  const auto d = live.evaluate(s, at(0s));
  EXPECT_EQ(d.verdict, vgd_ring_verdict_e::reinit);
  EXPECT_EQ(d.reason, vgd_ring_reason_e::gpu_reset);
}

TEST(VgdRingLiveness, VouchingIsTheDefaultBecauseThisClassExistsForBuild16) {
  vgd_ring_liveness_config_t cfg;
  EXPECT_TRUE(cfg.trust_rebuilding_heartbeat);
}
