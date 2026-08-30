/**
 * @file src/encoder_probe_suppression.h
 * @brief Per-encoder, per-codec suppression of codecs that faulted during probing.
 *
 * The encoder probe validates H.264, then HEVC, then AV1 against one
 * encoder_t. A graphics-driver fault anywhere in that sequence is caught by
 * the SEH / C++ shield in video.cpp, which then discards the whole encoder —
 * so a driver bug specific to one codec costs the user every codec on that
 * encoder, and they fall back to software encoding. Because the probe re-runs
 * on every launch and resume and negative results are deliberately not cached,
 * that repeats forever.
 *
 * Nothing about the faulted probe's state can be trusted to fix this in place.
 * Capability bits are fail-OPEN — validate_encoder starts with
 * `capabilities.set()` (every bit 1) and only corrects downward — and a codec
 * whose probe was cut short therefore reads as fully supported, HDR and 4:4:4
 * included. H.264's own bits are not finalised until after the AV1 probe has
 * run, so even "H.264 already passed" is not a safe thing to keep. On Windows
 * the shield is a bare __except and the build has no /EHa, so no destructors
 * ran either: the encode session and display from the faulted attempt are
 * leaked, not released.
 *
 * So this registry does not rescue the faulted pass. That pass still discards
 * everything and still falls back exactly as before. It records which codec
 * was in flight, and the *next* probe pass skips that one codec for that one
 * encoder — reusing the same lever as the H264_ONLY flag — and therefore
 * runs to completion with every capability bit properly written.
 *
 * Suppression is process-lifetime: it is cleared by a host restart, or by any
 * restart of the service. Note that a GPU driver update alone does NOT clear
 * it — installing a driver does not restart this service — so a driver that
 * fixes the fault will not re-enable the codec until the service next starts.
 * That is an accepted limitation rather than an oversight: only HEVC and AV1
 * can be suppressed (see is_suppressible), so the worst case is one optional
 * codec staying off slightly longer than strictly necessary.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace video::probe_suppression {

  /**
   * @brief Which codec's probe was in flight when a fault happened.
   */
  enum class codec_e {
    h264 = 0,
    hevc = 1,
    av1 = 2,
    /**
     * The fault happened in work that is not attributable to a single codec —
     * the shared SDR->HDR display reset between codec probes, for example.
     * Never suppresses anything: guessing here would disable a codec that is
     * perfectly healthy, so the encoder is simply probed in full next pass.
     */
    unattributed = 3,
  };

  /**
   * @brief Whether a fault on @p codec may be turned into a suppression.
   *
   * Only HEVC and AV1. H.264 is deliberately excluded, and that exclusion is
   * load-bearing rather than conservatism:
   *
   * H.264 is mandatory — validate_encoder bails out entirely if it fails, and
   * every encoder in the candidate list must pass it — so there is no partial
   * result to preserve by skipping it, and suppression buys nothing. What it
   * would cost is severe. The probe's shield catches faults from shared setup
   * too (acquiring the display, creating the D3D device, DXGI duplication),
   * and on Windows it is a bare __except, which also catches ordinary C++
   * exceptions such as a std::bad_alloc out of a wedged display stack. A
   * single transient fault of that kind sweeping the candidate list would
   * suppress H.264 on every encoder including the last-resort `software`
   * entry, leaving probe_encoders() with nothing to return — permanently,
   * because unlike the per-pass encoder_list copy this registry outlives the
   * pass. Every launch and resume would then answer HTTP 503 until the service
   * was restarted.
   *
   * An H.264 fault therefore keeps the pre-existing, self-healing behaviour:
   * discard the encoder for this pass and re-probe it in full on the next one.
   */
  constexpr bool is_suppressible(codec_e codec) {
    return codec == codec_e::hevc || codec == codec_e::av1;
  }

  /// Human-readable codec name for log lines.
  inline const char *codec_name(codec_e codec) {
    switch (codec) {
      case codec_e::h264:
        return "H.264";
      case codec_e::hevc:
        return "HEVC";
      case codec_e::av1:
        return "AV1";
      default:
        return "an unattributed stage";
    }
  }

  /**
   * @brief Set of (encoder, codec) pairs whose probe faulted the driver.
   *
   * Thread-safe: probe_encoders() can run concurrently with a stream start.
   */
  class registry_t {
  public:
    /// Record that @p codec faulted while being probed on @p encoder_name.
    /// Codecs that are not suppressible (H.264 and `unattributed`) are ignored.
    void suppress(std::string_view encoder_name, codec_e codec) {
      if (!is_suppressible(codec)) {
        return;
      }
      const std::lock_guard lock {mutex_};
      entries_[std::string {encoder_name}][index_of(codec)] = true;
    }

    /// Whether @p codec has previously faulted on @p encoder_name.
    bool is_suppressed(std::string_view encoder_name, codec_e codec) const {
      if (!is_suppressible(codec)) {
        return false;
      }
      const std::lock_guard lock {mutex_};
      const auto it = entries_.find(encoder_name);
      return it != entries_.end() && it->second[index_of(codec)];
    }

    /// Drop every record. Exists for tests; nothing in the host calls it.
    void clear() {
      const std::lock_guard lock {mutex_};
      entries_.clear();
    }

  private:
    static std::size_t index_of(codec_e codec) {
      return static_cast<std::size_t>(codec);
    }

    mutable std::mutex mutex_;
    /// std::less<> so find() accepts a string_view without allocating.
    std::map<std::string, std::array<bool, 3>, std::less<>> entries_;
  };

  /// The host-wide registry consulted by the encoder probe.
  inline registry_t &process_registry() {
    static registry_t registry;
    return registry;
  }

}  // namespace video::probe_suppression
