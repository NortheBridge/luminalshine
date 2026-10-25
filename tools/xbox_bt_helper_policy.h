#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace xbox_bt_helper {
  // Instance IDs are normalized to upper case by the caller. An actual
  // Bluetooth ancestor is required; a virtual HID path alone is insufficient.
  constexpr bool is_bluetooth_instance(std::wstring_view id) {
    return id.starts_with(L"BTHENUM\\") || id.starts_with(L"BTHLEDEVICE\\");
  }

  constexpr unsigned bounded_hold_ms(unsigned requested) {
    return std::clamp(requested, 1u, 1000u);
  }

  template<class TryClaim>
  std::optional<std::uint32_t> claim_free_slot(std::uint32_t reported_limit,
                                              std::uint32_t protocol_limit,
                                              TryClaim &&try_claim) {
    // Streaming allocates low indices first. Ownership is decided atomically
    // by the driver, never by a separate query followed by destructive reuse.
    for (auto id = (std::min)(reported_limit, protocol_limit); id > 0; --id) {
      if (try_claim(id - 1)) {
        return id - 1;
      }
    }
    return std::nullopt;
  }
}
