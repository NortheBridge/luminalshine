/**
 * @file src/session_stop_reason.h
 * @brief Vocabulary and log formatting for why a streaming session stopped (diagnostics only).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace stream::session {

  /**
   * @brief Why session::stop was called. One value per distinct call-site cause,
   *        so the log line written on the RUNNING -> STOPPING transition names
   *        the origin without anyone having to guess from timing.
   */
  enum class stop_reason_e : int {
    enet_disconnect,  ///< The control stream's ENet peer disconnected.
    client_termination,  ///< The client sent a termination message on the control stream.
    ping_timeout,  ///< No control traffic arrived within the ping timeout.
    control_decrypt_failure,  ///< A control message failed GCM tag verification.
    bad_encrypted_type,  ///< An encrypted control message wrapped another encrypted message.
    /// The session's video thread returned. Its exit guard stops the session whatever ended
    /// the thread, so this also covers a failed initial UDP ping and a shutdown raised without
    /// stop() by the control thread's termination broadcast; the logs around it name the cause.
    video_thread_exit,
    /// The session's audio thread returned; covers the same causes as video_thread_exit.
    audio_thread_exit,
    app_exited_before_peer,  ///< The app terminated before the client's control peer connected.
    admission_retired,  ///< RTSP admission was retired (cancel or expiry) while the session started.
    rtsp_clear_all,  ///< The RTSP server cleared all sessions on its own teardown or failure; the logged call site says which.
    rtsp_broadcast_shutdown,  ///< The RTSP handler cleared all sessions because the broadcast is shutting down.
    rtsp_reap,  ///< The RTSP server reaped an already stopping session.
    client_disconnect_request,  ///< A disconnect of one client's sessions was requested (Web UI).
    explicit_cancel,  ///< An explicit /cancel stopped the session (every session, or only the requester's while another client streams).
    count_,  ///< Number of reasons; not a valid reason.
  };

  constexpr const char *stop_reason_name(stop_reason_e reason) noexcept {
    switch (reason) {
      case stop_reason_e::enet_disconnect:
        return "enet_disconnect";
      case stop_reason_e::client_termination:
        return "client_termination";
      case stop_reason_e::ping_timeout:
        return "ping_timeout";
      case stop_reason_e::control_decrypt_failure:
        return "control_decrypt_failure";
      case stop_reason_e::bad_encrypted_type:
        return "bad_encrypted_type";
      case stop_reason_e::video_thread_exit:
        return "video_thread_exit";
      case stop_reason_e::audio_thread_exit:
        return "audio_thread_exit";
      case stop_reason_e::app_exited_before_peer:
        return "app_exited_before_peer";
      case stop_reason_e::admission_retired:
        return "admission_retired";
      case stop_reason_e::rtsp_clear_all:
        return "rtsp_clear_all";
      case stop_reason_e::rtsp_broadcast_shutdown:
        return "rtsp_broadcast_shutdown";
      case stop_reason_e::rtsp_reap:
        return "rtsp_reap";
      case stop_reason_e::client_disconnect_request:
        return "client_disconnect_request";
      case stop_reason_e::explicit_cancel:
        return "explicit_cancel";
      case stop_reason_e::count_:
        break;
    }
    return "unknown";
  }

  /// First 8 characters of a client UUID: enough to correlate log lines, never the full identifier.
  constexpr std::string_view short_client_id(std::string_view uuid) noexcept {
    return uuid.substr(0, 8);
  }

  /// The final path component of a std::source_location::file_name().
  constexpr std::string_view file_basename(std::string_view path) noexcept {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
  }

  /// Client identity of a session, captured when it is allocated.
  struct identity_t {
    std::uint32_t launch_session_id;
    std::string client_name;
    std::string client_uuid;
  };

  /// "id=<launch id> client='<name>' uuid=<short uuid>": the one shape every session log line uses.
  inline std::string describe(const identity_t &who) {
    std::string out = "id=" + std::to_string(who.launch_session_id) + " client='" + who.client_name + "' uuid=";
    out += short_client_id(who.client_uuid);
    return out;
  }

}  // namespace stream::session
