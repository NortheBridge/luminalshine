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
    std::chrono::nanoseconds recovery_budget {std::chrono::minutes(11)};
  };

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
   *     capture pipeline in its cheap timeout loop (the encoder re-sends the
   *     last frame, the client stays connected) instead of spinning the
   *     capture-reinit machinery while the GPU is still down.
   *  3. Everything else runs on exactly today's timeline: a GPU-reset edge
   *     reinitializes immediately, a stale heartbeat reinitializes after its
   *     grace window, and a genuinely undeliverable ring is declared broken.
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

      if (sample.state == vgd_ring_state::dead) {
        return decide(vgd_ring_verdict_e::reinit, vgd_ring_reason_e::ring_dead);
      }

      // --- The build-16 discriminator -----------------------------------
      // REBUILDING with a heartbeat that is still beating is a driver riding
      // out a GPU outage, not a driver that died. Wait it out; the wait is
      // bounded by recovery_budget and every other ladder below is unchanged.
      if (sample.state == vgd_ring_state::rebuilding && heartbeat_live(sample)) {
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
      _rebuilding_since.reset();

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
      if (sample.state != vgd_ring_state::rebuilding || !heartbeat_live(sample)) {
        return false;
      }
      return within_recovery_budget(now);
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

    /// Start the recovery clock if it is not running, and report whether the
    /// budget still has room. THE bound behind every rebuild wait: whichever
    /// entry point observes the rebuild first starts the clock, and both stop
    /// waiting when it expires.
    [[nodiscard]] bool within_recovery_budget(time_point now) {
      if (!_rebuilding_since) {
        _rebuilding_since = now;
      }
      return (now - *_rebuilding_since) <= _config.recovery_budget;
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
    std::optional<vgd_ring_reason_e> _last_reason;
  };

}  // namespace platf::dxgi
