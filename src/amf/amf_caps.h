/**
 * @file src/amf/amf_caps.h
 * @brief Native AMD AMF runtime capability probe.
 *
 * Loads `amfrt64.dll` directly (the same library FFmpeg's `av1_amf` /
 * `hevc_amf` wrappers consume) and asks the encoder caps interface
 * about hardware features that affect streaming: the number of
 * concurrent hardware encoder instances (1 on single-VCN cards, 2 on
 * RDNA 3 / RDNA 4 dual-VCN cards) and whether AV1 / HEVC / H.264
 * encode are present.
 *
 * This module does NOT take any dependency on the AMF SDK headers —
 * the small set of interfaces it needs are declared as opaque vtables
 * inside the .cpp file so the rest of the codebase, and our build
 * system, stays unaffected.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace amf_caps {

  /// Per-codec encoder capability summary as reported by AMF.
  struct codec_caps_t {
    /// True when the runtime advertises an encoder for this codec on
    /// the active adapter.
    bool supported = false;
    /// Maximum number of independent hardware encoder instances that
    /// can run in parallel for this codec. 2 means dual-VCN; 1 (or 0)
    /// means single-engine. Drives whether the dual-VCN UI toggle is
    /// offered to the user.
    std::uint32_t max_hw_instances = 0;
    /// Maximum profile / bitrate / resolution reported by AMF, if the
    /// runtime exposes them. Free-form strings because the values are
    /// codec-specific (profile constants differ across AV1/HEVC/H.264).
    std::string profile;
    std::string max_resolution;
  };

  struct probe_result_t {
    /// True when `amfrt64.dll` was found, loaded, and `AMFInit`
    /// returned success.
    bool runtime_available = false;
    /// Human-readable AMF runtime version string (e.g. "1.4.36").
    /// Empty when the probe couldn't read the version property.
    std::string runtime_version;
    /// Raw AMF version integer (uint64_t encoded major/minor/patch).
    /// 0 when unknown.
    std::uint64_t runtime_version_raw = 0;
    /// Adapter description as reported by DXGI for the GPU AMF bound
    /// to. Empty when we couldn't enumerate.
    std::string adapter_description;
    /// Per-codec capability detail.
    codec_caps_t av1;
    codec_caps_t hevc;
    codec_caps_t h264;
    /// Worst-case latency of the probe call.
    std::chrono::milliseconds probe_duration {0};
    /// When the probe last ran. Set even when runtime_available is
    /// false so callers can detect a successful "AMF is not installed"
    /// answer.
    std::chrono::system_clock::time_point at;
    /// Free-form error context when runtime_available is false (e.g.
    /// "amfrt64.dll not found", "AMFInit failed (hr=0x...)").
    std::string error;
  };

  /// Whether ANY of the supported codecs reports multi-instance
  /// capability. Convenience helper for "should we offer the dual-VCN
  /// toggle at all?".
  inline bool supports_multi_instance(const probe_result_t &p) {
    return p.av1.max_hw_instances >= 2 ||
           p.hevc.max_hw_instances >= 2 ||
           p.h264.max_hw_instances >= 2;
  }

  /**
   * @brief Run the probe. Idempotent; subsequent calls reuse the
   *        cached result unless `force_refresh` is true. Safe from
   *        any thread.
   */
  probe_result_t probe(bool force_refresh = false);

  /// Returns the last probe result without re-running. Thread-safe.
  /// Returns an "unknown" (runtime_available=false) result if the
  /// probe hasn't run yet.
  probe_result_t last_result();

}  // namespace amf_caps
