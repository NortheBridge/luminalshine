/**
 * @file src/platform/windows/nvidia_codec_support.h
 * @brief Conservative NVIDIA codec-generation capability checks.
 */
#pragma once

#include <cstdint>

namespace platf::nvidia {

  /**
   * @brief First NVIDIA PCI device ID belonging to an AV1-encode generation.
   *
   * Ampere (RTX 30 / GA10x) exposes AV1 *decode* but not AV1 *encode*; Ada
   * Lovelace and later expose both. Observed allocation either side of this
   * boundary, checked against pciutils `pci.ids`:
   *
   *   - highest Ampere display part: 0x25FB (RTX A500 Embedded)
   *   - 0x2600-0x267F: unallocated
   *   - lowest Ada part:              0x2681 (TITAN Ada)
   *   - lowest shipping Ada part:     0x2684 (RTX 4090)
   *
   * Note the margin below the boundary is a single device ID. That ordering
   * is an observed NVIDIA convention, not a documented contract, so this is
   * deliberately a `>=` test rather than an enumerated allowlist: an ID above
   * the boundary that turns out to lack AV1 encode still degrades gracefully
   * (the NVENC codec-GUID query rejects it and the probe moves on), whereas an
   * allowlist that has not been taught about a newly-released GPU would deny
   * AV1 to hardware that supports it. Prefer the failure that costs nothing.
   *
   * Known parts on the wrong side of that trade, all of them unreachable via
   * DXGI display-adapter enumeration in practice: datacenter Blackwell
   * (GB100/GB110, 0x2900-0x31FE) and Rubin (0x3000+) carry no NVENC at all,
   * and Jetson AGX Thor (0x2B00) had its AV1 encoder removed in hardware.
   */
  inline constexpr std::uint32_t kFirstAv1EncodeDeviceId = 0x2680;

  /**
   * @brief Whether an NVIDIA GPU's PCI device ID belongs to a generation with
   *        hardware AV1 encode.
   *
   * This is a fast path, not a safety guard: it keeps a codec the GPU is known
   * not to have out of the encoder probe, sparing the probe an encode session
   * that can only be rejected. Correctness does not depend on it — NVENC is
   * asked directly via `nvEncGetEncodeGUIDs()` before any encoder is created,
   * and that query is authoritative for driver-level support this table cannot
   * see (a new GPU on an old driver, for instance).
   *
   * Kept pure so the boundary is regression-tested without a GPU or the NVENC
   * DLL. The caller supplies `DXGI_ADAPTER_DESC::DeviceId` and is responsible
   * for having already matched NVIDIA's vendor ID (0x10DE).
   */
  constexpr bool supports_av1_encode(std::uint32_t device_id) {
    return device_id >= kFirstAv1EncodeDeviceId;
  }

}  // namespace platf::nvidia
