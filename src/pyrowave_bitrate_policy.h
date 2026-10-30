/**
 * @file src/pyrowave_bitrate_policy.h
 * @brief Pure decision logic for "which compressed-video bitrate does a
 * PyroWave session target?"
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>

namespace pyrowave {

  /// Lowest target a client request may select. Matches the wire pacer's own
  /// 1000 kbps floor in stream.cpp, so the encoder never budgets below what
  /// the pacer will shape to.
  inline constexpr std::int64_t kMinClientBitrateKbps = 1'000;

  /**
   * @brief Choose a PyroWave session's compressed-video target.
   *
   * The host setting is the ceiling. A client that sends a positive bitrate
   * lowers the target to that value, raised to @p floor_kbps; a client that
   * sends nothing usable (zero or negative) keeps the host target. An invalid
   * host target (zero or negative) stays invalid so session validation still
   * rejects it.
   *
   * @param client_kbps The client's requested video bitrate, in kbps.
   * @param host_kbps The host's configured PyroWave target, in kbps.
   * @param floor_kbps The lowest target a client request may select.
   * @return The session target in kbps, never above @p host_kbps.
   */
  constexpr std::int64_t session_bitrate_kbps(std::int64_t client_kbps, std::int64_t host_kbps, std::int64_t floor_kbps) {
    if (host_kbps <= 0) {
      return 0;
    }
    if (client_kbps <= 0) {
      return host_kbps;
    }
    return std::min(host_kbps, std::max(client_kbps, floor_kbps));
  }

}  // namespace pyrowave
