/**
 * @file src/platform/windows/display_helper_policy.h
 * @brief Pure decisions of the display-helper integration, kept Win32-free so they can be unit-tested.
 */
#pragma once

// standard includes
#include <cstdint>

namespace display_helper_integration {
  /**
   * @brief How long after a verified transactional restore a second REVERT for the
   *        same session is treated as a duplicate.
   */
  constexpr std::int64_t kDuplicateRevertWindowUs = 5'000'000;

  /**
   * @brief Whether a REVERT request should be dropped as a duplicate.
   *
   * The RTSP session-end cleanup and the app-exit path both revert the same
   * session, typically within a second of each other. Once the first restore has
   * completed and its helper has exited, the second request has nothing left to
   * restore; sending it anyway starts a helper whose redundant restore nobody
   * waits on, and a launch that arrives while it runs can reuse that helper just
   * as it exits.
   *
   * @param prefer_golden_if_current_missing The caller asked for the golden
   *        snapshot fallback; such a request is never suppressed.
   * @param last_revert_completed_us Steady-clock time of the last verified
   *        transactional restore, 0 if none.
   * @param now_us Current steady-clock time.
   * @param helper_process_running A helper process is alive. The caller must
   *        evaluate this under the helper mutex so an in-flight transactional
   *        wait has finished first.
   * @return true when the request is a duplicate of a restore that just completed.
   */
  constexpr bool duplicate_revert_suppressed(
    bool prefer_golden_if_current_missing,
    std::int64_t last_revert_completed_us,
    std::int64_t now_us,
    bool helper_process_running
  ) {
    return !prefer_golden_if_current_missing &&
           last_revert_completed_us > 0 &&
           now_us - last_revert_completed_us < kDuplicateRevertWindowUs &&
           !helper_process_running;
  }
}  // namespace display_helper_integration
