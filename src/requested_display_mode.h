#pragma once

namespace display_mode_policy {
  struct Mode {
    int width {0};
    int height {0};
    int fps {0};

    [[nodiscard]] constexpr bool valid() const noexcept {
      return width > 0 && height > 0 && fps > 0;
    }

    constexpr bool operator==(const Mode &) const noexcept = default;
  };

  [[nodiscard]] constexpr bool should_reconcile(
    bool virtual_display,
    bool explicit_per_client_override,
    Mode launch_mode,
    Mode rtsp_mode
  ) noexcept {
    return virtual_display && !explicit_per_client_override &&
           rtsp_mode.valid() && launch_mode != rtsp_mode;
  }
}  // namespace display_mode_policy
