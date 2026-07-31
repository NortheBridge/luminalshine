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

}  // namespace video
