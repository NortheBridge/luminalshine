/**
 * @file tests/unit/test_vgd_transition.cpp
 * @brief WGC->VGD transition primitives (task #61): the active-capture
 *        signal's note/query semantics and the kick-status naming
 *        contract.
 *
 * The full transition needs a live driver and streaming session; these
 * tests cover only pure process-local state (no device I/O, no PnP side
 * effects) so they behave identically on CI and on driver dev boxes.
 */
#ifdef _WIN32

  // standard includes
  #include <chrono>
  #include <string>
  #include <thread>

  // lib includes
  #include <gtest/gtest.h>

  // local includes
  #include "../tests_common.h"
  #include "src/platform/windows/vgd_transition.h"
  #include "src/platform/windows/video_worker.h"
  #include "src/platform/windows/virtual_display_vgd.h"

namespace {

  TEST(VgdWorkerRingTarget, ImportedTargetOverridesTheEmptyChildSessionMap) {
    const VDISPLAY::vgd::RingTargetInfo imported {0x123456789abcdef0ULL, 3, 0x4, 7};
    VDISPLAY::vgd::set_worker_ring_target(imported, "\\\\.\\DISPLAY77");
    ASSERT_TRUE(VDISPLAY::vgd::has_worker_ring_target());
    const auto resolved = VDISPLAY::vgd::ring_target_for_display("\\\\.\\DISPLAY77");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->session_id, imported.session_id);
    EXPECT_EQ(resolved->ring_slots, imported.ring_slots);
    EXPECT_EQ(resolved->transport_flags, imported.transport_flags);
    EXPECT_EQ(resolved->generation, imported.generation);
    EXPECT_FALSE(VDISPLAY::vgd::ring_target_for_display("\\\\.\\DISPLAY78").has_value());
    VDISPLAY::vgd::set_worker_ring_target(std::nullopt);
    EXPECT_FALSE(VDISPLAY::vgd::has_worker_ring_target());
  }

  TEST(VgdWorkerRingTarget, DeferredGenerationIdentitySurvivesExclusiveHandoff) {
    const VDISPLAY::vgd::RingTargetInfo imported {0xfedcba9876543210ULL, 4, 0x4, 0};
    VDISPLAY::vgd::set_worker_ring_target(imported, "\\\\.\\DISPLAY91");

    const auto resolved = VDISPLAY::vgd::ring_target_for_display("\\\\.\\DISPLAY91");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->session_id, imported.session_id);
    EXPECT_EQ(resolved->ring_slots, imported.ring_slots);
    EXPECT_EQ(resolved->transport_flags, imported.transport_flags);
    EXPECT_EQ(resolved->generation, 0u);
    EXPECT_FALSE(VDISPLAY::vgd::ring_target_for_display("\\\\.\\DISPLAY92").has_value());

    VDISPLAY::vgd::set_worker_ring_target(std::nullopt);
  }

  TEST(VideoWorkerReadiness, AnyDecodableIdrCanAdmitAClient) {
    // The synthetic bootstrap IDR is a decodable frame: admitting it keeps the
    // client's connection budget independent of a slow capture source, exactly
    // like the pre-worker pipeline's blank "alive" frame.
    video::packet_raw_generic placeholder {{0x01}, 1, true};
    placeholder.capture_placeholder = true;
    EXPECT_TRUE(platf::video_worker::packet_can_signal_capture_ready(placeholder));

    video::packet_raw_generic idr {{0x01, 0x02}, 3, true};
    idr.capture_placeholder = false;
    EXPECT_TRUE(platf::video_worker::packet_can_signal_capture_ready(idr));
  }

  TEST(VideoWorkerReadiness, DeltaFramesCannotAdmitAClient) {
    video::packet_raw_generic delta {{0x02}, 2, false};
    EXPECT_FALSE(platf::video_worker::packet_can_signal_capture_ready(delta));
  }

  TEST(VideoWorkerGeneration, RetiredEncoderPacketCannotEnterReplacementGeneration) {
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      4, false, true, 5, true, false
    ));
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      4, false, true, 5, true, true
    ));
  }

  TEST(VideoWorkerGeneration, PlaceholderIdrBootstrapsOnlyTheFirstAdmission) {
    // First admission ever: the synthetic bootstrap IDR may open the pipe.
    EXPECT_TRUE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      1, true, true, 1, true, true
    ));
    // A generation transition (capture reinit) still requires a real IDR.
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      5, true, true, 5, true, false
    ));
    // A zero generation is never admissible, bootstrap or not.
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      0, true, true, 1, true, true
    ));
  }

  TEST(VideoWorkerGeneration, DeltaCannotAdmitGeneration) {
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      5, false, false, 5, true, false
    ));
    EXPECT_FALSE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      5, false, false, 5, true, true
    ));
  }

  TEST(VideoWorkerGeneration, RealIdrAdmitsGenerationAndThenDeltasContinue) {
    EXPECT_TRUE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      6, false, true, 5, true, false
    ));
    EXPECT_TRUE(platf::video_worker::packet_metadata_can_enter_capture_generation(
      6, false, false, 6, false, false
    ));
  }

  TEST(VgdTransitionCaptureNote, RecordsKindDisplayAndBumpsSequence) {
    const auto before = platf::vgd_transition::last_capture_note();
    platf::vgd_transition::note_capture_backend(
      platf::vgd_transition::kCaptureKindWgc,
      "\\\\.\\DISPLAY7"
    );
    const auto after = platf::vgd_transition::last_capture_note();
    EXPECT_EQ(after.kind, platf::vgd_transition::kCaptureKindWgc);
    EXPECT_EQ(after.display, "\\\\.\\DISPLAY7");
    EXPECT_EQ(after.sequence, before.sequence + 1);
    EXPECT_GE(after.age_ms, 0);
  }

  TEST(VgdTransitionCaptureNote, SequenceIsMonotonicAcrossKinds) {
    platf::vgd_transition::note_capture_backend(
      platf::vgd_transition::kCaptureKindDda,
      "\\\\.\\DISPLAY1"
    );
    const auto first = platf::vgd_transition::last_capture_note();
    platf::vgd_transition::note_capture_backend(
      platf::vgd_transition::kCaptureKindVgdRing,
      "\\\\.\\DISPLAY274"
    );
    const auto second = platf::vgd_transition::last_capture_note();
    EXPECT_GT(second.sequence, first.sequence);
    EXPECT_EQ(second.kind, platf::vgd_transition::kCaptureKindVgdRing);
    // The note is last-writer-wins: the DDA record is gone.
    EXPECT_EQ(second.display, "\\\\.\\DISPLAY274");
  }

  TEST(VgdTransitionCaptureNote, AgeAdvancesBetweenQueries) {
    platf::vgd_transition::note_capture_backend(
      platf::vgd_transition::kCaptureKindWgc,
      "\\\\.\\DISPLAY2"
    );
    const auto early = platf::vgd_transition::last_capture_note();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto late = platf::vgd_transition::last_capture_note();
    EXPECT_EQ(late.sequence, early.sequence);
    EXPECT_GE(late.age_ms, early.age_ms + 20);
  }

  TEST(VgdTransitionKickStatus, NamesAreStableContract) {
    using platf::vgd_transition::kick_status;
    using platf::vgd_transition::kick_status_name;
    // The web UI keys its user-facing messages off these strings; renaming
    // one silently degrades the panel to the generic failure text.
    EXPECT_STREQ(kick_status_name(kick_status::started), "started");
    EXPECT_STREQ(kick_status_name(kick_status::already_vgd), "already_vgd");
    EXPECT_STREQ(kick_status_name(kick_status::busy), "busy");
    EXPECT_STREQ(kick_status_name(kick_status::stack_down), "stack_down");
    EXPECT_STREQ(kick_status_name(kick_status::shutting_down), "shutting_down");
  }

  // NOTE (review finding): no test here calls VDISPLAY::reselect_if_none()
  // or active_backend(). On a dev box with the driver package staged in
  // the DriverStore and the service stopped, backend selection performs a
  // REAL SwDeviceCreate of root\luminal_vgd (spawning an IddCx adapter
  // from a unit test) plus a device-started wait up to 20 s, and the
  // unlatched driver-start race makes consecutive-call assertions flaky.
  // Backend re-selection is covered by the field validation checklist
  // instead (install driver after service start, grep for the transition
  // sentence).

}  // namespace

#endif  // _WIN32
