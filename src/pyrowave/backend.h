#pragma once

#include "contract.h"
#include "src/video.h"

#include <string_view>

namespace pyrowave {
#ifdef SUNSHINE_ENABLE_PYROWAVE
  /** Entry point for the bounded disposable Windows capability-probe child. */
  int run_profile_probe_child(
    std::string_view encoded_adapter,
    std::string_view encoded_output,
    std::string_view encoded_luid
  ) noexcept;
  /** Explicit startup probe. Snapshot accessors below never initialize Vulkan. */
  void initialize_profile_probe();
  /** Cancel a running disposable probe during process shutdown. */
  void cancel_profile_probe() noexcept;
  bool profile_probe_complete();
  /** Cached mask of independently probed SDR/HDR and 4:2:0/4:4:4 profiles. */
  profile_mask_t profile_mask();
  bool supports_profile(bool hdr, bool chroma444);
  std::uint32_t server_codec_mask(bool allow_yuv444);
  bool available();
  video::encoder_t &encoder();
  std::unique_ptr<platf::encode_device_t> make_device(platf::display_t &display, const video::config_t &config);
  std::unique_ptr<video::encode_session_t> make_session(std::unique_ptr<platf::encode_device_t> device);
  video::packet_t encode(video::encode_session_t &session, std::int64_t frame);
#else
  inline int run_profile_probe_child(std::string_view, std::string_view, std::string_view) noexcept { return 9; }
  inline void initialize_profile_probe() {}
  inline void cancel_profile_probe() noexcept {}
  inline bool profile_probe_complete() { return false; }
  inline profile_mask_t profile_mask() { return 0; }
  inline bool supports_profile(bool, bool) { return false; }
  inline std::uint32_t server_codec_mask(bool) { return 0; }
  inline bool available() { return false; }
#endif
}
