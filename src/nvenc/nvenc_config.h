/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

#include <cstdint>

namespace nvenc {

  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  enum class split_encode_mode {
    auto_mode,  ///< Let the NVIDIA driver decide when split-frame encoding should be used
    enabled,  ///< Force split-frame encoding when supported
    disabled,  ///< Disable split-frame encoding even when it would otherwise be auto-enabled
  };

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // Percentage increase of VBV/HRD from the default single frame, allows low-latency variable bitrate
    int vbv_percentage_increase = 0;

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;

    // Min QP value for H.264 when enable_min_qp is selected
    unsigned min_qp_h264 = 19;

    // Min QP value for HEVC when enable_min_qp is selected
    unsigned min_qp_hevc = 23;

    // Min QP value for AV1 when enable_min_qp is selected
    unsigned min_qp_av1 = 23;

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;

    // Control split-frame encoding for supported HEVC/AV1 sessions
    split_encode_mode split_encode_mode = split_encode_mode::auto_mode;

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;

    // HARD deadline for the encode-async-completion wait in encode_frame:
    // the point at which we abandon the frame. NOT a GPU-health verdict —
    // see nvenc_base.cpp's wait loop, which only reports a TDR when the
    // device itself corroborates it.
    //
    // The old value was 100 ms, sized to bound the ENCODE. That was the
    // wrong quantity. The encode is a small minority of this wait: the
    // NVENC input texture doubles as the colour-conversion render target
    // (display_vram.cpp init_output), so the encode packet carries a
    // cross-engine fence on a 3D draw queued behind the game's render
    // work. The wait therefore tracks GAME GPU load, not encoder
    // throughput, and no per-GPU tuning can fix that — a field capture on
    // an RTX 5080 (the fastest supported part) showed p99 frame latency of
    // 271 ms and a legitimate max of 3.9 s with zero GPU resets, while the
    // worst PURE encode on the SLOWEST supported part (Pascal, 2160p,
    // 10-bit, P6, full-res two-pass) is only ~55-60 ms.
    //
    // So the default is anchored to the OS instead of to any GPU:
    // TdrDelay (2 s) + TdrDdiDelay (5 s) = the full documented span from
    // Windows starting to count a stalled packet to abandoning the driver
    // unwind. Past that a real fault is visible via GetDeviceRemovedReason
    // and waiting longer yields nothing. Windows-side callers recompute
    // this from the machine's actual registry values via
    // encode_wait_deadline_from_tdr().
    uint32_t encode_wait_timeout_ms = 7000;

    // Polling granularity for that wait. The wait is sliced so the encode
    // thread observes shutdown (and re-checks device health) at this
    // cadence instead of blocking for the whole deadline. Costs nothing on
    // healthy frames — the completion event returns immediately on signal,
    // so a normal frame is still a single syscall.
    uint32_t encode_wait_poll_slice_ms = 100;

    // Soft telemetry threshold. A wait longer than this logs once and is
    // counted; it NEVER escalates and never ends a session. Rare on fast
    // hardware, progressively more common on slower parts — which is the
    // diagnostic gradient we want out of a universal build.
    uint32_t encode_stall_warn_ms = 500;
  };

  /**
   * @brief Derive the hard encode-wait deadline from Windows' own TDR
   *        constants: (TdrDelay + TdrDdiDelay) seconds, clamped.
   *
   * Lower clamp 3000 ms keeps the deadline above the largest legitimate
   * wait observed in the field even if someone sets TdrDelay=1. Upper
   * clamp 10000 ms exists because TdrLevel=0 or the TdrDelay=60 that
   * overclocking/compute guides recommend (and some OEM images ship) must
   * not turn into a minute-long encode-thread block. 10 s is still safe
   * against the client: moonlight-common-c has no mid-stream video
   * deadline (FIRST_FRAME_TIMEOUT_SEC is gated on the first-frame latches).
   *
   * Pure and constexpr so the clamping is unit-testable without a GPU.
   */
  constexpr uint32_t encode_wait_deadline_from_tdr(uint32_t tdr_delay_s, uint32_t tdr_ddi_delay_s) {
    // Floor sits ABOVE the largest legitimate wait measured in the field
    // (3913 ms on an RTX 5080 with zero GPU resets). A lower floor would
    // let a machine with a small TdrDelay abandon a frame that was merely
    // slow — the original bug in a new guise.
    constexpr uint64_t floor_ms = 5000;
    constexpr uint64_t ceiling_ms = 10000;
    const uint64_t sum_ms = (static_cast<uint64_t>(tdr_delay_s) + static_cast<uint64_t>(tdr_ddi_delay_s)) * 1000ull;
    if (sum_ms < floor_ms) {
      return static_cast<uint32_t>(floor_ms);
    }
    if (sum_ms > ceiling_ms) {
      return static_cast<uint32_t>(ceiling_ms);
    }
    return static_cast<uint32_t>(sum_ms);
  }

}  // namespace nvenc
