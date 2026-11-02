/**
 * @file src/cancel_scope_policy.h
 * @brief Policy for how far one client's /cancel reaches.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace cancel_scope {

  /**
   * @brief How far a /cancel request reaches.
   */
  enum class scope_e {
    full,  ///< The requester is the only client: end its sessions and tear the app and displays down.
    requester_only,  ///< Another client is live: end only the requester's sessions and keep the app running.
  };

  /**
   * @brief A tracked RTSP session, reduced to what the policy needs.
   */
  struct session_t {
    std::string_view client_uuid;  ///< Empty when the session's client identity is unknown.
    bool stopping;  ///< True when the session is already being stopped.
  };

  /**
   * @brief Count the live sessions, pending launch and browser session, not the requester's, that a full cancel would end.
   *
   * A client counts as "other" when it cannot be proven to be the requester:
   * a non-stopping session or a pending launch with an empty or different uuid,
   * or an active browser (WebRTC) session. An empty requester therefore owns nothing.
   *
   * @param requester_uuid The uuid of the client that sent /cancel; empty when unknown.
   * @param sessions The tracked sessions.
   * @param pending_launch_owner The uuid of the client that owns the pending launch, if there is one.
   * @param webrtc_active True when a browser session is active.
   */
  constexpr std::size_t other_clients(
    std::string_view requester_uuid,
    std::span<const session_t> sessions,
    std::optional<std::string_view> pending_launch_owner,
    bool webrtc_active
  ) noexcept {
    const auto is_other = [&](std::string_view uuid) {
      return uuid.empty() || uuid != requester_uuid;
    };

    std::size_t count = 0;
    for (const auto &session : sessions) {
      if (!session.stopping && is_other(session.client_uuid)) {
        ++count;
      }
    }
    if (pending_launch_owner && is_other(*pending_launch_owner)) {
      ++count;
    }
    if (webrtc_active) {
      ++count;
    }
    return count;
  }

  /**
   * @brief Whether a session's launch id was seen by the /cancel decision.
   *
   * A full cancel decides on a snapshot and stops afterwards. The launch id is a
   * session's stable identity (one session per launch, assigned from a counter),
   * so a session that became visible after the snapshot carries an id the snapshot
   * lacks and must be left running.
   *
   * @param launch_id The launch id of the session being considered.
   * @param observed The launch ids of the sessions and the pending launch the decision saw.
   */
  constexpr bool was_observed(std::uint32_t launch_id, std::span<const std::uint32_t> observed) noexcept {
    return std::ranges::find(observed, launch_id) != observed.end();
  }

  /**
   * @brief Whether the pending launch is the one a cancel meant to pop.
   *
   * @param expected_id The launch id the caller observed, or nothing to pop whatever is pending.
   * @param pending_id The launch id that is pending now.
   */
  constexpr bool is_expected_launch(std::optional<std::uint32_t> expected_id, std::uint32_t pending_id) noexcept {
    return !expected_id || *expected_id == pending_id;
  }

  /**
   * @brief Decide the scope of a /cancel request: requester_only when any other client is live, else full.
   */
  constexpr scope_e decide(
    std::string_view requester_uuid,
    std::span<const session_t> sessions,
    std::optional<std::string_view> pending_launch_owner,
    bool webrtc_active
  ) noexcept {
    return other_clients(requester_uuid, sessions, pending_launch_owner, webrtc_active) > 0 ?
             scope_e::requester_only :
             scope_e::full;
  }

}  // namespace cancel_scope
