#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pyrowave::probe_protocol {
  // Keep the private command line well below CreateProcessW's 32,767 UTF-16
  // character limit even when both selections are at their maximum size.
  inline constexpr std::size_t kMaxSelectionBytes = 4'096;
  inline constexpr std::size_t kLengthDigits = 8;
  inline constexpr std::size_t kEncodedLuidSize = 17;

  struct optional_luid_t {
    bool present = false;
    std::uint32_t high = 0;
    std::uint32_t low = 0;

    friend constexpr bool operator==(const optional_luid_t &, const optional_luid_t &) = default;
  };

  constexpr int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  }

  constexpr char hex_digit(unsigned value) noexcept {
    return "0123456789ABCDEF"[value & 0xFu];
  }

  /**
   * Encode arbitrary UTF-8 bytes as one command-line-safe token.
   *
   * The first eight hexadecimal characters are the byte count. Every input
   * byte then occupies exactly two hexadecimal characters. Embedded NUL is
   * rejected because Windows APIs and config strings do not share a safe,
   * unambiguous interpretation for it.
   */
  inline std::optional<std::string> encode_selection(std::string_view value) {
    if (value.size() > kMaxSelectionBytes || value.find('\0') != std::string_view::npos) {
      return std::nullopt;
    }
    std::string encoded(kLengthDigits + value.size() * 2, '0');
    const auto length = static_cast<std::uint32_t>(value.size());
    for (std::size_t index = 0; index < kLengthDigits; ++index) {
      const auto shift = static_cast<unsigned>((kLengthDigits - 1 - index) * 4);
      encoded[index] = hex_digit(length >> shift);
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      const auto byte = static_cast<unsigned char>(value[index]);
      encoded[kLengthDigits + index * 2] = hex_digit(byte >> 4);
      encoded[kLengthDigits + index * 2 + 1] = hex_digit(byte);
    }
    return encoded;
  }

  inline std::optional<std::string> decode_selection(std::string_view encoded) {
    if (encoded.size() < kLengthDigits) return std::nullopt;
    std::uint32_t length = 0;
    for (std::size_t index = 0; index < kLengthDigits; ++index) {
      const int nibble = hex_value(encoded[index]);
      if (nibble < 0) return std::nullopt;
      length = (length << 4) | static_cast<std::uint32_t>(nibble);
    }
    if (length > kMaxSelectionBytes || encoded.size() != kLengthDigits + static_cast<std::size_t>(length) * 2) {
      return std::nullopt;
    }
    std::string decoded(length, '\0');
    for (std::size_t index = 0; index < length; ++index) {
      const int high = hex_value(encoded[kLengthDigits + index * 2]);
      const int low = hex_value(encoded[kLengthDigits + index * 2 + 1]);
      if (high < 0 || low < 0) return std::nullopt;
      const auto byte = static_cast<unsigned char>((high << 4) | low);
      if (byte == 0) return std::nullopt;
      decoded[index] = static_cast<char>(byte);
    }
    return decoded;
  }

  /** Fixed-width optional LUID token: presence nibble + high/low DWORDs. */
  inline std::string encode_optional_luid(optional_luid_t value) {
    std::string encoded(kEncodedLuidSize, '0');
    encoded[0] = value.present ? '1' : '0';
    if (!value.present) return encoded;
    for (std::size_t index = 0; index < 8; ++index) {
      const auto shift = static_cast<unsigned>((7 - index) * 4);
      encoded[1 + index] = hex_digit(value.high >> shift);
      encoded[9 + index] = hex_digit(value.low >> shift);
    }
    return encoded;
  }

  inline std::optional<optional_luid_t> decode_optional_luid(std::string_view encoded) {
    if (encoded.size() != kEncodedLuidSize || (encoded[0] != '0' && encoded[0] != '1')) {
      return std::nullopt;
    }
    optional_luid_t result {.present = encoded[0] == '1'};
    for (std::size_t index = 0; index < 8; ++index) {
      const int high = hex_value(encoded[1 + index]);
      const int low = hex_value(encoded[9 + index]);
      if (high < 0 || low < 0) return std::nullopt;
      result.high = (result.high << 4) | static_cast<std::uint32_t>(high);
      result.low = (result.low << 4) | static_cast<std::uint32_t>(low);
    }
    // Absence has one canonical representation. This prevents ignored bytes
    // from becoming a covert alternate spelling of a privileged selector.
    if (!result.present && (result.high != 0 || result.low != 0)) return std::nullopt;
    return result;
  }
}  // namespace pyrowave::probe_protocol
