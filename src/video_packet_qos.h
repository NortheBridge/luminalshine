/**
 * @file src/video_packet_qos.h
 * @brief Decoder-safe admission for encoded video packets.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stream::video_qos {
  // 50% headroom keeps the drain comfortably above the encoder's real output
  // (IDR bursts included) while still shaping traffic a 100-Mbps client path
  // can absorb — the per-packet wire interval stays the shape, only its
  // slope rises. Raised from 1.35 in the 2026-08-07 standing-latency round:
  // transient backlogs (scene-change IDR bursts) drained at only +35% of
  // saturated production, stretching every excursion above the 250 ms age
  // floor; +50% clears the same backlog ~1.4x faster with identical
  // batching and quantum. A 1-ms quantum tolerates ordinary Windows timer
  // wake latency; a finer quantum makes sustained throughput hostage to the
  // waitable-timer resolution (the high-resolution flag is not guaranteed
  // on all builds).
  inline constexpr long double kWirePacingHeadroom = 1.50L;
  inline constexpr long double kMaxWireDrainBitrateBps = 800'000'000.0L;
  inline constexpr std::chrono::microseconds kWirePacingQuantum {1000};
  inline constexpr std::chrono::seconds kRecoveryRequestRetry {2};

  struct pacing_budget_t {
    long double average_wire_bitrate_bps = 0;
    long double drain_bitrate_bps = 0;
    std::chrono::duration<long double> packet_interval {};
    std::size_t packets_per_quantum = 1;
  };

  inline pacing_budget_t pacing_budget(
    int payload_bitrate_kbps,
    int fec_percentage,
    std::size_t wire_packet_bytes,
    std::size_t payload_packet_bytes
  ) {
    const auto safe_payload_bytes = std::max<std::size_t>(payload_packet_bytes, 1);
    const auto safe_wire_bytes = std::max<std::size_t>(wire_packet_bytes, 1);
    const long double average =
      static_cast<long double>(std::max(payload_bitrate_kbps, 1)) * 1000.0L *
      (100.0L + static_cast<long double>(std::max(fec_percentage, 0))) / 100.0L *
      static_cast<long double>(safe_wire_bytes) /
      static_cast<long double>(safe_payload_bytes);
    const long double drain = std::min(
      average * kWirePacingHeadroom,
      kMaxWireDrainBitrateBps
    );
    const long double packet_bits = static_cast<long double>(safe_wire_bytes) * 8.0L;
    const auto interval = std::chrono::duration<long double> {packet_bits / drain};
    const auto quantum_seconds = std::chrono::duration<long double> {kWirePacingQuantum}.count();
    const auto packets_per_quantum = std::max<std::size_t>(
      1,
      static_cast<std::size_t>(std::ceil(drain * quantum_seconds / packet_bits))
    );
    return {
      .average_wire_bitrate_bps = average,
      .drain_bitrate_bps = drain,
      .packet_interval = interval,
      .packets_per_quantum = packets_per_quantum,
    };
  }

  inline std::uint64_t next_rtp_timestamp_ticks(
    std::uint64_t candidate,
    std::uint64_t last,
    int framerate
  ) {
    if (candidate > last) {
      return candidate;
    }

    const auto nominal_step = std::max<std::uint64_t>(
      1,
      90'000u / static_cast<std::uint64_t>(std::max(framerate, 1))
    );
    return last + nominal_step;
  }

  enum class reason_e {
    none,
    waiting_for_idr,
    frame_discontinuity,
    stale_frame,
  };

  struct decision_t {
    bool submit = false;
    bool request_idr = false;
    reason_e reason = reason_e::none;
  };

  struct recovery_t {
    std::uint64_t withheld_frames = 0;
    std::chrono::steady_clock::duration duration {};
  };

  struct state_t {
    bool awaiting_idr = true;
    bool recovery_requested = false;
    std::optional<std::int64_t> last_submitted_frame;
    std::chrono::steady_clock::time_point recovery_started {};
    std::chrono::steady_clock::time_point last_recovery_request {};
    std::uint64_t withheld_frames = 0;
  };

  /**
   * Decide whether an encoded packet may enter the transport.
   *
   * A predictive frame is useful only when every preceding encoded reference
   * was submitted. If a mailbox, worker pipe, or deadline loses one packet,
   * withholding the rest of that GOP is the only decoder-safe response.
   */
  inline decision_t evaluate(
    state_t &state,
    std::int64_t frame_index,
    bool is_idr,
    bool stale,
    std::chrono::steady_clock::time_point now
  ) {
    if (is_idr) {
      return {.submit = true};
    }

    reason_e reason = reason_e::none;
    if (state.awaiting_idr || !state.last_submitted_frame) {
      reason = reason_e::waiting_for_idr;
    } else if (frame_index != *state.last_submitted_frame + 1) {
      reason = reason_e::frame_discontinuity;
    } else if (stale) {
      reason = reason_e::stale_frame;
    }

    if (reason == reason_e::none) {
      return {.submit = true};
    }

    if (!state.awaiting_idr || state.recovery_started == std::chrono::steady_clock::time_point {}) {
      state.recovery_started = now;
    }
    state.awaiting_idr = true;
    ++state.withheld_frames;

    const bool request_idr = !state.recovery_requested ||
      state.last_recovery_request == std::chrono::steady_clock::time_point {} ||
      now - state.last_recovery_request >= kRecoveryRequestRetry;
    state.recovery_requested = true;
    if (request_idr) {
      state.last_recovery_request = now;
    }
    return {
      .submit = false,
      .request_idr = request_idr,
      .reason = reason,
    };
  }

  /** Record a packet only after all of its UDP shards have been submitted. */
  inline std::optional<recovery_t> submitted(
    state_t &state,
    std::int64_t frame_index,
    bool is_idr,
    std::chrono::steady_clock::time_point now
  ) {
    state.last_submitted_frame = frame_index;
    if (!is_idr) {
      return std::nullopt;
    }

    std::optional<recovery_t> recovery;
    if (state.awaiting_idr) {
      recovery = recovery_t {
        .withheld_frames = state.withheld_frames,
        .duration = state.recovery_started == std::chrono::steady_clock::time_point {} ?
                      std::chrono::steady_clock::duration {} :
                      now - state.recovery_started,
      };
    }
    state.awaiting_idr = false;
    state.recovery_requested = false;
    state.recovery_started = {};
    state.last_recovery_request = {};
    state.withheld_frames = 0;
    return recovery;
  }

  /** Re-arm recovery when an admitted IDR fails before transport submission. */
  inline void submission_failed(state_t &state, bool is_idr) {
    if (is_idr) {
      state.awaiting_idr = true;
      state.recovery_requested = false;
      state.last_recovery_request = {};
    }
  }
}  // namespace stream::video_qos
