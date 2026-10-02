/**
 * @file src/platform/windows/exact_device_window_router_policy.h
 * @brief Pure policy used by the interactive exact-device window router.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace platf::window_router_policy {
  struct rect_t {
    long left {};
    long top {};
    long right {};
    long bottom {};

    friend bool operator==(const rect_t &, const rect_t &) = default;
  };

  struct device_identity_t {
    std::string device_id;
    std::string display_name;
    bool active {false};
    bool virtual_display {false};
  };

  struct window_traits_t {
    bool visible {false};
    bool iconic {false};
    bool root {false};
    bool tool_window {false};
    bool no_activate {false};
    bool helper_process {false};
    bool shell_surface {false};
  };

  struct routing_token_t {
    std::uint64_t config_generation {};
    std::uint64_t topology_generation {};

    friend bool operator==(const routing_token_t &, const routing_token_t &) = default;
  };

  inline bool token_is_current(routing_token_t candidate, routing_token_t current) noexcept {
    return candidate.config_generation != 0 && candidate == current;
  }

  inline bool may_run_delayed_reapply(
    std::uint64_t expected_generation,
    std::uint64_t current_generation,
    std::uint64_t expected_connection_epoch,
    std::uint64_t current_connection_epoch,
    bool restore_requested,
    bool stop_requested
  ) noexcept {
    return expected_generation != 0 && expected_generation == current_generation &&
           expected_connection_epoch == current_connection_epoch && !restore_requested && !stop_requested;
  }

  inline bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      const auto lower = [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<unsigned char>(ch - 'A' + 'a') : ch;
      };
      if (lower(static_cast<unsigned char>(lhs[i])) != lower(static_cast<unsigned char>(rhs[i]))) {
        return false;
      }
    }
    return true;
  }

  inline std::optional<std::string> select_exact_active_virtual_display(
    std::span<const device_identity_t> devices,
    std::string_view exact_device_id
  ) {
    if (exact_device_id.empty()) {
      return std::nullopt;
    }
    for (const auto &device : devices) {
      if (device.active && device.virtual_display && !device.display_name.empty() && ascii_iequals(device.device_id, exact_device_id)) {
        return device.display_name;
      }
    }
    return std::nullopt;
  }

  /**
   * The dark-recovery marker identifies logical anchored-exclusive mode. Its
   * physical Windows arrangement is intentionally serialized as
   * "extended_primary_isolated", so the arrangement string must not be used
   * as the router admission signal.
   */
  inline bool should_arm_exact_router(
    bool dark_recovery_anchor,
    std::string_view exact_device_id,
    std::string_view configured_device_id
  ) noexcept {
    return dark_recovery_anchor && !exact_device_id.empty() && !configured_device_id.empty() &&
           ascii_iequals(exact_device_id, configured_device_id);
  }

  inline bool should_route_window(const window_traits_t &traits) noexcept {
    return traits.visible && !traits.iconic && traits.root && !traits.tool_window &&
           !traits.no_activate && !traits.helper_process && !traits.shell_surface;
  }

  inline rect_t route_rect(
    const rect_t &source_monitor,
    const rect_t &target_work_area,
    const rect_t &window,
    bool fullscreen
  ) noexcept {
    const long target_width_available = std::max(1L, target_work_area.right - target_work_area.left);
    const long target_height_available = std::max(1L, target_work_area.bottom - target_work_area.top);
    const long window_width = std::max(1L, window.right - window.left);
    const long window_height = std::max(1L, window.bottom - window.top);
    const long width = fullscreen ? target_width_available : std::min(window_width, target_width_available);
    const long height = fullscreen ? target_height_available : std::min(window_height, target_height_available);
    const long relative_x = window.left - source_monitor.left;
    const long relative_y = window.top - source_monitor.top;
    const long x = fullscreen ? target_work_area.left :
                                std::clamp(target_work_area.left + relative_x, target_work_area.left, target_work_area.right - width);
    const long y = fullscreen ? target_work_area.top :
                                std::clamp(target_work_area.top + relative_y, target_work_area.top, target_work_area.bottom - height);
    return {x, y, x + width, y + height};
  }

  /**
   * Coalesces noisy WinEvent callbacks and tags every queued HWND with the
   * active router generation. Reconfiguration clears the set, so Windows
   * messages left in the queue from a retired display become harmless.
   */
  class pending_window_set_t {
  public:
    explicit pending_window_set_t(std::size_t capacity = 256):
        capacity_(capacity) {}

    bool enqueue(std::uintptr_t window, routing_token_t token) {
      if (window == 0 || token.config_generation == 0 || token.topology_generation == 0 || pending_.size() >= capacity_) {
        return false;
      }
      return pending_.emplace(window, token).second;
    }

    bool take_if_current(std::uintptr_t window, routing_token_t current) {
      const auto it = pending_.find(window);
      if (it == pending_.end()) {
        return false;
      }
      const bool matches = token_is_current(it->second, current);
      pending_.erase(it);
      return matches;
    }

    void clear() noexcept {
      pending_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
      return pending_.size();
    }

  private:
    std::size_t capacity_;
    std::unordered_map<std::uintptr_t, routing_token_t> pending_;
  };
}  // namespace platf::window_router_policy
