/**
 * @file src/host_capabilities.h
 * @brief The LuminalShine API level and capability list reported in HTTPS `/serverinfo`.
 */
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace host_capabilities {
  // Stub: the real values land with the implementation.
  inline constexpr int api_level = 0;

  struct flags_t {
    bool pyrowave = false;
  };

  struct capability_t {
    std::string_view name;
    bool flags_t::*enabled;
  };

  inline constexpr std::array<capability_t, 0> capabilities {};

  inline std::vector<std::string_view> names(const flags_t &) {
    return {};
  }

  inline std::string wire_list(const flags_t &) {
    return {};
  }
}  // namespace host_capabilities
