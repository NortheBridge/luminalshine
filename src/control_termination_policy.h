/**
 * @file src/control_termination_policy.h
 * @brief Whether a stopping session's control peer is told the session ended gracefully.
 */
#pragma once

#include "session_stop_reason.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace stream::control_termination {

  /// IDX_TERMINATION code that tells a Moonlight-family client the host ended the stream on purpose.
  constexpr std::uint32_t graceful_termination_code = 0x80030023;

  /**
   * @brief Whether a stopping session's control peer gets IDX_TERMINATION with
   *        graceful_termination_code before it is disconnected.
   * @param reason The first cause recorded by session::stop; count_ when none was recorded.
   * @param peer_connected Whether the control peer is still connected.
   *
   * Only stops the host made on purpose while the client still listens qualify. A stop the client
   * caused or can no longer hear sends nothing, and so does one whose cause is ambiguous: the
   * video and audio thread exits also cover capture failures, and rtsp_clear_all an RTSP accept
   * failure, that must not look graceful. On host shutdown the control thread's exit loop
   * already sends the graceful code to the sessions it still holds.
   */
  constexpr bool should_send_graceful_termination(session::stop_reason_e reason, bool peer_connected) noexcept {
    if (!peer_connected) {
      return false;
    }

    // No default: a new reason fails -Wswitch until it is classified here.
    switch (reason) {
      case session::stop_reason_e::explicit_cancel:
      case session::stop_reason_e::client_disconnect_request:
      case session::stop_reason_e::rtsp_broadcast_shutdown:
        return true;
      case session::stop_reason_e::enet_disconnect:
      case session::stop_reason_e::client_termination:
      case session::stop_reason_e::ping_timeout:
      case session::stop_reason_e::control_decrypt_failure:
      case session::stop_reason_e::bad_encrypted_type:
      case session::stop_reason_e::video_thread_exit:
      case session::stop_reason_e::audio_thread_exit:
      case session::stop_reason_e::rtsp_clear_all:
      case session::stop_reason_e::app_exited_before_peer:
      case session::stop_reason_e::admission_retired:
      case session::stop_reason_e::rtsp_reap:
      case session::stop_reason_e::count_:
        break;
    }
    return false;
  }

  /// Kind of a control host event, as far as detached_peers_t needs to tell them apart.
  enum class peer_event_e {
    connect,
    receive,
    disconnect,
  };

  /**
   * @brief Control peers already detached from their session that are still finishing a
   *        graceful disconnect, so their last events never reach session lookup.
   *
   * ENet ends every such peer with one DISCONNECT event, which retires it. A CONNECT on a
   * detached slot means the slot was reset without one and reused: the stale entry is retired
   * and the event handled normally.
   */
  template<class Peer, class TimePoint>
  class detached_peers_t {
  public:
    void add(Peer peer, TimePoint since) {
      _peers.insert_or_assign(peer, since);
    }

    /// When @p peer was detached, if it still is.
    std::optional<TimePoint> detached_since(Peer peer) const {
      auto it = _peers.find(peer);
      if (it == _peers.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    /// Whether the event belongs to a detached peer and must be dropped.
    bool consume(Peer peer, peer_event_e event) {
      auto it = _peers.find(peer);
      if (it == _peers.end()) {
        return false;
      }
      if (event == peer_event_e::receive) {
        return true;
      }

      _peers.erase(it);
      return event == peer_event_e::disconnect;
    }

    std::size_t size() const noexcept {
      return _peers.size();
    }

  private:
    std::map<Peer, TimePoint> _peers;
  };

}  // namespace stream::control_termination
