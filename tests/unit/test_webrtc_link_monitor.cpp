/**
 * @file tests/unit/test_webrtc_link_monitor.cpp
 * @brief Unit tests for the WebRTC link-loss teardown decision helper.
 */

#include "../tests_common.h"

#include <src/webrtc_link_monitor.h>

using webrtc_stream::link_action_e;
using webrtc_stream::link_monitor_t;
using webrtc_stream::link_state_e;

TEST(WebRtcLinkMonitorTest, StartsUnknownAndIdle) {
  link_monitor_t monitor;

  EXPECT_EQ(monitor.state(), link_state_e::unknown);
  EXPECT_FALSE(monitor.closing());
  EXPECT_FALSE(monitor.grace_expired(monitor.epoch())) << "nothing armed a timer";
}

TEST(WebRtcLinkMonitorTest, HealthyHandshakeNeverActs) {
  link_monitor_t monitor;

  EXPECT_EQ(monitor.report(link_state_e::connecting), link_action_e::none);
  EXPECT_EQ(monitor.report(link_state_e::connected), link_action_e::none);
  EXPECT_EQ(monitor.state(), link_state_e::connected);
  EXPECT_FALSE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, FailedClosesImmediately) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);

  EXPECT_EQ(monitor.report(link_state_e::failed), link_action_e::close_session);
  EXPECT_TRUE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, FailedWithoutEverConnectingStillCloses) {
  // NAT traversal that never succeeds: ICE goes checking -> failed.
  link_monitor_t monitor;
  monitor.report(link_state_e::connecting);

  EXPECT_EQ(monitor.report(link_state_e::failed), link_action_e::close_session);
}

TEST(WebRtcLinkMonitorTest, OnlyOneCloseIsEverRequested) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::failed), link_action_e::close_session);

  // The peer-connection observer echoes what the ICE observer already said, and
  // lwrtc_peer_close() will report `closed` on the way out: neither may close twice.
  EXPECT_EQ(monitor.report(link_state_e::failed), link_action_e::none);
  EXPECT_EQ(monitor.report(link_state_e::closed), link_action_e::none);
  EXPECT_EQ(monitor.report(link_state_e::disconnected), link_action_e::none);
  EXPECT_FALSE(monitor.grace_expired(monitor.epoch()));
}

TEST(WebRtcLinkMonitorTest, DisconnectStartsGraceTimerInsteadOfClosing) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);

  EXPECT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  EXPECT_FALSE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, GraceExpiryClosesWhenStillDisconnected) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_TRUE(monitor.grace_expired(epoch));
  EXPECT_TRUE(monitor.closing());
  EXPECT_FALSE(monitor.grace_expired(epoch)) << "a second timer firing must not close twice";
}

TEST(WebRtcLinkMonitorTest, RecoveryBeforeGraceExpiryCancelsTeardown) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::connected), link_action_e::none);
  EXPECT_FALSE(monitor.grace_expired(epoch)) << "the link came back; the timer is stale";
  EXPECT_FALSE(monitor.closing());
  EXPECT_EQ(monitor.state(), link_state_e::connected);
}

TEST(WebRtcLinkMonitorTest, DuplicateDisconnectReportsDoNotRestartTheGrace) {
  // Peer-connection state and ICE state both report the same loss; the grace
  // period must be measured from the first report, not restarted by the echo.
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::disconnected), link_action_e::none);
  EXPECT_EQ(monitor.epoch(), epoch);
  EXPECT_TRUE(monitor.grace_expired(epoch));
}

TEST(WebRtcLinkMonitorTest, FlapWithinGraceArmsAFreshTimer) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto first = monitor.epoch();
  ASSERT_EQ(monitor.report(link_state_e::connected), link_action_e::none);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto second = monitor.epoch();

  EXPECT_NE(first, second);
  EXPECT_FALSE(monitor.grace_expired(first)) << "the first timer was superseded";
  EXPECT_FALSE(monitor.closing());
  EXPECT_TRUE(monitor.grace_expired(second));
}

TEST(WebRtcLinkMonitorTest, FailedDuringGraceClosesOnceAndDisarmsTimer) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::failed), link_action_e::close_session);
  EXPECT_FALSE(monitor.grace_expired(epoch)) << "close_session was already requested";
}

TEST(WebRtcLinkMonitorTest, IceRestartDuringGraceKeepsTheTimerArmed) {
  // A browser-driven ICE restart while the link is down reports `checking`
  // (-> connecting). That is an attempt, not a recovery: if it stalls, the
  // original grace still closes the session instead of waiting for FAILED.
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::connecting), link_action_e::none);
  EXPECT_EQ(monitor.epoch(), epoch) << "the restart must not invalidate the pending timer";
  EXPECT_TRUE(monitor.grace_expired(epoch));
}

TEST(WebRtcLinkMonitorTest, IceRestartThatSucceedsCancelsTeardown) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();
  ASSERT_EQ(monitor.report(link_state_e::connecting), link_action_e::none);

  EXPECT_EQ(monitor.report(link_state_e::connected), link_action_e::none);
  EXPECT_FALSE(monitor.grace_expired(epoch));
  EXPECT_FALSE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, InitialConnectingNeverArmsOrCloses) {
  // First negotiation: connecting is where every session starts, and nothing
  // armed a timer, so a stray grace_expired() must not close it.
  link_monitor_t monitor;

  EXPECT_EQ(monitor.report(link_state_e::connecting), link_action_e::none);
  EXPECT_FALSE(monitor.grace_expired(monitor.epoch()));
  EXPECT_FALSE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, UnknownIsIgnored) {
  // The wrapper's enums map onto every named state; `unknown` only means an
  // unmapped value and must neither change state nor disturb a running grace.
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::unknown), link_action_e::none);
  EXPECT_EQ(monitor.state(), link_state_e::disconnected);
  EXPECT_EQ(monitor.epoch(), epoch);
  EXPECT_TRUE(monitor.grace_expired(epoch));
}

TEST(WebRtcLinkMonitorTest, ClosedIsNotATeardownTrigger) {
  // `closed` can only result from a local close; whoever initiated it is already
  // tearing the session down, so the monitor must stay out of the way.
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);

  EXPECT_EQ(monitor.report(link_state_e::closed), link_action_e::none);
  EXPECT_FALSE(monitor.closing());
}

TEST(WebRtcLinkMonitorTest, ClosedDuringGraceDisarmsTimer) {
  link_monitor_t monitor;
  monitor.report(link_state_e::connected);
  ASSERT_EQ(monitor.report(link_state_e::disconnected), link_action_e::start_grace_timer);
  const auto epoch = monitor.epoch();

  EXPECT_EQ(monitor.report(link_state_e::closed), link_action_e::none);
  EXPECT_FALSE(monitor.grace_expired(epoch));
}

TEST(WebRtcLinkMonitorTest, StateNamesAreStable) {
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::unknown), "unknown");
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::connecting), "connecting");
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::connected), "connected");
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::disconnected), "disconnected");
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::failed), "failed");
  EXPECT_STREQ(webrtc_stream::link_state_name(link_state_e::closed), "closed");
}
