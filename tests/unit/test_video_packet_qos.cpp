#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "src/globals.h"
#include "src/thread_safe.h"
#include "src/video.h"
#include "src/video_packet_qos.h"

using namespace std::chrono_literals;

namespace {
  template<class Queue>
  bool wait_for_blocked_producer(Queue &queue, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (queue.waiting_producers() != 0) return true;
      std::this_thread::sleep_for(1ms);
    }
    return queue.waiting_producers() != 0;
  }
}

TEST(VideoPacketQueue, ReferencePreservingPolicyBlocksInsteadOfClearing) {
  safe::queue_t<int> queue {2, safe::queue_overflow_e::block};
  queue.raise(1);
  queue.raise(2);

  std::future<void> third;
  auto stop_queue = util::fail_guard([&] {
    queue.stop();
  });
  third = std::async(std::launch::async, [&] {
    queue.raise(3);
  });
  ASSERT_TRUE(wait_for_blocked_producer(queue));

  ASSERT_EQ(queue.pop(0ms), 1);
  ASSERT_EQ(third.wait_for(1s), std::future_status::ready);
  third.get();
  ASSERT_EQ(queue.pop(0ms), 2);
  ASSERT_EQ(queue.pop(0ms), 3);
}

TEST(VideoPacketQueue, StopUnblocksAProducerWaitingForSpace) {
  safe::queue_t<int> queue {1, safe::queue_overflow_e::block};
  queue.raise(1);

  std::future<void> blocked;
  auto stop_queue = util::fail_guard([&] {
    queue.stop();
  });
  blocked = std::async(std::launch::async, [&] {
    queue.raise(2);
  });
  ASSERT_TRUE(wait_for_blocked_producer(queue));

  queue.stop();
  ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
  blocked.get();
  EXPECT_FALSE(queue.pop(0ms).has_value());

  // Broadcast lifecycle reuses the same mailbox object on the next session.
  queue.stop();
  queue.reset();
  queue.raise(3);
  const auto reused = queue.pop(0ms);
  ASSERT_TRUE(reused.has_value());
  EXPECT_EQ(*reused, 3);
}

TEST(VideoPacketQueue, UnrelatedQueuesKeepLegacyClearOnOverflowBehavior) {
  safe::queue_t<int> queue {2};
  queue.raise(1);
  queue.raise(2);
  queue.raise(3);

  ASSERT_EQ(queue.pop(0ms), 3);
  EXPECT_FALSE(queue.pop(0ms).has_value());
}

TEST(VideoPacketQueue, StopResetCannotAdmitAProducerFromThePriorEpoch) {
  safe::queue_t<int> queue {1, safe::queue_overflow_e::block};
  queue.raise(1);
  std::future<void> stale_producer;
  auto stop_queue = util::fail_guard([&] {
    queue.stop();
  });

  stale_producer = std::async(std::launch::async, [&] {
    queue.raise(2);
  });
  ASSERT_TRUE(wait_for_blocked_producer(queue));

  queue.stop();
  queue.reset();
  ASSERT_EQ(stale_producer.wait_for(1s), std::future_status::ready);
  stale_producer.get();
  EXPECT_FALSE(queue.pop(0ms).has_value());

  queue.raise(3);
  const auto current = queue.pop(0ms);
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, 3);
}

TEST(VideoPacketQueue, HistoricalThirtySecondToThirtyThirdFrameNeverClearsReferences) {
  safe::queue_t<int> queue {32, safe::queue_overflow_e::block};
  for (int frame = 1; frame <= 32; ++frame) queue.raise(frame);

  std::future<void> thirty_third;
  auto stop_queue = util::fail_guard([&] {
    queue.stop();
  });
  thirty_third = std::async(std::launch::async, [&] {
    queue.raise(33);
  });
  ASSERT_TRUE(wait_for_blocked_producer(queue));

  ASSERT_EQ(queue.pop(0ms), 1);
  ASSERT_EQ(thirty_third.wait_for(1s), std::future_status::ready);
  thirty_third.get();
  for (int frame = 2; frame <= 33; ++frame) {
    ASSERT_EQ(queue.pop(0ms), frame);
  }
}

