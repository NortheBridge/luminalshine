/**
 * @file src/platform/windows/display_helper_wedge_escalation.h
 * @brief Pure decision logic for escalating persistent machine-wide display enumeration failures.
 */
#pragma once

// standard
#include <chrono>
#include <cstddef>

namespace display_helper_integration {

  /**
   * @brief State machine deciding how the display helper's restore loop reacts to
   * a sequence of display-enumeration health probes.
   *
   * Motivation (POSTMORTEM-2026-07-27.md, helper blocker #6): during the
   * machine-wide WDDM wedge the interactive-desktop helper silently polled
   * QueryDisplayConfig for 84 minutes, rejecting golden/session restore
   * snapshots every cycle with "no valid devices available" and never telling
   * anyone the display stack was dead.
   *
   * The machine consumes one sample per observation — "did display enumeration
   * return any devices?" plus a caller-supplied timestamp — and yields one of:
   * - Proceed: nothing notable (healthy, or a failure streak that has not yet
   *   met the persistence threshold); run the normal restore flow.
   * - Escalate: failures just crossed the persistence threshold (>= N
   *   consecutive failures spanning >= the minimum duration); emit ONE loud log
   *   (display stack dead machine-wide, reboot required) and hold the restore.
   * - Hold: still wedged; skip snapshot re-evaluation and stay quiet.
   * - Relog: still wedged and the re-log interval elapsed; emit the loud log
   *   again (rate-limited), then keep holding.
   * - RetryNow: enumeration transitioned from wedged back to succeeding;
   *   re-attempt the held restore immediately (callers must log the attempt).
   *
   * Purely functional over its inputs (no clock access, no logging, no I/O) so
   * it can be unit-tested deterministically. Not thread-safe; callers must
   * serialize access — the helper only touches it from the restore polling
   * thread, and successive polling threads are serialized by std::jthread joins.
   */
  class WedgeEscalation {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    enum class Action {
      Proceed,  ///< Normal restore flow; nothing to report.
      Escalate,  ///< Threshold crossed: emit the loud machine-wide-failure log, then hold.
      Hold,  ///< Wedged: skip snapshot re-evaluation, stay quiet.
      Relog,  ///< Wedged and the re-log interval elapsed: emit the loud log again.
      RetryNow,  ///< Enumeration recovered from a wedge: re-attempt the held restore now.
    };

    struct Config {
      /// Consecutive failed samples required before escalating.
      std::size_t failure_threshold {5};
      /// Failures must also span at least this long before escalating.
      std::chrono::milliseconds min_failure_duration {std::chrono::seconds(60)};
      /// While wedged, re-emit the loud log at most this often.
      std::chrono::milliseconds relog_interval {std::chrono::minutes(10)};
    };

    WedgeEscalation() = default;

    explicit WedgeEscalation(Config config):
        config_(config) {
    }

    /**
     * @brief Feed one enumeration health sample and get the resulting decision.
     * @param enumeration_ok Whether display enumeration returned any devices.
     * @param now Timestamp of the sample (monotonic).
     */
    [[nodiscard]] Action on_enumeration_result(bool enumeration_ok, time_point now) {
      if (enumeration_ok) {
        const bool was_wedged = wedged_;
        reset();
        return was_wedged ? Action::RetryNow : Action::Proceed;
      }

      if (consecutive_failures_ == 0) {
        first_failure_ = now;
      }
      ++consecutive_failures_;

      if (!wedged_) {
        if (consecutive_failures_ >= config_.failure_threshold &&
            (now - first_failure_) >= config_.min_failure_duration) {
          wedged_ = true;
          last_escalation_log_ = now;
          return Action::Escalate;
        }
        return Action::Proceed;
      }

      if ((now - last_escalation_log_) >= config_.relog_interval) {
        last_escalation_log_ = now;
        return Action::Relog;
      }
      return Action::Hold;
    }

    /// True once the persistence threshold has been crossed (loud log emitted).
    [[nodiscard]] bool wedged() const {
      return wedged_;
    }

    /// True while a failure streak is being tracked (wedged or still counting).
    /// Callers use this to keep probing cheaply instead of abandoning the loop.
    [[nodiscard]] bool engaged() const {
      return wedged_ || consecutive_failures_ > 0;
    }

    [[nodiscard]] std::size_t consecutive_failures() const {
      return consecutive_failures_;
    }

    /// How long the current failure streak has lasted (zero when healthy).
    [[nodiscard]] std::chrono::milliseconds failing_for(time_point now) const {
      if (consecutive_failures_ == 0) {
        return std::chrono::milliseconds::zero();
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(now - first_failure_);
    }

    [[nodiscard]] const Config &config() const {
      return config_;
    }

    void reset() {
      consecutive_failures_ = 0;
      wedged_ = false;
      first_failure_ = {};
      last_escalation_log_ = {};
    }

  private:
    Config config_ {};
    std::size_t consecutive_failures_ {0};
    bool wedged_ {false};
    time_point first_failure_ {};
    time_point last_escalation_log_ {};
  };
}  // namespace display_helper_integration
