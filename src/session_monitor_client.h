/**
 * @file src/session_monitor_client.h
 * @brief Thin producer client for the LuminalShineSessionMonitor service.
 *
 * The streaming host pushes per-session telemetry to the monitor via
 * a named pipe; the monitor maintains the ring buffer and serves it
 * back to the Web UI via localhost HTTP. This client is the producer
 * half — it serialises and dispatches ingest frames.
 *
 * Design properties:
 *   - Fire-and-forget from the caller's perspective. Every public
 *     function enqueues the frame and returns immediately; a single
 *     background thread drains the queue to the pipe. Callers on the
 *     streaming hot path (the encoder packet send loop, the RTSP
 *     event handlers) are never blocked on a slow / disconnected
 *     monitor.
 *   - Tolerant of monitor unavailability. When the pipe is closed
 *     or the monitor service isn't running, the background thread
 *     drops the frame after a short retry and continues. A
 *     subsequent call re-attempts connection. The streaming host
 *     never crashes because telemetry collection is down.
 *   - Process-wide singleton. `init()` is idempotent; calling it
 *     multiple times has no additional effect. `shutdown()` joins
 *     the background thread and is safe to call from atexit.
 *
 * The fact that the monitor uses string session IDs (so the Web
 * UI can render them without integer-overflow weirdness) and the
 * streaming host uses an atomic uint32 counter is reconciled here:
 * callers pass the existing `launch_session->id` and this client
 * stringifies it once before sending. The monitor never re-parses
 * — it stores the id as an opaque key.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

namespace session_mon {

  /**
   * @brief Snapshot of the static metadata that's known at
   *        session start. Optional fields can be filled in via
   *        `metadata_update` once the producer learns their values
   *        (e.g. the foreground application name only becomes known
   *        after Playnite or the proc launcher resolves it).
   */
  struct SessionMetadata {
    std::string  client_name;
    std::string  device;
    std::string  protocol;   ///< "RTSP", "RTP", "WebRTC"
    std::string  codec;      ///< "HEVC", "AV1", "H264"
    int          width                  = 0;
    int          height                 = 0;
    int          fps                    = 0;
    double       bitrate_mbps_target    = 0.0;
    bool         hdr                    = false;
    bool         yuv444                 = false;
    int          audio_channels         = 0;
    std::string  application;
    std::string  cpu_model;
    std::string  gpu_model;
    std::string  luminalshine_version;
  };

  /// One snapshot of per-second time-series values. Keys must match
  /// the names the Web UI's charts subscribe to. Known series:
  ///   - encode_latency_ms
  ///   - network_throughput_mbps
  ///   - client_losses
  ///   - idr_requests
  ///   - ref_invalidations
  ///   - actual_fps
  ///   - host_cpu_pct
  ///   - host_gpu_pct
  ///   - host_gpu_encoder_pct
  ///   - host_ram_pct
  ///   - host_vram_pct
  using Sample = std::map<std::string, double>;

  /**
   * @brief Initialise the background sender. Idempotent. Starts the
   *        host-perf-counter sampler thread that runs for the
   *        lifetime of the process and pushes host_cpu_pct /
   *        host_ram_pct samples to whichever sessions are currently
   *        active.
   */
  void init();

  /**
   * @brief Stop the background sender and join its thread. Safe to
   *        call from main shutdown or `atexit`. After this call no
   *        further ingest frames are sent; calls to session_started
   *        / sample / session_ended become no-ops.
   */
  void shutdown();

  /**
   * @brief Emit a session_started frame. The streaming host calls
   *        this from the rtsp / stream layer once the encoder is
   *        ready and all config is finalised.
   * @param id   Stringified launch_session->id; this is the same id
   *             the Web UI will key on for its panel.
   * @param md   Static metadata snapshot. Fields may be empty / 0
   *             when the producer doesn't know them yet — fill via
   *             metadata_update().
   */
  void session_started(const std::string &id, const SessionMetadata &md);

  /**
   * @brief Patch the session metadata. Shallow merge into the
   *        existing object on the monitor side. Use for late-arriving
   *        fields like the foreground application name.
   * @param patch JSON-serialisable key/value pairs to merge.
   */
  void metadata_update(const std::string &id, const std::map<std::string, std::string> &patch);

  /**
   * @brief Push one timestamped time-series sample. ts_epoch_s is
   *        the unix-seconds bucket the values belong to; the monitor
   *        de-duplicates within a bucket (latest-wins) so callers
   *        can sample more frequently than 1Hz without inflating
   *        storage.
   */
  void sample(const std::string &id, std::int64_t ts_epoch_s, const Sample &s);

  /**
   * @brief Convenience overload that times itself: ts is taken from
   *        std::chrono::system_clock::now() coerced to integer
   *        seconds.
   */
  void sample_now(const std::string &id, const Sample &s);

  /**
   * @brief Mark a session as ended. The monitor finalises its
   *        stream_ended_at marker, flushes the on-disk JSON, and the
   *        Web UI's "Active" badge clears.
   */
  void session_ended(const std::string &id);

  /**
   * @brief Helper to stringify a uint32 session id consistently
   *        with how the rest of the producer / monitor side expects
   *        it. Centralised here so we never accidentally end up
   *        with two formats for the same id.
   */
  std::string make_id(std::uint32_t launch_session_id);

}  // namespace session_mon
