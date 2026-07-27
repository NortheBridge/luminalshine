/**
 * @file tests/unit/test_tdr_state.cpp
 * @brief Incident grouping and terminal-state behaviour of tdr::.
 *
 * Regression coverage for the 2026-07-27 incident (POSTMORTEM-2026-07-27.md):
 * one GPU failure that Windows never recovered from was re-detected ~22
 * times and reported to the user as "39 TDR events", with no terminal
 * state to stop the retry ladders. Detections of an unresolved failure
 * must fold into a single incident, and a confirmed dead display stack
 * must latch until the stack is observed working again.
 */

// standard includes
#include <chrono>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/tdr_state.h"

namespace {
  class TdrState: public ::testing::Test {
  protected:
    void SetUp() override {
      // Process-wide singleton with no reset API by design (a real
      // incident must survive everything except a restart). Tests take
      // the state as they find it and assert on deltas.
      baseline_incidents = tdr::incident_count();
      baseline_events = tdr::event_count();
      tdr::note_stack_healthy();
    }

    void TearDown() override {
      tdr::note_stack_healthy();
    }

    std::uint64_t baseline_incidents {0};
    std::uint64_t baseline_events {0};
  };

  TEST_F(TdrState, RepeatedDetectionsFoldIntoOneIncident) {
    tdr::mark_event(tdr::source_t::encoder_stall, 0, "encode wait timeout");
    const auto after_first = tdr::incident_count();
    EXPECT_EQ(after_first, baseline_incidents + 1);

    // The same unresolved failure, re-detected by other subsystems and by
    // later client attempts. These are not new incidents.
    tdr::mark_event(tdr::source_t::encoder_d3d11, 0x887A0004, "retry ladder exhausted");
    tdr::mark_event(tdr::source_t::dd_test_d3d11, 0x887A0004, "retry ladder exhausted");
    tdr::mark_event(tdr::source_t::query_display_config, 0, "QDC unavailable");

    EXPECT_EQ(tdr::incident_count(), after_first)
      << "re-detections must not open new incidents";
    EXPECT_EQ(tdr::event_count(), baseline_events + 4)
      << "raw detection count still advances for the ring/telemetry consumers";

    const auto incident = tdr::current_incident();
    ASSERT_TRUE(incident.has_value());
    EXPECT_EQ(incident->events, 4u);
    // The first detector is the one closest to the cause, and is kept
    // distinct from whatever detected it most recently.
    EXPECT_EQ(incident->first_source, tdr::source_t::encoder_stall);
    EXPECT_EQ(incident->last_source, tdr::source_t::query_display_config);
    EXPECT_FALSE(incident->terminal) << "nothing confirmed the stack was dead";
  }

  TEST_F(TdrState, StackDownLatchesAndMarksIncidentTerminal) {
    EXPECT_FALSE(tdr::stack_down());

    tdr::mark_stack_down(0x887A0004, "QueryDisplayConfig unavailable (status 50, 0 paths)");

    EXPECT_TRUE(tdr::stack_down())
      << "a confirmed dead stack must latch so retry ladders can stop";
    const auto incident = tdr::current_incident();
    ASSERT_TRUE(incident.has_value());
    EXPECT_TRUE(incident->terminal);
    EXPECT_EQ(incident->last_source, tdr::source_t::display_stack_down);
  }

  TEST_F(TdrState, StackHealthyClearsTheLatch) {
    tdr::mark_stack_down(0x887A0004, "QueryDisplayConfig unavailable");
    ASSERT_TRUE(tdr::stack_down());

    // The driver recovered on its own, or the user reset it: a successful
    // device creation proves the stack is back and must not require a
    // service restart to act on.
    tdr::note_stack_healthy();
    EXPECT_FALSE(tdr::stack_down());

    // And a failure after recovery reads as a new incident, not a
    // continuation of the one that was closed.
    const auto before = tdr::incident_count();
    tdr::mark_event(tdr::source_t::encoder_stall, 0, "fresh failure after recovery");
    EXPECT_EQ(tdr::incident_count(), before + 1);
  }

  TEST_F(TdrState, SourceLabelCoversEveryEnumerator) {
    // A missing label silently renders as "unknown" in the Web UI.
    for (int i = 1; i <= 6; ++i) {
      const auto src = static_cast<tdr::source_t>(i);
      EXPECT_STRNE(tdr::source_label(src), "unknown")
        << "source_t value " << i << " has no label";
    }
  }
}  // namespace
