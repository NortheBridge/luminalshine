/**
 * @file src/control_termination_policy.h
 * @brief Whether a stopping session's control peer is told the session ended gracefully.
 */
#pragma once

#include "session_stop_reason.h"

#include <cstdint>

namespace stream::control_termination {

  /// IDX_TERMINATION code that tells a Moonlight-family client the host ended the stream on purpose.
  constexpr std::uint32_t graceful_termination_code = 0x80030023;

  constexpr bool should_send_graceful_termination(session::stop_reason_e, bool) noexcept {
    return false;
  }

}  // namespace stream::control_termination
