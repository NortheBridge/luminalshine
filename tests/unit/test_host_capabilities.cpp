/**
 * @file tests/unit/test_host_capabilities.cpp
 * @brief Test the LuminalShine API level and capability list reported in HTTPS /serverinfo.
 */

// standard includes
#include <set>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/host_capabilities.h"

namespace {
  using names_t = std::vector<std::string_view>;

  host_capabilities::flags_t all_flags() {
    host_capabilities::flags_t flags;
    for (const auto &capability : host_capabilities::capabilities) {
      flags.*capability.enabled = true;
    }
    return flags;
  }

  names_t table_names() {
    names_t result;
    for (const auto &capability : host_capabilities::capabilities) {
      result.push_back(capability.name);
    }
    return result;
  }

  bool is_kebab_case(std::string_view name) {
    if (name.empty() || name.front() == '-' || name.back() == '-') {
      return false;
    }
    for (const char ch : name) {
      const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
      if (!allowed) {
        return false;
      }
    }
    return true;
  }

  names_t split_wire_list(std::string_view wire) {
    names_t result;
    if (wire.empty()) {
      return result;
    }
    std::size_t begin = 0;
    while (true) {
      const auto comma = wire.find(',', begin);
      result.push_back(wire.substr(begin, comma - begin));
      if (comma == std::string_view::npos) {
        return result;
      }
      begin = comma + 1;
    }
  }

  TEST(HostCapabilities, ApiLevelIsOne) {
    // Update this together with the bump rule on host_capabilities::api_level.
    EXPECT_EQ(host_capabilities::api_level, 1);
  }

  TEST(HostCapabilities, JoinOfNoNamesIsEmpty) {
    EXPECT_EQ(host_capabilities::join_capabilities({}), "");
  }

  TEST(HostCapabilities, JoinOfOneNameIsThatName) {
    const names_t capability_names {"a"};

    EXPECT_EQ(host_capabilities::join_capabilities(capability_names), "a");
  }

  TEST(HostCapabilities, JoinSeparatesNamesWithABareComma) {
    const names_t capability_names {"a", "b"};

    EXPECT_EQ(host_capabilities::join_capabilities(capability_names), "a,b");
  }

  TEST(HostCapabilities, NoFlagsReportNoCapabilities) {
    EXPECT_TRUE(host_capabilities::names({}).empty());
    EXPECT_EQ(host_capabilities::wire_list({}), "");
  }

  TEST(HostCapabilities, PyrowaveFlagReportsPyrowave) {
    const host_capabilities::flags_t flags {.pyrowave = true};

    EXPECT_EQ(host_capabilities::names(flags), (names_t {"pyrowave"}));
    EXPECT_EQ(host_capabilities::wire_list(flags), "pyrowave");
  }

  TEST(HostCapabilities, EachFlagReportsOnlyItsOwnName) {
    ASSERT_FALSE(host_capabilities::capabilities.empty());
    for (const auto &capability : host_capabilities::capabilities) {
      host_capabilities::flags_t flags;
      flags.*capability.enabled = true;

      EXPECT_EQ(host_capabilities::names(flags), (names_t {capability.name})) << capability.name;
    }
  }

  TEST(HostCapabilities, AllFlagsFollowTheTableOrder) {
    ASSERT_FALSE(host_capabilities::capabilities.empty());
    EXPECT_EQ(host_capabilities::names(all_flags()), table_names());
  }

  TEST(HostCapabilities, NamesAreUniqueKebabCase) {
    std::set<std::string_view> seen;
    for (const auto &capability : host_capabilities::capabilities) {
      EXPECT_TRUE(is_kebab_case(capability.name)) << capability.name;
      EXPECT_TRUE(seen.insert(capability.name).second) << "duplicate: " << capability.name;
    }
  }

  TEST(HostCapabilities, WireListIsTheNamesJoinedByCommas) {
    const auto flags = all_flags();
    const auto wire = host_capabilities::wire_list(flags);

    EXPECT_EQ(wire.find(' '), std::string::npos);
    EXPECT_EQ(split_wire_list(wire), host_capabilities::names(flags));
  }
}  // namespace
