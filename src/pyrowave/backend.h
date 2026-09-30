#pragma once

#include "contract.h"
#include "src/video.h"

namespace pyrowave {
#ifdef SUNSHINE_ENABLE_PYROWAVE
  bool available();
  video::encoder_t &encoder();
  std::unique_ptr<platf::encode_device_t> make_device(platf::display_t &display, const video::config_t &config);
  std::unique_ptr<video::encode_session_t> make_session(std::unique_ptr<platf::encode_device_t> device);
  video::packet_t encode(video::encode_session_t &session, std::int64_t frame);
#else
  inline bool available() { return false; }
#endif
}
