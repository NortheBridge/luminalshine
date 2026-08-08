/**
 * @file src/platform/windows/vgd_ring_liveness.h
 * @brief Pure decision logic for "is the LuminalVGD frame ring alive, recovering, or dead?"
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>

namespace platf::dxgi {

  /// proto `ring_state::*` — mirrors VgdRingStatus::state (luminal_vgd.h).
  namespace vgd_ring_state {
    inline constexpr uint32_t uninitialized = 0;
    inline constexpr uint32_t active = 1;
    inline constexpr uint32_t rebuilding = 2;
    inline constexpr uint32_t dead = 3;
  }  // namespace vgd_ring_state

  /// Result of validating the authenticated ring identity imported by an
  /// isolated video worker. `provisional` is the driver's documented
  /// pre-first-surface state: generation exists, but its synchronization
  /// transport has not been published yet.
  enum class vgd_worker_ring_binding_e {
    reject,
    ready,
    provisional,
  };

  constexpr vgd_worker_ring_binding_e classify_worker_ring_binding(
    int32_t status_result,
    uint32_t state,
    uint32_t generation,
    uint64_t latest_sequence,
    uint32_t transport_flags,
    uint32_t expected_generation,
    uint32_t expected_transport_flags
  ) {
    if (status_result != 0 ||
        (state != vgd_ring_state::active && state != vgd_ring_state::rebuilding) ||
        generation == 0 ||
        (expected_generation != 0 && generation != expected_generation)) {
      return vgd_worker_ring_binding_e::reject;
    }
    if (state == vgd_ring_state::rebuilding &&
        latest_sequence == 0 && transport_flags == 0) {
      return vgd_worker_ring_binding_e::provisional;
    }
    return state == vgd_ring_state::active && latest_sequence != 0 &&
             transport_flags == expected_transport_flags ?
             vgd_worker_ring_binding_e::ready :
             vgd_worker_ring_binding_e::reject;
  }

  /// What the ring reader should do with the current observation.
  enum class vgd_ring_verdict_e {
    consume,  ///< Healthy: go claim a frame.
    wait,  ///< Transient: return capture_e::timeout and look again.
    reinit,  ///< Tear the capture down and rebuild it (today's failure handling).
    broken,  ///< The READER side is broken: blacklist the session, fall back to WGC/DDA.
  };

  /// Why the verdict was reached (logging + tests; never affects control flow).
  enum class vgd_ring_reason_e {
    healthy,
    ring_dead,  ///< Driver marked the ring DEAD.
    heartbeat_stale,  ///< Heartbeat behind the stale threshold, inside the grace window.
    heartbeat_lost,  ///< Heartbeat stale past the grace window — the driver is gone.
    recovering,  ///< REBUILDING with a LIVE heartbeat: a GPU outage the driver is riding out.
    rebuilding,  ///< REBUILDING without a usable heartbeat (pre-heartbeat driver).
    recovery_budget_expired,  ///< REBUILDING for longer than the host is willing to wait.
    gpu_reset,  ///< A GPU-reset-class event was recorded; rebuild against a fresh device.
    delivery_stall,  ///< Driver keeps publishing frames this reader can never claim.
  };

  /// One observation of the ring header plus the reader's own context.
  struct vgd_ring_sample_t {
    /// VgdRingStatus::state (vgd_ring_state::*).
    uint32_t state = vgd_ring_state::uninitialized;
    /// VgdRingStatus::generation — bumped whenever the driver retires the slots.
    uint32_t generation = 0;
    /// VgdRingStatus::latest_sequence — newest sequence the DRIVER has published.
    uint64_t latest_sequence = 0;
    /// Newest sequence this reader has actually delivered to the encoder.
    uint64_t delivered_sequence = 0;
    /// False when the driver publishes no usable heartbeat (heartbeat_qpc or
    /// qpc_frequency zero). Liveness then cannot be proven, only assumed.
    bool heartbeat_valid = false;
    /// How far the heartbeat is behind now. Meaningful only when valid.
    std::chrono::nanoseconds heartbeat_age {0};
    /// A GPU-reset-class event has been recorded since this reader opened.
    bool tdr_edge = false;
  };

  struct vgd_ring_decision_t {
    vgd_ring_verdict_e verdict = vgd_ring_verdict_e::consume;
    vgd_ring_reason_e reason = vgd_ring_reason_e::healthy;
    /// True when `reason` differs from the previous decision's — callers log on
    /// the edge only, so a multi-minute wait does not flood the log.
    bool first_report = false;
  };

  /**
   * @brief One outage's recovery clock, as it crosses reader instances.
   *
   * `started` is what the budget is measured from. `last_seen` is when a reader
   * last CONFIRMED that the ring was still rebuilding, and it is what makes the
   * handoff safe: a clock is only evidence of an outage that is still in
   * progress for as long as somebody has been watching. Once nobody is — the
   * reader was torn down and ring capture was not re-attempted for a while —
   * the clock says nothing about the ring a later reader opens, and adopting it
   * would spend a budget that ring never used. See adopt_recovery_start().
   */
  struct vgd_recovery_clock_t {
    std::chrono::steady_clock::time_point started {};
    std::chrono::steady_clock::time_point last_seen {};

    friend bool operator==(const vgd_recovery_clock_t &, const vgd_recovery_clock_t &) = default;
  };

  struct vgd_ring_liveness_config_t {
    /// The driver heartbeats the ring header far more often than this even when
    /// no frames flow (build >= 16 ticks it every 250 ms straight through a GPU
    /// outage); several missed beats means the worker is stopped.
    std::chrono::nanoseconds heartbeat_stale {std::chrono::milliseconds(2000)};

    /// How long a stale heartbeat is tolerated before reinitializing. Mode
    /// transitions unassign the swapchain (worker stops, heartbeat halts) for a
    /// few seconds; only a persistently silent ring is worth a capture teardown.
    std::chrono::nanoseconds heartbeat_reinit_grace {std::chrono::seconds(10)};

    /// If the driver is publishing frames but none reach us for this long, the
    /// reader side is broken (claim CAS or texture path).
    std::chrono::nanoseconds delivery_stall {std::chrono::seconds(5)};

    /// Ceiling on "REBUILDING with a live heartbeat". Deliberately ABOVE the
    /// driver's own TDR_RECOVERY_BUDGET (10 min, control.rs) so the driver's
    /// give-up — which marks the ring dead / departs the monitor and is
    /// therefore detected through the normal ladder — always lands first. This
    /// bound exists so a driver that heartbeats forever without ever finishing
    /// its rebuild cannot freeze the capture indefinitely.
    ///
    /// The budget is AGGREGATE across reader instances, not per instance: the
    /// owner persists the running clock and seeds it into the next reader with
    /// adopt_recovery_start(). Without that, every capture reinit would hand a
    /// fresh budget to a fresh object and the real wait — rebuild, spend the
    /// budget, reinit, spend it again — would be unbounded.
    std::chrono::nanoseconds recovery_budget {std::chrono::minutes(11)};

    /// How long a persisted recovery clock may go unconfirmed and still be
    /// adopted by the next reader.
    ///
    /// The aggregate budget above is only sound while the clock describes ONE
    /// CONTINUOUS outage. A reader torn down mid-rebuild leaves its clock in the
    /// owner's store with nobody watching the ring, and ring capture may not be
    /// attempted again for a long time (the session falls back to WGC/DDA, the
    /// monitor is unplugged, the stream ends and restarts). A clock adopted
    /// after such a gap is not a continuation of anything: the ring it lands on
    /// may be perfectly healthy, and its first ordinary REBUILDING blip — a mode
    /// change, a swapchain reassignment, ~10 ms — would then be judged against a
    /// budget that has long since run out, reinit with recovery_budget_expired
    /// and PERMANENTLY blacklist the ring (g_broken_session, display_vgd.cpp, is
    /// unclearable for the life of the process).
    ///
    /// A genuine outage re-confirms the clock at the capture loop's rate for as
    /// long as it lasts, and the owner republishes it once a second, so a real
    /// handoff — destroy the reader, rebuild it, open the ring — never comes
    /// close to this. It is a staleness test, not a wait.
    std::chrono::nanoseconds recovery_clock_handoff_grace {std::chrono::seconds(30)};

    /// Whether "REBUILDING with a live heartbeat" may be read as RECOVERING at
    /// all.
    ///
    /// Only driver build >= 16 keeps its IddCx monitor arrived and keeps
    /// heartbeating through a GPU outage. Every older driver departs the
    /// monitor and its heartbeat simply STOPS — and a stopped heartbeat is
    /// indistinguishable from a beating one for up to `heartbeat_stale` (2 s),
    /// because "age" is all the ring header exposes. Leaving this OFF for those
    /// drivers is what keeps them bit-identical to pre-change behaviour: the
    /// ladder never reports `recovering`, so no caller anywhere defers anything,
    /// not even for those 2 s. The owner sets it from the handshake's
    /// driver_build (see display_vgd.cpp); it defaults on because this class
    /// exists for build 16.
    bool trust_rebuilding_heartbeat = true;
  };

  /**
   * @brief How long a `wait` verdict may sleep before looking at the ring again.
   *
   * A `wait` that returns immediately is not free. The base capture loop reads
   * `capture_e::timeout` as "nothing this round" and calls straight back after a
   * 10 ms nap, so an instant return free-runs the capture thread at ~100 Hz for
   * the whole (multi-minute) recovery — a core burnt in a SYSTEM service to
   * re-read a header the driver touches four times a second. Honouring the
   * caller's timeout, exactly as the frame-claim path below already does, puts
   * that back at the rate the caller asked for.
   *
   * Sliced rather than slept in one go so a ring that comes back mid-wait is
   * noticed within a slice instead of at the end of the caller's whole budget:
   * the recovery-detection latency stays what it was.
   *
   * @param caller_timeout The timeout snapshot() was called with. Zero (the
   *        frame-pacing path) must stay non-blocking.
   * @param already_waited How much of it has been spent.
   * @param max_slice Longest single sleep.
   * @return The next sleep, or zero when the budget is spent.
   */
  [[nodiscard]] inline std::chrono::nanoseconds vgd_wait_slice(
    std::chrono::nanoseconds caller_timeout,
    std::chrono::nanoseconds already_waited,
    std::chrono::nanoseconds max_slice = std::chrono::milliseconds(5)
  ) {
    const auto remaining = caller_timeout - already_waited;
    if (remaining <= std::chrono::nanoseconds::zero()) {
      return std::chrono::nanoseconds::zero();
    }
    return remaining < max_slice ? remaining : max_slice;
  }

  /**
   * @brief State machine deciding whether a LuminalVGD frame ring is healthy,
   * recovering, or dead.
   *
   * Motivation (driver build 16): on a TDR the driver no longer departs its
   * IddCx monitor. It tears down the D3D device and swapchain, marks the ring
   * REBUILDING, and keeps ticking a device-independent heartbeat while it waits
   * for the GPU to come back — for MINUTES. Before build 16 a ring sat in
   * REBUILDING for ~10 ms (a swapchain reassignment blip), so the host could
   * treat "REBUILDING" and "no frames" as interchangeable with "briefly busy".
   * It no longer can: the host must read REBUILDING **with a live heartbeat**
   * as RECOVERING and keep the session, and only a stale/absent heartbeat as
   * dead. Getting that wrong converts a recoverable GPU outage into a host-side
   * teardown, which is the exact amplification build 16 exists to remove.
   *
   * Ordering is load-bearing:
   *  1. DEAD is always terminal.
   *  2. RECOVERING (REBUILDING + live heartbeat + inside the budget) short-
   *     circuits everything below it, including a pending GPU-reset reinit.
   *     Deferring that reinit — rather than cancelling it — is what keeps the
   *     capture pipeline in its cheap timeout loop instead of spinning the
   *     capture-reinit machinery while the GPU is still down.
   *
   *     Note the encoder is SILENT for the whole hold: it attempts exactly one
   *     encode per rebuild, and once that fails it parks rather than re-entering
   *     the failed NVENC session (which NVENC's contract forbids — re-entry is
   *     the known heap-corruption path). So the client hits its own no-video
   *     timeout and reconnects when the display returns. That is the same thing
   *     it does today; what changed is that the HOST now survives, instead of
   *     tearing down into an unrecoverable session.
   *  3. Everything else runs on exactly today's timeline: a GPU-reset edge
   *     reinitializes immediately, a stale heartbeat reinitializes after its
   *     grace window, and a genuinely undeliverable ring is declared broken.
   *
   * Two properties keep "wait longer" from becoming "wait forever":
   *  - The recovery budget is AGGREGATE, not per instance. Every capture reinit
   *    destroys and rebuilds the reader that owns this object, so the owner
   *    persists the running clock and seeds the next one through
   *    adopt_recovery_start()/recovery_start(). One outage gets one budget —
   *    and, symmetrically, ONE OUTAGE ONLY: the clock is cleared the moment the
   *    ring is observed not rebuilding, and a clock nobody has confirmed
   *    recently is not adopted at all, so a ring can never be charged for an
   *    outage that is over or that was never its own.
   *  - The discriminator only runs for drivers the owner has vouched for
   *    (config.trust_rebuilding_heartbeat). A pre-build-16 driver departs its
   *    monitor and its heartbeat STOPS, which for `heartbeat_stale` (2 s) looks
   *    exactly like build 16's beating one — so those drivers skip the branch
   *    entirely and keep their pre-change timeline to the bit.
   *
   * A generation bump retires every sequence published under the previous
   * generation: the driver's rebuild frees all slots without rewinding
   * latest_sequence, so a frame published just before the rebuild is unclaimable
   * forever. Counting it as "undelivered" would declare the reader broken the
   * instant the ring came back.
   *
   * Purely functional over its inputs (no clock access, no I/O, no logging) so
   * it can be unit-tested deterministically. Not thread-safe: the ring reader
   * touches it only from the capture thread.
   */
  class vgd_ring_liveness_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    vgd_ring_liveness_t() = default;

    explicit vgd_ring_liveness_t(vgd_ring_liveness_config_t config):
        _config(config) {
    }

    /**
     * @brief Feed one ring observation and get the resulting decision.
     * @param sample The ring header snapshot plus reader context.
     * @param now Timestamp of the sample (monotonic).
     */
    [[nodiscard]] vgd_ring_decision_t evaluate(const vgd_ring_sample_t &sample, time_point now) {
      note_generation(sample);

      // THE clock invariant: `_rebuilding_since` means "the ring has been
      // continuously REBUILDING since", so any observation that is not
      // REBUILDING ends that run — here, before any branch can return.
      //
      // It used to be cleared only on the fall-through at the bottom, which
      // every reinit-producing branch above returns past: a DEAD ring, a pending
      // GPU reset (the documented recovery ending — the ring goes ACTIVE with a
      // tdr_edge still recorded, so the deferred reinit is taken), a heartbeat
      // that went stale on an ACTIVE ring. Each of those left a running clock
      // behind for the owner to persist, where it outlived the outage it
      // measured and waited to be adopted by a later reader. Clearing it on
      // recovery is what stops a spent budget from being charged to a ring that
      // never spent it.
      if (sample.state != vgd_ring_state::rebuilding) {
        clear_recovery_clock();
      }

      if (sample.state == vgd_ring_state::dead) {
        return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::ring_dead);
      }

      // --- The build-16 discriminator -----------------------------------
      // REBUILDING with a heartbeat that is still beating is a driver riding
      // out a GPU outage, not a driver that died. Wait it out; the wait is
      // bounded by recovery_budget and every other ladder below is unchanged.
      // Skipped entirely for drivers that never do this (see
      // config.trust_rebuilding_heartbeat), which drops straight through to
      // the pre-change ladder below.
      if (rebuilding_and_alive(sample)) {
        if (!within_recovery_budget(now)) {
          return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::recovery_budget_expired);
        }
        // The rebuild frees every slot, so anything published before it can
        // never be claimed. Drop the delivery-stall evidence rather than
        // letting it survive the outage and fire on the first healthy sample.
        _undelivered_since.reset();
        _heartbeat_stale_since.reset();
        return decide(vgd_ring_verdict_e::wait, vgd_ring_reason_e::recovering);
      }

      // --- Everything below keeps today's timeline exactly ---------------

      // A recorded GPU reset means our own D3D device and the shared slot
      // textures are suspect: rebuild against a fresh device. Checked here,
      // ahead of the heartbeat ladder, so a driver that stopped heartbeating
      // (build 15 and earlier depart the monitor on TDR) still reinitializes
      // immediately instead of waiting out the grace window.
      if (sample.tdr_edge) {
        return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::gpu_reset);
      }

      if (sample.heartbeat_valid) {
        if (sample.heartbeat_age > _config.heartbeat_stale) {
          if (!_heartbeat_stale_since) {
            _heartbeat_stale_since = now;
          }
          if (now - *_heartbeat_stale_since > _config.heartbeat_reinit_grace) {
            return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::heartbeat_lost);
          }
          return decide(vgd_ring_verdict_e::wait, vgd_ring_reason_e::heartbeat_stale);
        }
        _heartbeat_stale_since.reset();
      }

      if (sample.state == vgd_ring_state::rebuilding) {
        // REBUILDING without a usable heartbeat: a pre-heartbeat driver, or a
        // ring whose header has not been touched yet. Nothing proves the
        // driver is alive, so this gets the same bounded wait but is NOT
        // reported as recovering (callers must not defer anything on it).
        if (!within_recovery_budget(now)) {
          return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::recovery_budget_expired);
        }
        _undelivered_since.reset();
        return decide(vgd_ring_verdict_e::wait, vgd_ring_reason_e::rebuilding);
      }

      // A frame NEWER than the one we last delivered sitting unclaimed for a
      // long stretch means the reader side is broken. Cumulative publish
      // counters are deliberately NOT used: an idle desktop (driver publishing
      // nothing new) is indistinguishable from a stall by counters alone.
      if (sample.latest_sequence > sample.delivered_sequence &&
          sample.latest_sequence > _retired_sequence) {
        if (!_undelivered_since) {
          _undelivered_since = now;
        } else if (now - *_undelivered_since > _config.delivery_stall) {
          return decide(vgd_ring_verdict_e::broken, vgd_ring_reason_e::delivery_stall);
        }
      } else {
        _undelivered_since.reset();
      }

      return decide(vgd_ring_verdict_e::consume, vgd_ring_reason_e::healthy);
    }

    /**
     * @brief Is this sample RECOVERING right now?
     *
     * A second entry point for liveness checks that do not go through the full
     * ladder — the capture loop's stale-DXGI-factory reinit asks this to decide
     * whether to defer. It shares the ONE recovery clock with evaluate() and
     * starts it if it is not already running, so a caller that only ever asks
     * this question (and therefore never reaches evaluate()) still gets a
     * bounded answer: it returns false once the budget is spent. Deliberately
     * not const for exactly that reason.
     */
    [[nodiscard]] bool recovering(const vgd_ring_sample_t &sample, time_point now) {
      // Same clock invariant as evaluate(): this can be the only entry point a
      // deferring capture loop ever reaches, so it has to be able to end the
      // outage as well as start one.
      if (sample.state != vgd_ring_state::rebuilding) {
        clear_recovery_clock();
        return false;
      }
      if (!rebuilding_and_alive(sample)) {
        return false;
      }
      return within_recovery_budget(now);
    }

    /**
     * @brief Adopt a recovery clock that started before this object existed.
     *
     * The reader owning this ladder is destroyed and rebuilt by every capture
     * reinit, so a per-object budget restarts on each one and the wait that
     * actually matters — the total time the host spends refusing to give up on
     * one outage — is unbounded. The owner keeps the clock across reader
     * instances (see display_vgd.cpp) and seeds it here, so one budget covers
     * one outage no matter how many readers it spans.
     *
     * The clock is adopted only when it is still CURRENT — when a reader
     * confirmed the ring was rebuilding within `recovery_clock_handoff_grace`.
     * That is the difference between a handoff (the outage never stopped; this
     * object is the next pair of eyes on it) and a leftover (nobody has been
     * watching, so the clock says nothing about the ring this object just
     * opened). Adopting a leftover is how a HEALTHY ring got charged for an
     * outage it never had, expired a budget it never spent, and was permanently
     * blacklisted for it.
     *
     * A no-op when `clock` is empty, so adopting "no outage in progress" never
     * disturbs a clock this object already started.
     *
     * @param clock The owner's persisted clock, if any.
     * @param now Timestamp of the adoption (monotonic).
     */
    void adopt_recovery_start(std::optional<vgd_recovery_clock_t> clock, time_point now) {
      if (!clock) {
        return;
      }
      if (now - clock->last_seen > _config.recovery_clock_handoff_grace) {
        return;
      }
      _rebuilding_since = clock->started;
      _rebuilding_seen = clock->last_seen;
    }

    /**
     * @brief The running recovery clock, or empty when no outage is in
     * progress.
     *
     * The owner persists this after every evaluate() and feeds it back to the
     * next reader through adopt_recovery_start(). Empty is the signal that the
     * outage ENDED (the ladder clears the clock the moment the ring stops
     * rebuilding), which is what lets a later, unrelated outage start a full
     * budget of its own instead of inheriting a spent one.
     */
    [[nodiscard]] std::optional<vgd_recovery_clock_t> recovery_start() const {
      if (!_rebuilding_since) {
        return std::nullopt;
      }
      return vgd_recovery_clock_t {*_rebuilding_since, _rebuilding_seen.value_or(*_rebuilding_since)};
    }

    /**
     * @brief Abandon the running recovery clock.
     *
     * For the owner to call when it has decided the outage is over as far as
     * this ladder is concerned — after an exhausted budget hands the session to
     * the fallback path, say. Distinct from adopt_recovery_start(std::nullopt),
     * which deliberately does nothing so that "no outage in progress" cannot
     * silently cancel a clock this object already started.
     */
    void reset_recovery_clock() {
      clear_recovery_clock();
    }

    /// Called after a frame is handed to the encoder: clears the stall evidence.
    void note_delivered() {
      _undelivered_since.reset();
    }

    /// How long the ring has been continuously REBUILDING (zero when it is not).
    [[nodiscard]] std::chrono::nanoseconds rebuilding_for(time_point now) const {
      if (!_rebuilding_since) {
        return std::chrono::nanoseconds::zero();
      }
      return now - *_rebuilding_since;
    }

    [[nodiscard]] const vgd_ring_liveness_config_t &config() const {
      return _config;
    }

    /// Highest sequence retired by a generation bump (test/diagnostic hook).
    [[nodiscard]] uint64_t retired_sequence() const {
      return _retired_sequence;
    }

  private:
    [[nodiscard]] bool heartbeat_live(const vgd_ring_sample_t &sample) const {
      return sample.heartbeat_valid && sample.heartbeat_age <= _config.heartbeat_stale;
    }

    /// The build-16 duck: REBUILDING, monitor still arrived, heartbeat still
    /// beating. THE one predicate behind both entry points, so a driver the
    /// owner has not vouched for can never reach a `recovering` verdict by
    /// either route.
    [[nodiscard]] bool rebuilding_and_alive(const vgd_ring_sample_t &sample) const {
      return _config.trust_rebuilding_heartbeat &&
             sample.state == vgd_ring_state::rebuilding &&
             heartbeat_live(sample);
    }

    /// Start the recovery clock if it is not running, and report whether the
    /// budget still has room. THE bound behind every rebuild wait: whichever
    /// entry point observes the rebuild first starts the clock, and both stop
    /// waiting when it expires.
    [[nodiscard]] bool within_recovery_budget(time_point now) {
      if (!_rebuilding_since) {
        _rebuilding_since = now;
      }
      // Every rebuilding observation is also a confirmation that the outage is
      // still in progress. That is what the next reader checks before adopting.
      _rebuilding_seen = now;
      return (now - *_rebuilding_since) <= _config.recovery_budget;
    }

    void clear_recovery_clock() {
      _rebuilding_since.reset();
      _rebuilding_seen.reset();
    }

    void note_generation(const vgd_ring_sample_t &sample) {
      if (!_generation) {
        // First observation: adopt the generation without retiring anything.
        // Frames published before capture opened are still claimable, and a
        // reader that cannot claim them must still be caught by the stall
        // detector.
        _generation = sample.generation;
        return;
      }
      if (*_generation == sample.generation) {
        return;
      }
      // The driver retired the slots (rebuild, mode change) without rewinding
      // latest_sequence — everything published under the old generation is
      // unclaimable now, so it cannot count as evidence that this reader is
      // broken. Frames published under the NEW generation carry higher
      // sequences and re-arm the detector immediately.
      _generation = sample.generation;
      _retired_sequence = sample.latest_sequence;
      _undelivered_since.reset();
    }

    vgd_ring_decision_t decide(vgd_ring_verdict_e verdict, vgd_ring_reason_e reason) {
      const bool first = !_last_reason || *_last_reason != reason;
      _last_reason = reason;
      return {verdict, reason, first};
    }

    vgd_ring_liveness_config_t _config {};
    std::optional<uint32_t> _generation;
    uint64_t _retired_sequence = 0;
    std::optional<time_point> _undelivered_since;
    std::optional<time_point> _heartbeat_stale_since;
    std::optional<time_point> _rebuilding_since;
    /// When this object last CONFIRMED the ring was rebuilding. Travels with
    /// `_rebuilding_since` across the reader handoff; see adopt_recovery_start().
    std::optional<time_point> _rebuilding_seen;
    std::optional<vgd_ring_reason_e> _last_reason;
  };

}  // namespace platf::dxgi
