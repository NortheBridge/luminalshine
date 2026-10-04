#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>

namespace pyrowave {
  inline constexpr int kCodec = 3;
  inline constexpr int kDefaultBitrateKbps = 700'000;
  inline constexpr int kMaxBitrateMbps = 10'000;
  // Aurora 2c574a9 / moonlight-common-c a5228fa profile bits. Keep these
  // separate from the ordinary Moonlight codec bits: PyroWave is an explicit
  // format-3 dispatch and must never grow the legacy three-codec arrays.
  inline constexpr std::uint32_t kServerSdr420 = 0x00800000;
  inline constexpr std::uint32_t kServerSdr444 = 0x01000000;
  inline constexpr std::uint32_t kServerHdr420 = 0x02000000;
  inline constexpr std::uint32_t kServerHdr444 = 0x04000000;
  // Source-compatible aliases retained for the initial 4:2:0 integration.
  inline constexpr std::uint32_t kServerSdr = kServerSdr420;
  inline constexpr std::uint32_t kServerHdr = kServerHdr420;

  enum class profile_e : std::uint8_t {
    sdr420,
    sdr444,
    hdr420,
    hdr444,
  };
  using profile_mask_t = std::uint8_t;

  constexpr profile_e profile(bool hdr, bool chroma444) {
    return hdr ? (chroma444 ? profile_e::hdr444 : profile_e::hdr420) :
                 (chroma444 ? profile_e::sdr444 : profile_e::sdr420);
  }
  constexpr profile_mask_t profile_bit(profile_e value) {
    return profile_mask_t {1u} << static_cast<unsigned>(value);
  }
  constexpr bool profile_mask_supports(profile_mask_t mask, bool hdr, bool chroma444) {
    return (mask & profile_bit(profile(hdr, chroma444))) != 0;
  }
  constexpr std::uint32_t profile_server_bit(profile_e value) {
    switch (value) {
      case profile_e::sdr420: return kServerSdr420;
      case profile_e::sdr444: return kServerSdr444;
      case profile_e::hdr420: return kServerHdr420;
      case profile_e::hdr444: return kServerHdr444;
    }
    return 0;
  }
  constexpr std::uint32_t server_codec_mask_for_profiles(profile_mask_t mask, bool allow_yuv444) {
    std::uint32_t result = 0;
    for (const auto value : {profile_e::sdr420, profile_e::sdr444, profile_e::hdr420, profile_e::hdr444}) {
      const bool is_444 = value == profile_e::sdr444 || value == profile_e::hdr444;
      if ((!is_444 || allow_yuv444) && (mask & profile_bit(value))) {
        result |= profile_server_bit(value);
      }
    }
    return result;
  }

  // ANNOUNCE extensions are deliberately strict. Missing attributes are
  // distinguished by the caller; a present value is valid only when it is an
  // exact protocol 0 or 1 (no whitespace, signs, or textual booleans).
  constexpr std::optional<bool> parse_wire_bool(std::string_view value) {
    if (value == "0") return false;
    if (value == "1") return true;
    return std::nullopt;
  }

  struct frame_fec_status_t {
    std::uint32_t frame_index = 0;
    std::uint16_t highest_received_sequence = 0;
    std::uint16_t next_contiguous_sequence = 0;
    std::uint16_t missing_before_highest = 0;
    std::uint16_t total_data_packets = 0;
    std::uint16_t total_parity_packets = 0;
    std::uint16_t received_data_packets = 0;
    std::uint16_t received_parity_packets = 0;
    std::uint8_t fec_percentage = 0;
    std::uint8_t block_index = 0;
    std::uint8_t block_count = 0;

    constexpr bool has_loss() const {
      return missing_before_highest != 0 ||
             std::uint32_t {received_data_packets} + received_parity_packets <
               std::uint32_t {total_data_packets} + total_parity_packets;
    }
    constexpr bool unrecoverable() const {
      return std::uint32_t {received_data_packets} + received_parity_packets <
             total_data_packets;
    }
  };
  inline constexpr std::size_t kFrameFecStatusWireSize = 21;

