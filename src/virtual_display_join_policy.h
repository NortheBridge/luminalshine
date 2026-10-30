/**
 * @file src/virtual_display_join_policy.h
 * @brief Policy that keeps a client joining a running stream from changing the owner's virtual display.
 */
#pragma once

namespace virtual_display_join {

  /**
   * @brief Whether a launch/resume may cancel the fallback monitor armed by an earlier launch.
   *
   * With no session active nothing owns that monitor any more, so a fresh launch
   * or an idle /resume must supersede it. While another session is streaming the
   * monitor is that session's live state, and a joining client must leave it alone.
   * @param no_active_sessions True when no streaming session is currently active.
   */
  constexpr bool may_supersede_fallback_monitors(bool no_active_sessions) noexcept {
    return no_active_sessions;
  }

  /**
   * @brief Whether a launch/resume may rewrite the global runtime output override.
   *
   * A joiner shares the owner's display, so the override already points at it.
   * Rewriting it, or recording it as pending, lets a failed /resume clear the
   * override while the owner is still streaming.
   * @param joined_existing_virtual_display True when this launch reuses a display owned by another active session.
   */
  constexpr bool may_change_output_override(bool joined_existing_virtual_display) noexcept {
    return !joined_existing_virtual_display;
  }

  /**
   * @brief Whether RTSP ANNOUNCE may remove and recreate the virtual display to match the RTSP mode.
   *
   * A joiner keeps the owner's display mode; recreating the display would drop
   * the owner's stream onto a new mode.
   * @param joined_existing_virtual_display True when this launch reuses a display owned by another active session.
   */
  constexpr bool may_reconcile_mode(bool joined_existing_virtual_display) noexcept {
    return !joined_existing_virtual_display;
  }

}  // namespace virtual_display_join
