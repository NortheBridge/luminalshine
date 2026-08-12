/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

// local includes
#include "entry_handler.h"
#include "thread_pool.h"

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief A boolean flag to indicate whether the cursor should be displayed.
 */
extern bool display_cursor;

#ifdef _WIN32
  // Declare global singleton used for NVIDIA control panel modifications
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief A global singleton used for NVIDIA control panel modifications.
 */
extern nvprefs::nvprefs_interface nvprefs_instance;
#endif

/**
 * @brief Handles process-wide communication.
 */
namespace mail {
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);
  MAIL(broadcast_shutdown);
  MAIL(video_packets);
  MAIL(audio_packets);
  MAIL(switch_display);

  // Local mail
  MAIL(touch_port);
  MAIL(idr);
  MAIL(invalidate_ref_frames);
  MAIL(gamepad_feedback);
  MAIL(hdr);
  MAIL(chroma_downgrade);
  // Raised by the RTSP video thread once the client's UDP peer has been
  // authenticated.  Windows may prewarm capture and encoding before that
  // point, but encoded packets must not be handed to the broadcaster while
  // the destination endpoint is still unset.
  MAIL(video_peer_ready);
  // Raised once the session's video pipeline can deliver its first packet:
  // by the isolated worker at FIRST_PACKET, or immediately when a session
  // commits to the in-process pipeline (which initializes after the peer
  // attaches and therefore has nothing to wait for). RTSP holds the ANNOUNCE
  // response on this for strict-first-frame clients (Xbox/webOS ports) whose
  // older moonlight-common-c enforces a hard 10-second no-video budget.
  MAIL(video_pipeline_ready);
  // Raised by the broadcaster when an encoded-frame sequence gap or excessive
  // packet age makes the current predictive chain unsafe to transmit. The
  // isolated worker uses this alongside `idr` to bypass only the normal
  // client-feedback cooldown and produce one immediate recovery IDR.
  MAIL(video_discontinuity);
  // Carries the frame index of an IDR after all of its UDP shards have been
  // submitted. Reading an IDR from worker IPC is not delivery and must not
  // acknowledge recovery early.
  MAIL(video_idr_submitted);
#undef MAIL

}  // namespace mail
