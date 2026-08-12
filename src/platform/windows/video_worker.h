#pragma once

#include <cstdint>
#include <string>

#include "src/thread_safe.h"
#include "src/video.h"

namespace platf::video_worker {
  bool is_child_process();
  void set_child_pipe(std::string pipe_name, std::uint32_t parent_pid);
  int run_child();

  /** Notify the isolated-process supervisor that capture is rebuilding. */
  void notify_capture_reinitializing();

  /** Capture-source epoch currently owned by this worker process. */
  std::uint64_t current_capture_generation();

  /** True for any decodable IDR, including the synthetic bootstrap frame. */
  bool packet_can_signal_capture_ready(video::packet_raw_t &packet);

  /**
   * Validate packet metadata at a capture-generation boundary.
   *
   * Packets older than the active generation are stale. A newer generation,
   * or the first packet still required for the active generation, is admitted
   * only by an IDR. The synthetic bootstrap IDR may open the very first
   * generation (`first_admission`) so client admission never waits on a slow
   * capture source; every later generation transition requires a real
   * captured IDR.
   */
  bool packet_metadata_can_enter_capture_generation(
    std::uint64_t packet_generation,
    bool capture_placeholder,
    bool is_idr,
    std::uint64_t active_generation,
    bool generation_needs_idr,
    bool first_admission
  );

  /** Launch an authenticated idle worker before RTSP supplies its config. */
  bool prewarm();

  /** Retire an unused prewarmed worker. */
  void cancel_prewarm();

  /** Run capture+encode in a crash-isolated child process. */
  bool capture(safe::mail_t mail, video::config_t config, void *channel_data);
}
