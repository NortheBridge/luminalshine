/**
 * @file src/platform/windows/nvidia_codec_support.h
 * @brief Conservative NVIDIA codec-generation capability checks.
 */
#pragma once

#include <cstdint>

namespace platf::nvidia {

  /**
   * NVIDIA Ampere (RTX 30 / GA10x) exposes AV1 decode but not AV1 encode.
   * Ada Lovelace and later generations expose AV1 encode. NVIDIA's PCI
   * device-ID allocation places Ampere below 0x2680 and Ada at/above 0x2680.
   * Keep this helper pure so the generation boundary is regression-tested
   * without requiring a GPU or loading the NVENC DLL.
   */
  constexpr bool supports_av1_encode(std::uint32_t device_id) {
    return device_id >= 0x2680;
  }

}  // namespace platf::nvidia