TEST(VideoPacketQueue, ProductionFactoryIsBoundedBlockingAndSessionOwned) {
  const auto packet = [](std::int64_t index) -> video::packet_t {
    return std::make_unique<video::packet_raw_generic>(
      std::vector<std::uint8_t> {0}, index, index == 1
    );
  };
  auto slow_mail = std::make_shared<safe::mail_raw_t>();
  auto fast_mail = std::make_shared<safe::mail_raw_t>();
  auto slow = video::packet_queue(slow_mail, mail::video_packets);
  auto fast = video::packet_queue(fast_mail, mail::video_packets);
  for (std::uint32_t i = 0; i < video::kPacketQueueDepth; ++i) {
    slow->raise(packet(i + 1));
  }

  std::future<void> blocked_slow;
  auto stop_queues = util::fail_guard([&] {
    slow->stop();
    fast->stop();
  });
  blocked_slow = std::async(std::launch::async, [&] {
    slow->raise(packet(video::kPacketQueueDepth + 1));
  });
  ASSERT_TRUE(wait_for_blocked_producer(*slow));

  fast->raise(packet(1));
  EXPECT_NE(fast->pop(0ms), nullptr);
  EXPECT_EQ(blocked_slow.wait_for(20ms), std::future_status::timeout);

  slow->stop();
  ASSERT_EQ(blocked_slow.wait_for(1s), std::future_status::ready);
  blocked_slow.get();
}

TEST(VideoPacketQos, GapWithholdsPredictiveFramesAndRequestsExactlyOneIdr) {
  stream::video_qos::state_t state;
  const auto start = std::chrono::steady_clock::now();

  auto idr = stream::video_qos::evaluate(state, 100, true, false, start);
  ASSERT_TRUE(idr.submit);
  stream::video_qos::submitted(state, 100, true, start);

  auto contiguous = stream::video_qos::evaluate(state, 101, false, false, start + 1ms);
  ASSERT_TRUE(contiguous.submit);
  stream::video_qos::submitted(state, 101, false, start + 1ms);

  const auto gap = stream::video_qos::evaluate(state, 133, false, false, start + 2ms);
  EXPECT_FALSE(gap.submit);
  EXPECT_TRUE(gap.request_idr);
  EXPECT_EQ(gap.reason, stream::video_qos::reason_e::frame_discontinuity);

  const auto held = stream::video_qos::evaluate(state, 134, false, false, start + 3ms);
  EXPECT_FALSE(held.submit);
  EXPECT_FALSE(held.request_idr);

  ASSERT_TRUE(stream::video_qos::evaluate(state, 135, true, false, start + 4ms).submit);
  const auto recovery = stream::video_qos::submitted(state, 135, true, start + 4ms);
  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(recovery->withheld_frames, 2u);

  EXPECT_TRUE(stream::video_qos::evaluate(state, 136, false, false, start + 5ms).submit);
}

TEST(VideoPacketQos, DeadlineDropWaitsForIdrBeforeResuming) {
  stream::video_qos::state_t state;
  const auto now = std::chrono::steady_clock::now();
  stream::video_qos::submitted(state, 10, true, now);

  const auto stale = stream::video_qos::evaluate(state, 11, false, true, now + 1ms);
  EXPECT_FALSE(stale.submit);
  EXPECT_TRUE(stale.request_idr);
  EXPECT_EQ(stale.reason, stream::video_qos::reason_e::stale_frame);

  EXPECT_FALSE(stream::video_qos::evaluate(state, 12, false, false, now + 2ms).submit);
  EXPECT_TRUE(stream::video_qos::evaluate(state, 13, true, false, now + 3ms).submit);
  stream::video_qos::submitted(state, 13, true, now + 3ms);
  EXPECT_TRUE(stream::video_qos::evaluate(state, 14, false, false, now + 4ms).submit);
}

TEST(VideoPacketQos, SessionsRecoverIndependently) {
  stream::video_qos::state_t a;
  stream::video_qos::state_t b;
  const auto now = std::chrono::steady_clock::now();
  stream::video_qos::submitted(a, 1, true, now);
  stream::video_qos::submitted(b, 20, true, now);

  EXPECT_FALSE(stream::video_qos::evaluate(a, 3, false, false, now + 1ms).submit);
  EXPECT_TRUE(stream::video_qos::evaluate(b, 21, false, false, now + 1ms).submit);
}

