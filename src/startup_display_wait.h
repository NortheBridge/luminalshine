/**
 * @file src/startup_display_wait.h
 * @brief Bounded-poll wait for a physical display to enumerate at startup.
 *
 * At cold boot, post-resume, or shortly after a TDR the Windows display API
 * can transiently fail to enumerate a physical display for several seconds
 * — display_helper logs ERROR_GEN_FAILURE from QueryDisplayConfig while the
 * GPU / monitor topology stabilises. Committing to the SudoVDA fallback
 * during that window has historically forced capture onto the iGPU and
 * exposed the AMF encoder-probe crash path. This helper polls a caller-
 * supplied predicate a bounded number of times before giving up, so the
 * fallback is only taken after the OS has had a fair chance to settle.
 *
 * The helper is header-only and parameterised on callbacks (display
 * predicate, shutdown predicate, sleep) so unit tests can drive it
 * deterministically without real clocks, threads, or display drivers.
 */
#pragma once

#include <chrono>
#include <functional>
#include <thread>

namespace startup {

  struct display_wait_callbacks {
    /// Returns true when a physical display has enumerated.
    std::function<bool()> is_physical_display_ready;
    /// Returns true if the caller wants to short-circuit the wait
    /// (e.g. a shutdown event has been signalled).
    std::function<bool()> should_abort;
    /// Sleep callback — injectable so tests do not have to wait in real time.
    /// If unset, falls back to std::this_thread::sleep_for.
    std::function<void(std::chrono::milliseconds)> sleep_for;
  };

  struct display_wait_result {
    bool display_ready;  ///< true iff is_physical_display_ready() returned true
    bool aborted;        ///< true iff should_abort() returned true mid-wait
    int attempts;        ///< total predicate evaluations (initial probe + retries)
  };

  /**
   * @brief Poll is_physical_display_ready up to max_polls additional times
   * with poll_step delay between attempts. Returns as soon as the predicate
   * is true, the abort predicate is true, or the budget is exhausted.
   *
   * The "initial probe" is counted as attempt #1 — the caller has typically
   * already evaluated is_physical_display_ready once before deciding to
   * enter the wait, so attempts starts at 1.
   *
   * @param max_polls   Maximum number of additional predicate evaluations
   *                    after the implicit initial probe. Total wall-clock
   *                    upper bound is roughly max_polls * poll_step.
   * @param poll_step   Delay between poll attempts.
   * @param cb          Callbacks; only is_physical_display_ready is required.
   */
  inline display_wait_result wait_for_physical_display(
    int max_polls,
    std::chrono::milliseconds poll_step,
    const display_wait_callbacks &cb
  ) {
    display_wait_result result {false, false, 1};

    for (int i = 0; i < max_polls; ++i) {
      if (cb.should_abort && cb.should_abort()) {
        result.aborted = true;
        return result;
      }
      if (cb.sleep_for) {
        cb.sleep_for(poll_step);
      } else {
        std::this_thread::sleep_for(poll_step);
      }
      ++result.attempts;
      if (cb.is_physical_display_ready && cb.is_physical_display_ready()) {
        result.display_ready = true;
        return result;
      }
    }

    return result;
  }

}  // namespace startup
