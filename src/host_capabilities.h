/**
 * @file src/host_capabilities.h
 * @brief The LuminalShine API level and capability list reported in HTTPS `/serverinfo`.
 * @details `appversion` and `GfeVersion` are GameStream emulation constants, and the release in
 *          `LuminalShineVersion` is no contract. These two fields let a client check what the
 *          host supports without parsing release strings.
 */
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace host_capabilities {
  /**
   * @brief The LuminalShine client protocol level, reported as `LuminalShineApiLevel`.
   * @details Bump it by one with every client-visible protocol change: a new `/serverinfo`
   *          field, request parameter, response element or capability name, a changed
   *          meaning or format of an existing one, or the removal of one. Never lower it or
   *          reuse a level. A client compares it with `>=`. It does not replace the
   *          capability list, which says what this host can do right now.
   */
  inline constexpr int api_level = 1;

  /**
   * @brief Runtime state that decides which capabilities this host reports.
   */
  struct flags_t {
    bool pyrowave = false;  ///< `config::pyrowave.enabled && pyrowave::available()` (SDR 4:2:0 probed), as `ServerCodecModeSupport` uses.
  };

  /**
   * @brief One capability name and the flag that turns it on.
   */
  struct capability_t {
    std::string_view name;  ///< Lowercase kebab-case: `a-z`, `0-9` and `-`, never `,`.
    bool flags_t::*enabled;
  };

  /**
   * @brief Every capability, in wire order.
   * @details List only features that exist in this build and that a client can act on, not
   *          proposed ones. Append new entries at the end; never rename or reuse a name.
   */
  inline constexpr std::array capabilities {
    capability_t {"pyrowave", &flags_t::pyrowave},
  };

  /**
   * @brief The capabilities turned on by @p flags, in wire order.
   */
  inline std::vector<std::string_view> names(const flags_t &flags) {
    std::vector<std::string_view> result;
    for (const auto &capability : capabilities) {
      if (flags.*capability.enabled) {
        result.push_back(capability.name);
      }
    }
    return result;
  }

  /**
   * @brief Join capability names the way `LuminalShineCapabilities` carries them: `,` with no spaces.
   */
  inline std::string join_capabilities(std::span<const std::string_view> capability_names) {
    std::string result;
    for (const auto name : capability_names) {
      if (!result.empty()) {
        result += ',';
      }
      result += name;
    }
    return result;
  }

  /**
   * @brief The `LuminalShineCapabilities` text for @p flags, empty when none.
   */
  inline std::string wire_list(const flags_t &flags) {
    return join_capabilities(names(flags));
  }
}  // namespace host_capabilities