  /** Parse the pinned client's packed, big-endian SS_FRAME_FEC_STATUS. */
  constexpr std::optional<frame_fec_status_t> parse_frame_fec_status(std::string_view payload) {
    if (payload.size() != kFrameFecStatusWireSize) return std::nullopt;
    const auto byte = [&](std::size_t offset) {
      return static_cast<std::uint8_t>(payload[offset]);
    };
    const auto be16 = [&](std::size_t offset) {
      return static_cast<std::uint16_t>((std::uint16_t {byte(offset)} << 8) |
                                        std::uint16_t {byte(offset + 1)});
    };
    const auto be32 = [&](std::size_t offset) {
      return (std::uint32_t {byte(offset)} << 24) |
             (std::uint32_t {byte(offset + 1)} << 16) |
             (std::uint32_t {byte(offset + 2)} << 8) |
             std::uint32_t {byte(offset + 3)};
    };
    frame_fec_status_t result {
      .frame_index = be32(0),
      .highest_received_sequence = be16(4),
      .next_contiguous_sequence = be16(6),
      .missing_before_highest = be16(8),
      .total_data_packets = be16(10),
      .total_parity_packets = be16(12),
      .received_data_packets = be16(14),
      .received_parity_packets = be16(16),
      .fec_percentage = byte(18),
      .block_index = byte(19),
      .block_count = byte(20),
    };
    if (result.total_data_packets == 0 ||
        result.received_data_packets > result.total_data_packets ||
        result.received_parity_packets > result.total_parity_packets ||
        result.block_count < 1 || result.block_count > 4 ||
        result.block_index >= result.block_count) {
      return std::nullopt;
    }
    return result;
  }

  constexpr bool frame_fec_has_loss(const frame_fec_status_t &status) {
    return status.has_loss();
  }
  constexpr bool frame_fec_unrecoverable(const frame_fec_status_t &status) {
    return status.unrecoverable();
  }

  constexpr std::uint64_t bitrate_kbps(int mbps) {
    return mbps < 0 || mbps > kMaxBitrateMbps ? 0 :
           mbps == 0 ? kDefaultBitrateKbps : std::uint64_t(mbps) * 1000;
  }
  constexpr std::uint64_t frame_budget_bytes(int kbps, int fps) {
    return kbps <= 0 || fps <= 0 ? 0 : std::uint64_t(kbps) * 1000 / (std::uint64_t(fps) * 8);
  }
  constexpr std::uint64_t max_frame_bytes(int packet_size) {
    // Reserve more than the actual RTP/video headers and frame prefix. Keep
    // each of the four legacy FEC blocks strictly below its 10-bit index limit.
    return packet_size < 256 || packet_size > 2048 ? 0 :
           std::uint64_t(packet_size - 128) * (4 * 1023) - 1024;
  }
  constexpr std::uint64_t max_target_bytes(int packet_size) {
    const auto hard_max = max_frame_bytes(packet_size);
    // The RDO target describes coded coefficients, while the transmitted frame
    // also carries sequence/block headers and payload-boundary padding. Keep a
    // fixed 25% envelope for that overhead so a target at the admitted boundary
    // cannot deterministically fail the hard post-encode transport check.
    return hard_max - hard_max / 4;
  }
  constexpr bool valid_session(int width, int height, int fps, int kbps, int packet_size) {
    return width >= 128 && width <= 8192 && height >= 128 && height <= 8192 &&
           !(width & 1) && !(height & 1) && fps >= 1 && fps <= 240 &&
           kbps >= 1 && kbps <= 10'000'000 && max_target_bytes(packet_size) > 0 &&
           frame_budget_bytes(kbps, fps) <= max_target_bytes(packet_size);
  }

