#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "src/platform/windows/video_worker_capture_status.h"

namespace {
  using namespace platf::video_worker;

  std::vector<std::uint8_t> bytes(const capture_status_t &status) {
    std::vector<std::uint8_t> result(sizeof(status));
    std::memcpy(result.data(), &status, sizeof(status));
    return result;
  }

  TEST(VideoWorkerCaptureStatus, ActualBackendTransitionsRoundTripAcrossTheWire) {
    // The requested source is not authoritative: the worker can fall back to
    // WGC, reopen DDA, or recover onto the ring during the same connection.
    std::uint64_t sent_sequence = 0;
    for (const auto *kind : {"vgd-ring", "wgc", "dda", "vgd-ring"}) {
      platf::vgd_transition::capture_note child;
      child.kind = kind;
      child.display = R"(\\.\DISPLAY12)";
      child.sequence = sent_sequence + 1;
      const auto wire = capture_status_update(child, sent_sequence);
      ASSERT_TRUE(wire);
      const auto parent = decode_capture_status(bytes(*wire));
      ASSERT_TRUE(parent);
      EXPECT_EQ(parent->kind, kind);
      EXPECT_EQ(parent->display, child.display);
      sent_sequence = child.sequence;
      EXPECT_FALSE(capture_status_update(child, sent_sequence));
    }
  }

  TEST(VideoWorkerCaptureStatus, NoStatusBeforeCaptureAndNoRepeatedAgeRefresh) {
    platf::vgd_transition::capture_note child;
    EXPECT_FALSE(capture_status_update(child, 0));
    child.kind = "vgd-ring";
    child.sequence = 1;
    EXPECT_TRUE(capture_status_update(child, 0));
    EXPECT_FALSE(capture_status_update(child, 1));
    // Reopening the same source is a new observation, not a duplicate.
    child.sequence = 2;
    EXPECT_TRUE(capture_status_update(child, 1));
    // A replacement child starts its own sequence from one.
    child.sequence = 1;
    EXPECT_TRUE(capture_status_update(child, 0));
  }

  TEST(VideoWorkerCaptureStatus, PreservesEmptyAndMaximumLengthDisplayNames) {
    platf::vgd_transition::capture_note child;
    child.kind = "wgc";
    child.sequence = 1;
    for (const auto &display : {std::string {}, std::string(63, 'd')}) {
      child.display = display;
      const auto wire = capture_status_update(child, 0);
      ASSERT_TRUE(wire);
      const auto parent = decode_capture_status(bytes(*wire));
      ASSERT_TRUE(parent);
      EXPECT_EQ(parent->display, display);
    }
    child.display = std::string(64, 'd');
    EXPECT_FALSE(capture_status_update(child, 0));
    child.display = std::string("display\0suffix", 14);
    EXPECT_FALSE(capture_status_update(child, 0));
  }

  TEST(VideoWorkerCaptureStatus, RejectsMalformedPayloadsWithoutPublishingABackend) {
    capture_status_t wire {};
    std::strcpy(wire.kind, "wgc");
    auto payload = bytes(wire);
    EXPECT_FALSE(decode_capture_status(std::span<const std::uint8_t>(payload).first(payload.size() - 1)));
    payload.push_back(0);
    EXPECT_FALSE(decode_capture_status(payload));
    std::memset(wire.kind, 'x', sizeof(wire.kind));
    EXPECT_FALSE(decode_capture_status(bytes(wire)));
    std::strcpy(wire.kind, "wgc");
    std::memset(wire.display, 'x', sizeof(wire.display));
    EXPECT_FALSE(decode_capture_status(bytes(wire)));
    wire = {};
    std::strcpy(wire.kind, "requested-vgd");
    EXPECT_FALSE(decode_capture_status(bytes(wire)));
    platf::vgd_transition::capture_note child;
    child.sequence = 1;
    child.kind = "requested-vgd";
    EXPECT_FALSE(capture_status_update(child, 0));
  }
}
