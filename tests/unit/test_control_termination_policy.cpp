/**
 * @file tests/unit/test_control_termination_policy.cpp
 * @brief Unit tests for when a stopping session's control peer gets the graceful termination message.
 */

#include "src/control_termination_policy.h"

#include <gtest/gtest.h>
#include <set>

namespace {

  using stream::control_termination::graceful_termination_code;
  using stream::control_termination::should_send_graceful_termination;
  using stream::session::stop_reason_e;

  /// Host-initiated stops of a client that is still connected and expects to be told.
  const std::set<stop_reason_e> host_initiated {
    stop_reason_e::explicit_cancel,
    stop_reason_e::client_disconnect_request,
    stop_reason_e::rtsp_broadcast_shutdown,
  };

  TEST(ControlTerminationPolicy, GracefulCodeMatchesShutdownLoop) {
    EXPECT_EQ(graceful_termination_code, 0x80030023u);
  }

  TEST(ControlTerminationPolicy, HostInitiatedStopsWithLivePeerSend) {
    for (auto reason : host_initiated) {
      EXPECT_TRUE(should_send_graceful_termination(reason, true)) << stream::session::stop_reason_name(reason);
    }
  }

  TEST(ControlTerminationPolicy, ClientSideStopsSendNothing) {
    for (auto reason : {
           stop_reason_e::enet_disconnect,
           stop_reason_e::client_termination,
           stop_reason_e::ping_timeout,
           stop_reason_e::control_decrypt_failure,
           stop_reason_e::bad_encrypted_type,
         }) {
      EXPECT_FALSE(should_send_graceful_termination(reason, true)) << stream::session::stop_reason_name(reason);
    }
  }

  TEST(ControlTerminationPolicy, AmbiguousOrPeerlessStopsSendNothing) {
    for (auto reason : {
           stop_reason_e::video_thread_exit,
           stop_reason_e::audio_thread_exit,
           stop_reason_e::rtsp_clear_all,
           stop_reason_e::app_exited_before_peer,
           stop_reason_e::admission_retired,
           stop_reason_e::rtsp_reap,
         }) {
      EXPECT_FALSE(should_send_graceful_termination(reason, true)) << stream::session::stop_reason_name(reason);
    }
  }

  TEST(ControlTerminationPolicy, UnrecordedReasonSendsNothing) {
    EXPECT_FALSE(should_send_graceful_termination(stop_reason_e::count_, true));
  }

  TEST(ControlTerminationPolicy, NoConnectedPeerNeverSends) {
    for (int value = 0; value <= static_cast<int>(stop_reason_e::count_); ++value) {
      const auto reason = static_cast<stop_reason_e>(value);
      EXPECT_FALSE(should_send_graceful_termination(reason, false)) << stream::session::stop_reason_name(reason);
    }
  }

  TEST(ControlTerminationPolicy, OnlyHostInitiatedReasonsSend) {
    for (int value = 0; value <= static_cast<int>(stop_reason_e::count_); ++value) {
      const auto reason = static_cast<stop_reason_e>(value);
      EXPECT_EQ(should_send_graceful_termination(reason, true), host_initiated.contains(reason))
        << stream::session::stop_reason_name(reason);
    }
  }

}  // namespace
