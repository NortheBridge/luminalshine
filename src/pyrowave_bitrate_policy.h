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
   * @brief Choose the client's video bitrate that caps a PyroWave session.
   *
   * A positive configuredBitrateKbps is the client's selection; its adjusted
   * form has base FEC, audio and overhead removed, which PyroWave adds back on
   * top of its compressed-video target. When that adjustment leaves nothing,
   * the request still stands and selects the floor. Without a positive
   * configured value, a positive maximumBitrateKbps is used as sent.
   *
   * @param raw_configured_kbps configuredBitrateKbps as the client sent it.
   * @param adjusted_configured_kbps configuredBitrateKbps after the adjustment.
   * @param maximum_kbps maximumBitrateKbps as the client sent it.
   * @return The client's video bitrate in kbps, or 0 when the client sent none.
   */
  constexpr std::int64_t client_video_bitrate_kbps(std::int64_t raw_configured_kbps, std::int64_t adjusted_configured_kbps, std::int64_t maximum_kbps) {
    if (raw_configured_kbps > 0) {
      return adjusted_configured_kbps > 0 ? adjusted_configured_kbps : kMinClientBitrateKbps;
    }
    return maximum_kbps > 0 ? maximum_kbps : 0;
  }

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