TEST(VideoPacketQos, FailedIdrSubmissionRearmsExactlyOneRecoveryRequest) {
  stream::video_qos::state_t state;
  const auto now = std::chrono::steady_clock::now();
  stream::video_qos::submitted(state, 10, true, now);

  ASSERT_TRUE(stream::video_qos::evaluate(state, 12, false, false, now + 1ms).request_idr);
  ASSERT_TRUE(stream::video_qos::evaluate(state, 13, true, false, now + 2ms).submit);
  stream::video_qos::submission_failed(state, true);

  const auto retry = stream::video_qos::evaluate(state, 14, false, false, now + 3ms);
  EXPECT_FALSE(retry.submit);
  EXPECT_TRUE(retry.request_idr);
}

TEST(VideoPacketQos, DuplicateAndBackwardFramesAreDiscontinuities) {
  const auto now = std::chrono::steady_clock::now();

  stream::video_qos::state_t duplicate;
  stream::video_qos::submitted(duplicate, 20, true, now);
  const auto duplicate_result = stream::video_qos::evaluate(duplicate, 20, false, false, now + 1ms);
  EXPECT_FALSE(duplicate_result.submit);
  EXPECT_EQ(duplicate_result.reason, stream::video_qos::reason_e::frame_discontinuity);

  stream::video_qos::state_t backward;
  stream::video_qos::submitted(backward, 20, true, now);
  const auto backward_result = stream::video_qos::evaluate(backward, 19, false, false, now + 1ms);
  EXPECT_FALSE(backward_result.submit);
  EXPECT_EQ(backward_result.reason, stream::video_qos::reason_e::frame_discontinuity);
}

TEST(VideoPacketQos, LostRecoveryRequestIsRetriedAfterBoundedDelay) {
  stream::video_qos::state_t state;
  const auto now = std::chrono::steady_clock::now();
  stream::video_qos::submitted(state, 10, true, now);

  EXPECT_TRUE(stream::video_qos::evaluate(state, 12, false, false, now + 1ms).request_idr);
  EXPECT_FALSE(stream::video_qos::evaluate(state, 13, false, false, now + 1s).request_idr);
  EXPECT_TRUE(stream::video_qos::evaluate(state, 14, false, false, now + 2100ms).request_idr);
  EXPECT_FALSE(stream::video_qos::evaluate(state, 15, false, false, now + 3s).request_idr);
}

TEST(VideoPacketQos, WirePacingIncludesFecAndAvoidsMillisecondMicrobursts) {
  const auto budget = stream::video_qos::pacing_budget(65'388, 20, 1'408, 1'376);

  EXPECT_GT(budget.average_wire_bitrate_bps, 80'000'000.0L);
  EXPECT_LT(budget.average_wire_bitrate_bps, 81'000'000.0L);
  EXPECT_NEAR(
    static_cast<double>(budget.drain_bitrate_bps),
    static_cast<double>(budget.average_wire_bitrate_bps * stream::video_qos::kWirePacingHeadroom),
    1.0
  );
  // One 1-ms quantum carries ~11 wire packets at this bitrate (1.50
  // headroom): a group small enough to shape 100-Mbps client paths, large
  // enough that one timer wake per quantum sustains the full drain rate.
  EXPECT_EQ(budget.packets_per_quantum, 11u);
  EXPECT_GT(budget.packet_interval, std::chrono::microseconds {85});
  EXPECT_LT(budget.packet_interval, std::chrono::microseconds {100});
}

TEST(VideoPacketQos, WirePacingHasAnExplicitSafetyCap) {
  const auto budget = stream::video_qos::pacing_budget(1'000'000, 50, 1'500, 1'000);
  EXPECT_EQ(budget.drain_bitrate_bps, stream::video_qos::kMaxWireDrainBitrateBps);
}

TEST(VideoPacketQos, BackwardRtpClockAdvancesByOneNominalFrame) {
  EXPECT_EQ(stream::video_qos::next_rtp_timestamp_ticks(80'000, 90'000, 120), 90'750u);
  EXPECT_EQ(stream::video_qos::next_rtp_timestamp_ticks(91'000, 90'000, 120), 91'000u);
}
