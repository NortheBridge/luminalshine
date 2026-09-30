#pragma once

#include <cstdint>

namespace pyrowave {
  inline constexpr int kCodec = 3;
  inline constexpr int kDefaultBitrateKbps = 700'000;
  inline constexpr int kMaxBitrateMbps = 10'000;
  // Aurora 2c574a9 / moonlight-common-c a5228fa: SDR and HDR10 4:2:0.
  inline constexpr std::uint32_t kServerSdr = 0x00800000;
  inline constexpr std::uint32_t kServerHdr = 0x02000000;

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
  constexpr bool valid_session(int width, int height, int fps, int kbps, int packet_size) {
    return width >= 128 && width <= 8192 && height >= 128 && height <= 8192 &&
           !(width & 1) && !(height & 1) && fps >= 1 && fps <= 240 &&
           kbps >= 1 && kbps <= 10'000'000 && max_frame_bytes(packet_size) > 0 &&
           frame_budget_bytes(kbps, fps) <= max_frame_bytes(packet_size);
  }
}
