/**
 * @file tests/unit/test_session_stop_reason.cpp
 * @brief Unit tests for the streaming-session stop reason vocabulary.
 */

#include "src/session_stop_reason.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

namespace {

  using stream::session::stop_reason_e;
  using stream::session::stop_reason_name;

  TEST(SessionStopReason, EveryReasonHasADistinctNonEmptyName) {
    std::set<std::string> seen;
    for (int value = 0; value < static_cast<int>(stop_reason_e::count_); ++value) {
      const char *name = stop_reason_name(static_cast<stop_reason_e>(value));
      ASSERT_NE(name, nullptr) << "reason " << value;
      EXPECT_GT(std::strlen(name), 0u) << "reason " << value;
      EXPECT_STRNE(name, "unknown") << "reason " << value << " has no name case";
      EXPECT_TRUE(seen.insert(name).second) << "duplicate name '" << name << "' for reason " << value;
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(stop_reason_e::count_));
  }

  TEST(SessionStopReason, SentinelAndOutOfRangeValuesAreUnknown) {
    EXPECT_STREQ(stop_reason_name(stop_reason_e::count_), "unknown");
    EXPECT_STREQ(stop_reason_name(static_cast<stop_reason_e>(-1)), "unknown");
  }

  TEST(SessionStopReason, ShortClientIdKeepsOnlyTheUuidPrefix) {
    EXPECT_EQ(stream::session::short_client_id("0123456789abcdef"), "01234567");
    EXPECT_EQ(stream::session::short_client_id("abc"), "abc");
    EXPECT_TRUE(stream::session::short_client_id("").empty());
  }

  TEST(SessionStopReason, DescribeNamesTheSessionWithoutTheFullUuid) {
    const stream::session::identity_t who {42, "Living Room", "0123456789abcdef-0123"};
    EXPECT_EQ(stream::session::describe(who), "id=42 client='Living Room' uuid=01234567");
  }

  TEST(SessionStopReason, FileBasenameHandlesBothSeparators) {
    EXPECT_EQ(stream::session::file_basename("D:\\repo\\src\\stream.cpp"), "stream.cpp");
    EXPECT_EQ(stream::session::file_basename("/repo/src/rtsp.cpp"), "rtsp.cpp");
    EXPECT_EQ(stream::session::file_basename("nvhttp.cpp"), "nvhttp.cpp");
    EXPECT_TRUE(stream::session::file_basename("").empty());
  }

}  // namespace
