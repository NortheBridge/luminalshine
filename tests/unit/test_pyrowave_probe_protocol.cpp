#include "src/pyrowave/probe_protocol.h"

#include <gtest/gtest.h>

#include <string>

TEST(PyroWaveProbeProtocol, SelectionRoundTripsAsCanonicalHex) {
  const std::string value = "GPU ";
  const std::string utf8_suffix = "\xE2\x98\x83";
  const auto encoded = pyrowave::probe_protocol::encode_selection(value + utf8_suffix);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->substr(0, 8), "00000007");
  for (const char character : *encoded) {
    EXPECT_TRUE((character >= '0' && character <= '9') || (character >= 'A' && character <= 'F'));
  }
  EXPECT_EQ(pyrowave::probe_protocol::decode_selection(*encoded), value + utf8_suffix);

  const auto empty = pyrowave::probe_protocol::encode_selection({});
  ASSERT_EQ(empty, std::optional<std::string> {"00000000"});
  EXPECT_EQ(pyrowave::probe_protocol::decode_selection(*empty), std::optional<std::string> {""});
}

TEST(PyroWaveProbeProtocol, SelectionRejectsMalformedOrAmbiguousTokens) {
  using pyrowave::probe_protocol::decode_selection;
  EXPECT_FALSE(decode_selection(""));
  EXPECT_FALSE(decode_selection("0000000"));
  EXPECT_FALSE(decode_selection("00000001"));
  EXPECT_FALSE(decode_selection("00000001GG"));
  EXPECT_FALSE(decode_selection("0000000100"));  // embedded NUL
  EXPECT_FALSE(decode_selection("00000002AA"));  // declared length mismatch

  std::string too_large(pyrowave::probe_protocol::kMaxSelectionBytes + 1, 'x');
  EXPECT_FALSE(pyrowave::probe_protocol::encode_selection(too_large));
  EXPECT_FALSE(pyrowave::probe_protocol::encode_selection(std::string_view("a\0b", 3)));
}

TEST(PyroWaveProbeProtocol, OptionalLuidUsesOneCanonicalFixedWidthToken) {
  using namespace pyrowave::probe_protocol;
  const optional_luid_t present {
    .present = true,
    .high = 0xFEDCBA98u,
    .low = 0x01234567u,
  };
  const auto encoded = encode_optional_luid(present);
  EXPECT_EQ(encoded, "1FEDCBA9801234567");
  EXPECT_EQ(decode_optional_luid(encoded), std::optional<optional_luid_t> {present});

  const optional_luid_t absent {};
  EXPECT_EQ(encode_optional_luid(absent), "00000000000000000");
  EXPECT_EQ(decode_optional_luid("00000000000000000"), std::optional<optional_luid_t> {absent});
  EXPECT_FALSE(decode_optional_luid("00000000000000001"));
  EXPECT_FALSE(decode_optional_luid("2FEDCBA9801234567"));
  EXPECT_FALSE(decode_optional_luid("1FEDCBA980123456"));
  EXPECT_FALSE(decode_optional_luid("1FEDCBA980123456Z"));
}
