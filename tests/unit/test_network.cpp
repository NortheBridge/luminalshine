/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include "../tests_common.h"

#include <src/network.h>

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Sunshine"),
    std::make_tuple("", "Sunshine"),
    std::make_tuple("😁", "Sunshine"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

namespace {
  std::array<unsigned char, 16> v6_bytes(const std::string &addr) {
    return boost::asio::ip::make_address_v6(addr).to_bytes();
  }
}  // namespace

struct V6PrefixMatchTest: testing::TestWithParam<std::tuple<std::string, std::string, unsigned, bool>> {};

TEST_P(V6PrefixMatchTest, Run) {
  auto [a, b, prefix_len, expected] = GetParam();
  ASSERT_EQ(net::v6_prefix_match(v6_bytes(a), v6_bytes(b), prefix_len), expected);
}

INSTANTIATE_TEST_SUITE_P(
  V6PrefixMatchTests,
  V6PrefixMatchTest,
  testing::Values(
    // prefix_len 0 matches everything
    std::make_tuple("2001:db8::1", "fe80::1", 0u, true),
    std::make_tuple("::", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 0u, true),
    // prefix_len 1 compares only the most significant bit
    std::make_tuple("::", "8000::", 1u, false),
    std::make_tuple("8000::", "ffff::", 1u, true),
    // byte-boundary straddling: 2001:db8:0:2:: and 2001:db8:0:3:: differ only at bit 63
    std::make_tuple("2001:db8:0:2::", "2001:db8:0:3::", 63u, true),
    std::make_tuple("2001:db8:0:2::", "2001:db8:0:3::", 64u, false),
    // typical /64: same prefix, different interface identifiers
    std::make_tuple("2001:db8:0:1::10", "2001:db8:0:1:aaaa:bbbb:cccc:dddd", 64u, true),
    std::make_tuple("2001:db8:0:1::10", "2001:db8:0:2::10", 64u, false),
    // bit 64 (first bit past the /64 boundary)
    std::make_tuple("2001:db8::", "2001:db8:0:0:8000::", 64u, true),
    std::make_tuple("2001:db8::", "2001:db8:0:0:8000::", 65u, false),
    std::make_tuple("2001:db8:0:0:8000::", "2001:db8:0:0:8000:0:0:1", 65u, true),
    // /127 point-to-point pairs differ only in the last bit
    std::make_tuple("2001:db8::", "2001:db8::1", 127u, true),
    std::make_tuple("2001:db8::", "2001:db8::1", 128u, false),
    // prefix_len 128 requires an exact match
    std::make_tuple("2001:db8::1", "2001:db8::1", 128u, true),
    // mismatch in the very first byte fails for any non-zero prefix
    std::make_tuple("2001:db8::1", "3001:db8::1", 8u, false),
    std::make_tuple("2001:db8::1", "3001:db8::1", 128u, false)
  )
);

/**
 * @brief Test fixture for bind_address tests with setup/teardown
 */
class BindAddressTest: public ::testing::Test {
protected:
  std::string original_bind_address;

  void SetUp() override {
    // Save the original bind_address config
    original_bind_address = config::sunshine.bind_address;
  }

  void TearDown() override {
    // Restore the original bind_address config
    config::sunshine.bind_address = original_bind_address;
  }
};

/**
 * @brief Test that get_bind_address returns wildcard when bind_address is not configured
 */
TEST_F(BindAddressTest, DefaultBehaviorIPv4) {
  // Clear bind_address to test the default behavior
  config::sunshine.bind_address = "";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "0.0.0.0");
}

/**
 * @brief Test that get_bind_address returns wildcard when bind_address is not configured (IPv6)
 */
TEST_F(BindAddressTest, DefaultBehaviorIPv6) {
  // Clear bind_address to test the default behavior
  config::sunshine.bind_address = "";

  const auto bind_addr = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::");
}

/**
 * @brief Test that get_bind_address returns configured IPv4 address
 */
TEST_F(BindAddressTest, ConfiguredIPv4Address) {
  // Set a specific IPv4 address
  config::sunshine.bind_address = "192.168.1.100";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "192.168.1.100");
}

/**
 * @brief Test that get_bind_address returns configured IPv6 address
 */
TEST_F(BindAddressTest, ConfiguredIPv6Address) {
  // Set a specific IPv6 address
  config::sunshine.bind_address = "::1";

  const auto bind_addr = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::1");
}

/**
 * @brief Test that get_bind_address returns configured address regardless of address family
 */
TEST_F(BindAddressTest, ConfiguredAddressOverridesFamily) {
  // Set a specific IPv6 address but request IPv4 family
  // The configured address should still be returned
  config::sunshine.bind_address = "2001:db8::1";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "2001:db8::1");
}

/**
 * @brief Test with loopback addresses
 */
TEST_F(BindAddressTest, LoopbackAddresses) {
  // Test IPv4 loopback
  config::sunshine.bind_address = "127.0.0.1";
  const auto bind_addr_v4 = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "127.0.0.1");

  // Test IPv6 loopback
  config::sunshine.bind_address = "::1";
  const auto bind_addr_v6 = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "::1");
}

/**
 * @brief Test with link-local addresses
 */
TEST_F(BindAddressTest, LinkLocalAddresses) {
  // Test IPv4 link-local
  config::sunshine.bind_address = "169.254.1.1";
  const auto bind_addr_v4 = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "169.254.1.1");

  // Test IPv6 link-local
  config::sunshine.bind_address = "fe80::1";
  const auto bind_addr_v6 = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "fe80::1");
}

/**
 * @brief Test that af_to_any_address_string still works correctly
 */
TEST_F(BindAddressTest, WildcardAddressFunction) {
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::IPV4), "0.0.0.0");
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::BOTH), "::");
}
