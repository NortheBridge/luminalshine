/**
 * @file src/webrtc_stream.h
 * @brief Declarations for WebRTC session tracking and frame handoff.
 */
#pragma once

// standard includes
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "audio.h"
#include "video.h"

namespace webrtc_stream {
  struct SessionOptions {
    bool audio = true;
    bool video = true;
    bool encoded = true;
    bool host_audio = false;

    std::optional<std::string> client_name;
    std::optional<std::string> client_uuid;

    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> fps;
    std::optional<int> bitrate_kbps;
    std::optional<std::string> codec;
    std::optional<bool> hdr;
    std::optional<int> audio_channels;
    std::optional<std::string> audio_codec;
    std::optional<std::string> profile;
    std::optional<int> app_id;
    std::optional<bool> resume;
    std::optional<std::string> video_pacing_mode;
    std::optional<int> video_pacing_slack_ms;
    std::optional<int> video_max_frame_age_ms;
  };

  struct SessionState {
    std::string id;
    bool audio = true;
    bool video = true;
    bool encoded = true;

    std::optional<std::string> client_name;
    std::optional<std::string> client_uuid;

    std::uint64_t audio_packets = 0;
    std::uint64_t video_packets = 0;
    std::uint64_t audio_dropped = 0;
    std::uint64_t video_dropped = 0;
    std::size_t audio_queue_frames = 0;
    std::size_t video_queue_frames = 0;
    std::uint32_t video_inflight_frames = 0;
    bool has_remote_offer = false;
    bool has_local_answer = false;
    std::size_t ice_candidates = 0;

    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> fps;
    std::optional<int> bitrate_kbps;
    std::optional<std::string> codec;
    std::optional<bool> hdr;
    std::optional<int> audio_channels;
    std::optional<int> negotiated_audio_channels;
    std::optional<std::string> audio_codec;
    std::optional<std::string> profile;
    std::optional<std::string> video_pacing_mode;
    std::optional<int> video_pacing_slack_ms;
    std::optional<int> video_max_frame_age_ms;

    std::optional<std::chrono::steady_clock::time_point> last_audio_time;
    std::optional<std::chrono::steady_clock::time_point> last_video_time;

    std::size_t last_audio_bytes = 0;
    std::size_t last_video_bytes = 0;
    bool last_video_idr = false;
    std::int64_t last_video_frame_index = 0;
  };

  bool has_active_sessions();

  std::optional<SessionState> create_session(const SessionOptions &options);
  std::optional<std::string> ensure_capture_started(const SessionOptions &options);
  bool close_session(std::string_view id);
  std::optional<SessionState> get_session(std::string_view id);
  std::vector<SessionState> list_sessions();
  void shutdown_all_sessions();

  void cancel_paused_display_cleanup();

  /**
   * @brief Signal process teardown to any in-flight paused-display cleanup thread.
   *
   * Mirrors stream::notify_shutdown(): sets a sticky flag so the detached cleanup thread bails out
   * without logging through Boost.Log after the sinks are torn down during CRT exit.
   */
  void notify_shutdown();
  void submit_video_packet(video::packet_raw_t &packet);
  void submit_audio_packet(const audio::buffer_t &packet);
  void submit_video_frame(const std::shared_ptr<platf::img_t> &frame);
  void submit_audio_frame(const std::vector<float> &samples, int sample_rate, int channels, int frames);

  /**
   * @brief Outcome of rewriting an answer's audio parameters.
   */
  struct opus_sdp_result_t {
    std::string sdp;  ///< The rewritten SDP.
    int channels = 2;  ///< Channel count actually negotiated with the peer.
    bool multiopus = false;  ///< True when a multichannel Opus payload type was selected.
  };

  /**
   * @brief Rewrite an answer SDP so the Opus payload matches our capture layout.
   *
   * Browsers never offer multichannel Opus on their own -- libwebrtc registers
   * `AudioEncoderMultiChannelOpus` as `NotAdvertised`, so it is usable but is
   * left out of `GetSupportedEncoders()`. A peer that wants surround has to put
   * a `multiopus/48000/<channels>` payload in its *offer*; this function then
   * selects it, promotes it to the front of the m-line so it wins the
   * negotiation, and restates the stream layout authoritatively from
   * `audio::stream_configs` -- the same table `audio::capture()` feeds the
   * native encoder, so both ends describe the PCM we actually push.
   *
   * When the peer offered no matching multiopus payload the rewrite falls back
   * to stereo and reports `channels == 2`, which is the caller's cue to fold
   * the captured surround down before pushing it.
   *
   * @param sdp The answer SDP to rewrite.
   * @param channels Requested channel count (2 for stereo, 6 for 5.1, 8 for 7.1).
   * @return The rewritten SDP plus the layout that was actually negotiated.
   */
  opus_sdp_result_t apply_opus_audio_params(std::string_view sdp, int channels);

  void set_rtsp_sessions_active(bool active);
  void set_rtsp_capture_config(const video::config_t &video_config, const audio::config_t &audio_config);

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type);
  bool add_ice_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate);
  bool set_local_answer(std::string_view id, const std::string &sdp, const std::string &type);
  bool get_local_answer(std::string_view id, std::string &sdp_out, std::string &type_out);
  bool wait_for_local_answer(
    std::string_view id,
    std::string &sdp_out,
    std::string &type_out,
    std::chrono::milliseconds timeout
  );
  bool add_local_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate);

  struct IceCandidateInfo {
    std::string mid;
    int mline_index = -1;
    std::string candidate;
    std::size_t index = 0;
  };

  std::vector<IceCandidateInfo> get_local_candidates(std::string_view id, std::size_t since);

  std::string get_server_cert_fingerprint();
  std::string get_server_cert_pem();
}  // namespace webrtc_stream