  /**
   * Scale an RDO frame budget without integer overflow and never let the
   * encoder target exceed the negotiated transport bound. The scale bounds
   * mirror the session-local controller: bitrate backs off to at most 50%, and
   * replenishment savings can spend at most 2x on quality.
   */
  inline std::uint64_t adaptive_frame_budget_bytes(
    std::uint64_t base_bytes,
    double bitrate_scale,
    double budget_scale,
    std::uint64_t max_bytes
  ) {
    if (base_bytes == 0 || max_bytes == 0) return 0;
    if (!std::isfinite(bitrate_scale)) bitrate_scale = 1.0;
    if (!std::isfinite(budget_scale)) budget_scale = 1.0;
    bitrate_scale = std::clamp(bitrate_scale, 0.5, 1.0);
    budget_scale = std::clamp(budget_scale, 1.0, 2.0);
    const long double scaled = static_cast<long double>(base_bytes) *
                               static_cast<long double>(bitrate_scale) *
                               static_cast<long double>(budget_scale);
    if (scaled >= static_cast<long double>(max_bytes)) return max_bytes;
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(scaled));
  }

  inline int adaptive_pacing_bitrate_kbps(int base_kbps, double bitrate_scale) {
    if (base_kbps <= 0) return 0;
    if (!std::isfinite(bitrate_scale)) bitrate_scale = 1.0;
    bitrate_scale = std::clamp(bitrate_scale, 0.5, 1.0);
    const long double scaled = static_cast<long double>(base_kbps) *
                               static_cast<long double>(bitrate_scale);
    return std::max(1, static_cast<int>(scaled));
  }

  // Valid client FEC-status loss reports drive the session-local RTP FEC
  // controller. Each newly affected frame adds four percentage points, capped
  // at a 16-point boost. The boost expires ten seconds after loss and the
  // effective percentage never drops below the host's configured base.
  constexpr int next_adaptive_fec_boost(int current) {
    const int bounded = std::clamp(current, 0, 16);
    return bounded >= 12 ? 16 : bounded + 4;
  }
  constexpr int effective_fec_percentage(int base, int boost, std::int64_t elapsed_since_loss_ms) {
    const int valid_base = std::max(base, 0);
    if (elapsed_since_loss_ms >= 10'000 || boost <= 0 || valid_base >= 25) return valid_base;
    return valid_base + std::min(boost, 25 - valid_base);
  }

  /** Preserve the negotiated wire-rate budget when adaptive FEC adds parity. */
  constexpr int fec_video_scale_percent(int base, int effective) {
    const auto valid_base = std::clamp(base, 0, 1'000'000);
    const auto valid_effective = std::clamp(effective, valid_base, 1'000'000);
    if (valid_effective <= valid_base) return 100;
    const auto numerator = std::int64_t {100} * (100 + valid_base);
    // Floor deliberately so integer packetization cannot overshoot the
    // session's original video-plus-parity budget.
    return std::clamp(static_cast<int>(numerator / (100 + valid_effective)), 1, 100);
  }

  constexpr std::uint64_t apply_video_scale_percent(std::uint64_t value, int percent) {
    const auto bounded = static_cast<std::uint64_t>(std::clamp(percent, 1, 100));
    // Quotient/remainder form avoids overflowing value * percent.
    return (value / 100) * bounded + ((value % 100) * bounded) / 100;
  }

  constexpr int apply_video_scale_percent(int value, int percent) {
    if (value <= 0) return 0;
    const auto scaled = apply_video_scale_percent(static_cast<std::uint64_t>(value), percent);
    return static_cast<int>(std::min<std::uint64_t>(scaled, std::numeric_limits<int>::max()));
  }

  struct adaptive_fec_update_t {
    int fec_percentage = 0;
    int video_scale_percent = 100;
    bool scale_changed = false;
  };

  /** Thread-safe per-session FEC controller shared by feedback and egress. */
  class adaptive_fec_controller_t {
  public:
    void set_base(int base) {
      std::lock_guard lock {mutex_};
      base_ = std::max(base, 0);
      boost_ = 0;
      last_loss_ms_ = 0;
      has_loss_ = false;
      last_scale_ = 100;
    }

    adaptive_fec_update_t report_loss(std::int64_t now_ms) {
      std::lock_guard lock {mutex_};
      last_loss_ms_ = now_ms;
      has_loss_ = true;
      boost_ = next_adaptive_fec_boost(boost_);
      return snapshot_locked(0);
    }

    adaptive_fec_update_t sample(std::int64_t now_ms) {
      std::lock_guard lock {mutex_};
      const auto elapsed_ms = has_loss_ ?
        std::max<std::int64_t>(0, now_ms - last_loss_ms_) : 10'000;
      auto result = snapshot_locked(elapsed_ms);
      if (elapsed_ms >= 10'000) boost_ = 0;
      return result;
    }

  private:
    adaptive_fec_update_t snapshot_locked(std::int64_t elapsed_ms) {
      adaptive_fec_update_t result;
      result.fec_percentage = effective_fec_percentage(base_, boost_, elapsed_ms);
      result.video_scale_percent = fec_video_scale_percent(base_, result.fec_percentage);
      result.scale_changed = result.video_scale_percent != last_scale_;
      last_scale_ = result.video_scale_percent;
      return result;
    }

    std::mutex mutex_;
    int base_ = 0;
    int boost_ = 0;
    int last_scale_ = 100;
    std::int64_t last_loss_ms_ = 0;
    bool has_loss_ = false;
  };
}
