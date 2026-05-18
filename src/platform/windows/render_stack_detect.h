/**
 * @file src/platform/windows/render_stack_detect.h
 * @brief Detect whether a foreground game process has loaded NVIDIA AI
 *        render-stack modules (DLSS, DLSS Frame Generation, DLAA).
 *
 * Used by the streaming session to emit an informational tip when the
 * combined workload (game AI passes + LuminalShine NVENC encode at 4K
 * HDR) is known to push the GPU to its TDR threshold on RTX 40/50.
 *
 * Deliberately framed as a help signal, not a warning: the contention
 * pattern is an interaction between the NVIDIA driver, the game, and
 * the encode workload — not a LuminalShine defect — so the tip's tone
 * is "here's how to make it smoother", not "this will crash".
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace platf::render_stack {

  /// Modules whose presence in any foreground process indicates an
  /// NVIDIA AI render pass that runs on the Tensor cores. NVIDIA ships
  /// them inside the per-game NGX bin; their names are stable.
  struct module_match_t {
    /// "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlaa.dll", etc.
    std::string module_name;
    /// PID of the process that had the module loaded.
    std::uint32_t pid;
    /// Image name (e.g. "Cyberpunk2077.exe"). Best-effort; empty when
    /// we don't have rights to read it.
    std::string process_image;
  };

  struct detection_t {
    /// True when at least one foreground process had any AI render
    /// module loaded.
    bool any_match;
    /// Specific flags for the three modules we care about (DLSS,
    /// DLSS Frame Generation, DLAA). FG is the most TDR-prone of the
    /// three; we surface it separately so callers can branch on it.
    bool has_dlss;
    bool has_dlss_fg;
    bool has_dlaa;
    /// Every matching module + which process it lives in. Order is
    /// not defined.
    std::vector<module_match_t> matches;
    /// When the snapshot was taken.
    std::chrono::system_clock::time_point at;
  };

  /**
   * @brief Walk all accessible processes on the system and return the
   *        set of NVIDIA AI render-stack modules they have loaded.
   *
   * O(processes × modules-per-process). Practical cost ~10 ms on a
   * mid-end Windows system. Safe to call from any thread. Returns an
   * empty detection (any_match=false) on any internal error rather
   * than throwing.
   */
  detection_t snapshot();

  /**
   * @brief Whether the active streaming configuration is in the
   *        contention-prone band that combines with AI render passes
   *        to provoke TDRs on RTX 40/50.
   *
   * Criteria (ALL must be true to return true):
   *   - resolution_width × resolution_height >= 3840 × 2160
   *   - hdr_enabled (Rec. 2020 + SMPTE 2084 PQ)
   *   - bit_depth == 10
   *   - codec_label starts with "av1_nvenc" or "hevc_nvenc"
   */
  bool config_is_at_risk(
    int resolution_width,
    int resolution_height,
    bool hdr_enabled,
    int bit_depth,
    const std::string &codec_label
  );

  /**
   * @brief Render the user-facing "streaming tip" string for a given
   *        detection + config. Returns an empty string when no tip is
   *        warranted (configuration isn't at risk OR no AI modules
   *        were detected).
   *
   * Tone: informational, not a warning. The user should walk away with
   * "here's how to make this smoother if you want", not "your stream
   * will crash".
   */
  std::string make_streaming_tip(
    const detection_t &detection,
    int resolution_width,
    int resolution_height,
    const std::string &codec_label
  );

  /**
   * @brief Last detection + tip emitted by `evaluate_and_tip`. Returned
   *        by `last_event()` for the Web UI's "Streaming environment"
   *        card.
   */
  struct event_record_t {
    detection_t detection;
    int resolution_width = 0;
    int resolution_height = 0;
    bool hdr_enabled = false;
    int bit_depth = 0;
    std::string codec_label;
    std::string tip;  ///< empty if no tip was emitted
  };

  std::optional<event_record_t> last_event();

  /**
   * @brief Take a snapshot, evaluate it against the running config,
   *        and emit the soft tip exactly once per (codec, resolution,
   *        hdr) tuple within a process lifetime. Returns the rendered
   *        tip (empty when nothing was emitted).
   */
  std::string evaluate_and_tip(
    int resolution_width,
    int resolution_height,
    bool hdr_enabled,
    int bit_depth,
    const std::string &codec_label
  );

}  // namespace platf::render_stack
