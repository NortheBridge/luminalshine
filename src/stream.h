/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <atomic>
#include <optional>
#include <string>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "thread_safe.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool lossless_scaling_framegen;
    std::string frame_generation_provider;
    std::optional<int> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;
  };

  namespace session {
    /// Count of sessions whose threads (video/audio/control) have not yet been joined.
    /// Decremented at the end of session::join, AFTER all per-session threads finish.
    /// During a wedged teardown (e.g., NVENC/DXGI hang on driver failure) this counter
    /// stays > 0 even though the session is logically dead. Use active_sessions for
    /// "is any session in the RUNNING state right now?" instead.
    extern std::atomic_uint running_sessions;

    /// Count of sessions in state_e::RUNNING. Decrements as soon as session::stop
    /// transitions the state to STOPPING — independent of whether the per-session
    /// threads have actually exited yet. This is the right counter to consult when
    /// deciding whether downstream subsystems (e.g. the virtual-display recovery
    /// monitor) should still treat a session as live, because it responds immediately
    /// to a client disconnect even if the videoThread is wedged inside dxgi/NVENC.
    extern std::atomic_uint active_sessions;

    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);
  }  // namespace session

  void cancel_paused_display_cleanup();
  void request_idr_for_all_sessions();
}  // namespace stream
