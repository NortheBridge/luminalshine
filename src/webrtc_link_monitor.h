/**
 * @file src/webrtc_link_monitor.h
 * @brief Decision helper for tearing a WebRTC session down when the browser link is lost.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>

namespace webrtc_stream {
  /**
   * @brief Liveness of the browser link, normalized from the peer-connection and ICE observers.
   *
   * libwebrtc reports two overlapping state machines (RTCPeerConnection.connectionState and
   * iceConnectionState). Both are folded onto this one scale so a single monitor can reason
   * about them; the wrapper's raw values are mapped in webrtc_stream.cpp.
   */
  enum class link_state_e {
    unknown,  ///< Nothing reported yet.
    connecting,  ///< new / checking / connecting: negotiation in progress.
    connected,  ///< connected / completed: media can flow.
    disconnected,  ///< Consent checks are failing; ICE may still recover on its own.
    failed,  ///< ICE gave up. The peer is unreachable and will not recover.
    closed,  ///< The peer connection was closed. Only ever locally initiated; ignored.
  };

  /**
   * @brief What the session owner must do after a link-state report.
   */
  enum class link_action_e {
    none,  ///< Nothing to do.
    start_grace_timer,  ///< Re-check with link_monitor_t::grace_expired() after kLinkDisconnectGrace.
    close_session,  ///< Tear the session down now.
  };

  /**
   * @brief How long a link may stay disconnected before the session is torn down.
   *
   * ICE flags a link disconnected within a few seconds of the browser vanishing and
   * usually recovers a brief Wi-Fi hiccup on its own; anything longer than this is a
   * gone client, not a hiccup.
   */
  constexpr std::chrono::milliseconds kLinkDisconnectGrace {10000};

  /**
   * @brief Human-readable name of a link state, for logging.
   */
  const char *link_state_name(link_state_e state);

  /**
   * @brief Pure decision helper: folds link-state reports into one teardown decision.
   *
   * Not thread-safe; the caller serializes access. Every state change bumps an epoch so a
   * grace timer armed for an earlier disconnect can tell that it has been superseded.
   */
  class link_monitor_t {
  public:
    /**
     * @brief Record a link-state report.
     * @return The action the caller must take. Duplicate reports (both observers agreeing),
     *         `unknown`, and anything after the monitor decided to close return `none`.
     */
    link_action_e report(link_state_e state);

    /**
     * @brief Called when a grace timer armed by report() fires.
     * @param epoch The epoch returned by epoch() right after the report that armed the timer.
     * @return true when the link has not come back since that report (still disconnected, or
     *         stuck in an ICE restart) and the session must close.
     */
    bool grace_expired(std::uint64_t epoch);

    link_state_e state() const {
      return state_;
    }

    std::uint64_t epoch() const {
      return epoch_;
    }

    /// True once report() or grace_expired() asked for the session to close.
    bool closing() const {
      return closing_;
    }

  private:
    link_state_e state_ = link_state_e::unknown;
    std::uint64_t epoch_ = 0;
    bool grace_armed_ = false;
    bool closing_ = false;
  };
}  // namespace webrtc_stream
