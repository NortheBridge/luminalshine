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
