/**
 * @file src/encoder_recovery_gate.h
 * @brief Pure decision logic for "may an encoder thread wait out a capture
 * source's recovery instead of ending the client's session?"
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

namespace video {

  /// What an encoder thread should do with an operation that failed while the
  /// capture source says it is inside a bounded recovery window.
  enum class encoder_recovery_action_e {
    fail,  ///< Run today's failure handling (tear the session down).
    hold,  ///< Keep the session and wait for the window to close.
  };

  struct encoder_recovery_gate_config_t {
    /**
     * @brief Ceiling on the time ONE encoder thread will hold a session open
     * across a capture source's recovery.
     *
     * Sits above the capture loop's own deferral ceiling, which sits above the
     * ring reader's recovery budget, which sits above the driver's: each layer
     * gives up before the one above it has to, so the give-up that actually
     * fires is always the most specific one — the one with the best diagnosis.
     * This exists only so a publisher that gets stuck reporting recovery cannot
     * wedge an encoder thread forever; a SYSTEM service must not have a state
     * from which no bound expires.
     */
    std::chrono::nanoseconds hold_ceiling {std::chrono::minutes(16)};
  };

  /**
   * @brief Bounds how long an encoder thread defers to a recovering capture
   * source.
   *
   * Why this exists: the capture side answers a GPU outage by NOT
   * reinitializing — it holds its cheap timeout loop until the display comes
   * back (see platf::display_t::capture_recovering()). That alone makes things
   * worse. Before, a reinit parked the encoder thread; deferring it leaves the
   * encoder RUNNING across the outage, where its first failed encode — or
   * failed encode-device creation, because the GPU is down — runs the ordinary
   * failure path and ends the client's session. The outage still kills the
   * stream, just from the other thread. So the recovery verdict has to reach
   * the encoder too, and this is the part of that with no I/O in it.
   *
   * Accumulates only the time actually observed in recovery, so it measures the
   * outage rather than wall time: a source that flaps in and out of recovery
   * still exhausts the ceiling instead of restarting it, while genuine progress
   * (note_progress()) clears it so a later, unrelated outage gets a full
   * ceiling of its own.
   *
   * Purely functional over its inputs (no clock access, no I/O, no logging) so
   * it can be unit-tested deterministically. Not thread-safe: each encoder
   * thread owns one.
   */
  class encoder_recovery_gate_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    encoder_recovery_gate_t() = default;

    explicit encoder_recovery_gate_t(encoder_recovery_gate_config_t config):
        _config(config) {
    }

    /**
     * @brief Feed the capture source's current verdict and get this thread's.
     * @param capture_recovering What platf::display_t::capture_recovering() says.
     * @param now Timestamp of the observation (monotonic).
     * @return hold to keep the session and wait; fail to run today's handling.
     *
     * Call this both to decide whether to hold and to keep holding — the held
     * time only advances across consecutive calls that observed recovery, so
     * polling it in a wait loop is what makes the ceiling meaningful.
     */
    [[nodiscard]] encoder_recovery_action_e evaluate(bool capture_recovering, time_point now) {
      // Charge the interval that just elapsed if it was spent holding. Charging
      // it against the PREVIOUS observation rather than this one is what makes
      // the ceiling proof against flapping: a source that alternates recovering
      // and not still accumulates every interval it spent recovering, instead
      // of resetting the clock each time it blinks.
      if (_last_observation && _last_was_recovering) {
        const auto elapsed = now - *_last_observation;
        // Monotonic clock, so `elapsed` cannot be negative; guard anyway rather
        // than let a bad sample rewind the ceiling.
        if (elapsed > std::chrono::nanoseconds::zero()) {
          _held += elapsed;
        }
      }
      _last_observation = now;
      _last_was_recovering = capture_recovering;

      if (!capture_recovering) {
        return encoder_recovery_action_e::fail;
      }
      if (_held > _config.hold_ceiling) {
        return encoder_recovery_action_e::fail;
      }
      return encoder_recovery_action_e::hold;
    }

    /**
     * @brief The encoder made real progress: a frame encoded, or an encode
     * device built against a recovered display.
     *
     * The outage this gate was holding for is over, so the next one starts from
     * a full ceiling. Only progress clears the clock — never the mere absence
     * of a recovery report.
     */
    void note_progress() {
      _held = std::chrono::nanoseconds::zero();
      _last_observation.reset();
      _last_was_recovering = false;
    }

    /// Total time this thread has held a session open for the current outage.
    [[nodiscard]] std::chrono::nanoseconds held_for() const {
      return _held;
    }

    [[nodiscard]] const encoder_recovery_gate_config_t &config() const {
      return _config;
    }

  private:
    encoder_recovery_gate_config_t _config {};
    std::chrono::nanoseconds _held {0};
    std::optional<time_point> _last_observation;
    bool _last_was_recovering = false;
  };

  struct recovery_keepalive_config_t {
    /**
     * @brief Fastest the keepalive may run while holding.
     *
     * Every keepalive emitted during an outage is an encode against a GPU that
     * may still be resetting, so running it at the session's full minimum-FPS
     * cadence (16 ms at 60 fps) would hammer a recovering device for minutes to
     * re-send a picture that cannot change — there is no new capture. The client
     * only needs to hear from us often enough not to time out.
     */
    std::chrono::nanoseconds fastest {std::chrono::milliseconds(250)};

    /**
     * @brief Slowest the keepalive may run while holding.
     *
     * The client's own no-video timeout is the entire reason this exists: a host
     * that holds a session open while emitting nothing has saved the session and
     * lost the client, which is worse than either outcome alone. So a session
     * whose minimum-FPS target is very low still gets a floor.
     */
    std::chrono::nanoseconds slowest {std::chrono::seconds(1)};

    /// How often a FAILING keepalive may be logged. A hold runs for minutes and
    /// this is polled at the capture loop's rate.
    std::chrono::nanoseconds failure_log_interval {std::chrono::seconds(15)};

    /**
     * @brief Ceiling on how long ONE keepalive encode may take before this pacer
     * stops asking for more, or empty when the emitting thread can afford to
     * wait on the GPU.
     *
     * Empty is right for a dedicated encoder thread: a slow encode there costs
     * the stream and nothing else, and giving up on the keepalive would lose the
     * client the hold exists to keep.
     *
     * It is NOT right for a thread that also has to keep ticking. The sync
     * encode path emits from the CAPTURE thread — the same thread that publishes
     * the recovery verdict, enforces the capture loop's own deferral ceiling and
     * services joining sessions and display switches — because on that path the
     * encode callback IS the capture thread. A keepalive aimed at a GPU that is
     * still resetting is bounded only by the encoder's internal deadline
     * (seconds for NvEnc, unspecified elsewhere), and every millisecond of it is
     * a millisecond the recovery loop is not ticking. A budget bounds the total
     * charge to ONE slow call per hold: the first overrun suspends the pacer
     * until real progress resumes.
     *
     * A keepalive that FAILS quickly is not slow and keeps running — that is the
     * common case during an outage, it costs the recovery loop nothing, and it
     * is the whole point of the mechanism.
     */
    std::optional<std::chrono::nanoseconds> emit_budget {};
  };

  /**
   * @brief Paces the video keepalive an encoder emits while it is holding a
   * session open across a capture source's recovery.
   *
   * Why this exists: holding the session is only half of surviving an outage.
   * The other half is that the CLIENT has to survive it too, and a Moonlight
   * client that receives no video for a few seconds tears the session down from
   * its end. An outage the driver rides out runs for MINUTES. So a hold that
   * also stops emitting saves the host's session and loses the client's — the
   * host ends up nursing a session nobody is watching, which is a worse outcome
   * than the teardown the hold was introduced to prevent.
   *
   * The sync encode path is where that bites: its callback IS the capture
   * thread, so there is nothing to park, and skipping the encode outright (the
   * obvious way to avoid a failing encode against a resetting GPU) means no
   * packets at all for the whole window. Re-sending the last converted frame on
   * a slow cadence keeps the stream alive, costs nothing when the GPU is fine
   * (the picture is static anyway — there is no new capture to send), and when
   * the GPU is not fine the encode simply fails.
   *
   * That failure must NOT end the session: it is the outage, not a verdict about
   * the session, and the encoder gate above is what bounds how long it may go on
   * being tolerated. Nor is a successful keepalive progress — progress is a real
   * captured frame. Treating one as progress would refresh the gate's ceiling on
   * every keepalive and make the hold unbounded.
   *
   * Purely functional over its inputs (no clock access, no I/O, no logging) so
   * it can be unit-tested deterministically. Not thread-safe: one per session.
   */
  class recovery_keepalive_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    recovery_keepalive_t() = default;

    explicit recovery_keepalive_t(recovery_keepalive_config_t config):
        _config(config) {
    }

    /**
     * @brief Set the cadence from the session's minimum-FPS frame time.
     *
     * Clamped into [fastest, slowest]: the session's own static-content cadence
     * is the right intent, but neither end of its range is safe here.
     */
    void set_interval(std::chrono::nanoseconds session_interval) {
      _interval = session_interval < _config.fastest ? _config.fastest :
                  session_interval > _config.slowest ? _config.slowest :
                                                       session_interval;
    }

    [[nodiscard]] std::chrono::nanoseconds interval() const {
      return _interval;
    }

    /**
     * @brief Is a keepalive frame due right now?
     *
     * @param holding Whether the encoder is holding for a display recovery.
     * @param now Timestamp of the observation (monotonic).
     * @return true when the caller should emit a keepalive frame.
     *
     * The first observation of a hold is always due, so the client hears from
     * the host on the same tick the hold starts rather than one interval into an
     * outage it is already counting down on.
     */
    [[nodiscard]] bool due(bool holding, time_point now) const {
      if (!holding || _suspended) {
        return false;
      }
      if (!_last_emitted) {
        return true;
      }
      return now - *_last_emitted >= _interval;
    }

    /// Record that a keepalive went out — successfully or not. Either way the
    /// device was asked, which is what the cadence is pacing.
    void note_emitted(time_point now) {
      _last_emitted = now;
    }

    /**
     * @brief Report how long the keepalive that just went out took.
     * @return true when this call suspended the pacer, so the caller can say so
     *         once instead of on every skipped tick.
     *
     * A no-op unless the caller set an emit budget — see
     * recovery_keepalive_config_t::emit_budget for which threads need one and
     * why. Suspension lasts until note_progress(): the display coming back is
     * the only evidence that the device answers promptly again.
     */
    bool note_emit_duration(std::chrono::nanoseconds took) {
      if (_suspended || !_config.emit_budget || took <= *_config.emit_budget) {
        return false;
      }
      _suspended = true;
      return true;
    }

    /// True while a keepalive that overran the emit budget has stood this pacer
    /// down. due() is false throughout.
    [[nodiscard]] bool suspended() const {
      return _suspended;
    }

    /**
     * @brief Whether this keepalive failure should be logged.
     *
     * Rate-limited: a hold runs for minutes and every keepalive in it can fail.
     */
    [[nodiscard]] bool should_report_failure(time_point now) {
      if (_failure_reported && now - *_failure_reported < _config.failure_log_interval) {
        return false;
      }
      _failure_reported = now;
      return true;
    }

    /**
     * @brief A real captured frame went out: this outage is over.
     *
     * Deliberately NOT called for a successful keepalive — see the class note.
     */
    void note_progress() {
      _last_emitted.reset();
      _failure_reported.reset();
      _suspended = false;
    }

    [[nodiscard]] const recovery_keepalive_config_t &config() const {
      return _config;
    }

  private:
    recovery_keepalive_config_t _config {};
    std::chrono::nanoseconds _interval {std::chrono::milliseconds(250)};
    std::optional<time_point> _last_emitted;
    std::optional<time_point> _failure_reported;
    bool _suspended = false;
  };

}  // namespace video
