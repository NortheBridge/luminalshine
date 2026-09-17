/**
 * @file src/webrtc_link_monitor.cpp
 * @brief Definitions for the WebRTC link-loss teardown decision helper.
 */
// local includes
#include "webrtc_link_monitor.h"

namespace webrtc_stream {
  const char *link_state_name(link_state_e state) {
    switch (state) {
      case link_state_e::unknown:
        return "unknown";
      case link_state_e::connecting:
        return "connecting";
      case link_state_e::connected:
        return "connected";
      case link_state_e::disconnected:
        return "disconnected";
      case link_state_e::failed:
        return "failed";
      case link_state_e::closed:
        return "closed";
    }
    return "unknown";
  }

  link_action_e link_monitor_t::report(link_state_e state) {
    if (closing_ || state == state_ || state == link_state_e::unknown) {
      return link_action_e::none;
    }
    // An ICE restart while the link is down goes disconnected -> checking. That is
    // not a recovery, so the grace timer armed by the disconnect stays valid: only a
    // genuine state change bumps the epoch.
    const bool restart_after_loss = state_ == link_state_e::disconnected && state == link_state_e::connecting;
    state_ = state;
    if (!restart_after_loss) {
      ++epoch_;
      grace_armed_ = false;
    }
    switch (state) {
      case link_state_e::failed:
        closing_ = true;
        return link_action_e::close_session;
      case link_state_e::disconnected:
        grace_armed_ = true;
        return link_action_e::start_grace_timer;
      case link_state_e::unknown:
      case link_state_e::connecting:
      case link_state_e::connected:
      case link_state_e::closed:
        break;
    }
    return link_action_e::none;
  }

  bool link_monitor_t::grace_expired(std::uint64_t epoch) {
    // grace_armed_ only survives the disconnected -> connecting (ICE restart) transition;
    // every other change bumps the epoch and disarms it, so a matching epoch here means
    // the link never came back since the report that armed this timer.
    if (closing_ || !grace_armed_ || epoch != epoch_) {
      return false;
    }
    closing_ = true;
    return true;
  }
}  // namespace webrtc_stream
