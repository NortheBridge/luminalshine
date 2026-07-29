/**
 * @file src/platform/windows/vgd_transition.h
 * @brief Forced WGC->VGD transition: heals a host stuck in WGC fallback
 *        once the LuminalVGD driver becomes available.
 *
 * Backend selection latches once per process, and the devnode ensure is a
 * one-shot startup hook — so a driver installed/staged AFTER the service
 * started left the host capturing via WGC until the next service restart
 * (task #61). This module arms a periodic watcher that, while the backend
 * is not LuminalVGD: re-attempts the devnode ensure (rate-limited), then
 * re-runs backend selection and the driver-status init once the control
 * device answers. Every completed transition logs the sentence
 * "LuminalShine has transitioned LuminalVGD into VGD Mode".
 *
 * Live streams already running in WGC fallback are moved by the
 * per-session fallback monitor (virtual_display.cpp,
 * schedule_virtual_display_fallback_monitor), which is armed at
 * creation-failure time and acts independently of this watcher.
 *
 * Also home to the active-capture-backend signal: platf::display()
 * records which capture backend each display open landed on, so the
 * fallback monitor can confirm the ring actually engaged and the
 * Troubleshooting page can show WGC-vs-VGD truth (nothing else in the
 * process records the winner).
 */
#pragma once

#include <cstdint>
#include <string>

namespace platf::vgd_transition {

  /// Arm the periodic watcher. Called from Windows platf::init() (after
  /// tdr_lifeboat::init()); requires the global task_pool to be running.
  void init();

  /// Bounded stop of the transition worker. Call from main's teardown
  /// path BEFORE platf::vgd_devnode::shutdown_join() — the worker may be
  /// inside a devnode ensure.
  void shutdown_join();

  enum class kick_status {
    started,        ///< a transition attempt is now running in the background
    already_vgd,    ///< the backend is already LuminalVGD — nothing to transition
    busy,           ///< a transition attempt or driver rebind is already in flight
    stack_down,     ///< the display stack is down; transition deferred
    shutting_down,  ///< the process is tearing down
  };

  const char *kick_status_name(kick_status status);

  /// Kick a transition attempt immediately (web UI "Transition now"
  /// button). The attempt itself runs on a worker thread; the returned
  /// status says only whether one was started.
  kick_status kick_now();

  // -- active-capture-backend signal ---------------------------------------

  /// Capture-backend kinds recorded by platf::display().
  inline constexpr const char *kCaptureKindVgdRing = "vgd-ring";
  inline constexpr const char *kCaptureKindWgc = "wgc";
  inline constexpr const char *kCaptureKindDda = "dda";

  struct capture_note {
    std::string kind;        ///< one of the kCaptureKind* values, or empty when never noted
    std::string display;     ///< display name the capture opened against
    std::int64_t age_ms;     ///< milliseconds since the note was recorded
    std::uint64_t sequence;  ///< monotonically increasing note counter (0 = never)
  };

  /// Record which capture backend a successful display open landed on.
  /// Called by platf::display(); cheap (mutex + two string copies).
  void note_capture_backend(const char *kind, const std::string &display);

  /// Latest capture note (kind empty / sequence 0 when no capture has
  /// been opened yet this process).
  capture_note last_capture_note();

}  // namespace platf::vgd_transition
