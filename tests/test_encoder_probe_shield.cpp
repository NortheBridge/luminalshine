#include "src/encoder_probe_shield.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using video::probe_shield::outcome_e;
using video::probe_shield::result_t;
using video::probe_shield::run_cpp_exception_shield;

TEST(EncoderProbeShield, ProbeReturnsTrue_OutcomeOk_ProbeReturnedTrue) {
  bool called = false;
  const auto r = run_cpp_exception_shield([&] {
    called = true;
    return true;
  });
  EXPECT_TRUE(called);
  EXPECT_EQ(r.outcome, outcome_e::ok);
  EXPECT_TRUE(r.probe_returned);
  EXPECT_TRUE(r.std_what.empty());
}

TEST(EncoderProbeShield, ProbeReturnsFalse_OutcomeOk_ProbeReturnedFalse) {
  const auto r = run_cpp_exception_shield([] {
    return false;
  });
  EXPECT_EQ(r.outcome, outcome_e::ok);
  EXPECT_FALSE(r.probe_returned);
  EXPECT_TRUE(r.std_what.empty());
}

TEST(EncoderProbeShield, ProbeThrowsRuntimeError_OutcomeStdException_CapturesWhat) {
  const auto r = run_cpp_exception_shield([]() -> bool {
    throw std::runtime_error("boom: amf init failed");
  });
  EXPECT_EQ(r.outcome, outcome_e::std_exception);
  EXPECT_FALSE(r.probe_returned);
  EXPECT_EQ(r.std_what, "boom: amf init failed");
}

TEST(EncoderProbeShield, ProbeThrowsBadAlloc_OutcomeStdException_CapturesWhat) {
  const auto r = run_cpp_exception_shield([]() -> bool {
    throw std::bad_alloc {};
  });
  EXPECT_EQ(r.outcome, outcome_e::std_exception);
  EXPECT_FALSE(r.probe_returned);
  EXPECT_FALSE(r.std_what.empty());
}

TEST(EncoderProbeShield, ProbeThrowsNonStd_OutcomeUnknownException) {
  const auto r = run_cpp_exception_shield([]() -> bool {
    throw 42;  // an int — not derived from std::exception
  });
  EXPECT_EQ(r.outcome, outcome_e::unknown_exception);
  EXPECT_FALSE(r.probe_returned);
  EXPECT_TRUE(r.std_what.empty());
}

TEST(EncoderProbeShield, ProbeThrowsNonStdCustomType_OutcomeUnknownException) {
  struct opaque_driver_fault {};
  const auto r = run_cpp_exception_shield([]() -> bool {
    throw opaque_driver_fault {};
  });
  EXPECT_EQ(r.outcome, outcome_e::unknown_exception);
  EXPECT_FALSE(r.probe_returned);
}
