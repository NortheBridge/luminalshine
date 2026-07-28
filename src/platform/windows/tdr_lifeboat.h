/**
 * @file src/platform/windows/tdr_lifeboat.h
 * @brief TDR topology lifeboat — keep a hardware-backed display available
 *        while Windows recovers from a GPU hang.
 *
 * In all three machine-wide WDDM wedges observed so far (2026-05-17,
 * 2026-07-26, 2026-07-27 — see POSTMORTEM-2026-07-27.md), an IddCx virtual
 * display was the EXCLUSIVE active display when the GPU hung, and Windows'
 * TDR recovery then failed, leaving every output black until a reboot. The
 * open hypothesis is that recovery waits on the indirect display whose own
 * presentation pipeline runs on the very GPU being reset.
 *
 * The lifeboat: the moment the host detects a GPU hang (the earliest signal
 * is tdr::mark_event with source encoder_stall, ~100 ms in), best-effort
 * activate one PHYSICAL display alongside the virtual one — extend the
 * topology, never deactivate the virtual display capture is running on — so
 * the OS always has a hardware-backed display to recompose onto during
 * recovery. Even if the hypothesis is wrong, this ends the "every output is
 * black" experience: whatever recovery does, one physical output is lit.
 *
 * Everything here is best-effort and bounded: if the display stack is
 * already too far gone for the helper to act (~5 s), we log and give up —
 * the existing escalation paths (stack_down latch, session teardown) own
 * the terminal handling.
 */
#pragma once

#include "src/tdr_state.h"

#include <chrono>
#include <optional>

namespace tdr_lifeboat {

  /// Minimum spacing between lifeboat attempts. One attempt per incident:
  /// a single GPU hang cascades through several detectors and, on a stack
  /// that never recovers, is re-detected on every client attempt (~33 s
  /// apart in the 2026-07-27 field incident). Matches tdr_state's incident
  /// window so "once per incident" and "once per cooldown" line up.
  constexpr std::chrono::minutes ATTEMPT_COOLDOWN {5};

  /**
   * @brief Whether a tdr:: event source warrants a lifeboat attempt.
   *
   * Every TDR-class detection qualifies: the earlier the attempt, the more
   * likely the display stack can still service a topology write, but even a
   * late detector (display_stack_down) is worth one bounded try. Unknown /
   * out-of-range values do not trigger.
   */
  [[nodiscard]] constexpr bool is_lifeboat_source(tdr::source_t source) noexcept {
    switch (source) {
      case tdr::source_t::encoder_d3d11:
      case tdr::source_t::dd_test_d3d11:
      case tdr::source_t::virtual_display_enumerate:
      case tdr::source_t::query_display_config:
      case tdr::source_t::encoder_stall:
      case tdr::source_t::display_stack_down:
        return true;
    }
    return false;
  }

  /**
   * @brief Pure "should a lifeboat attempt run now?" decision.
   *
   * Combines the two gating rules — the event source must be TDR-class,
   * and no attempt may have been armed within the cooldown window — into
   * one testable object with the clock injected by the caller.
   *
   * Not thread-safe on its own; the production consumer (tdr_lifeboat.cpp)
   * serialises access. Header-only so unit tests exercise the exact
   * shipping logic on every platform.
   */
  class attempt_gate {
  public:
    explicit attempt_gate(std::chrono::steady_clock::duration cooldown = ATTEMPT_COOLDOWN):
        cooldown_ {cooldown} {
    }

    /**
     * @brief Decide whether to attempt, and arm the cooldown if so.
     * @param source The tdr:: event source that fired.
     * @param now Caller-supplied steady-clock timestamp.
     * @return True exactly when `source` is lifeboat-relevant and no prior
     *         attempt was armed within the cooldown window. A true return
     *         arms the cooldown — the caller is expected to attempt.
     */
    [[nodiscard]] bool should_attempt(tdr::source_t source, std::chrono::steady_clock::time_point now) {
      if (!is_lifeboat_source(source)) {
        return false;
      }
      if (last_attempt_ && (now - *last_attempt_) < cooldown_) {
        // Same incident, re-detected. Denials deliberately do NOT extend
        // the cooldown: a wedge that keeps re-firing must not starve the
        // next attempt forever.
        return false;
      }
      last_attempt_ = now;
      return true;
    }

  private:
    std::chrono::steady_clock::duration cooldown_;
    std::optional<std::chrono::steady_clock::time_point> last_attempt_;
  };

  /**
   * @brief Arm the lifeboat: installs the tdr:: event hook.
   *
   * Call once from Windows platf::init(), after the task pool is running.
   * The hook does a constant-time gate check on the caller's thread and
   * posts all real work to the task pool — it never blocks mark_event's
   * caller and never throws.
   */
  void init();

}  // namespace tdr_lifeboat
