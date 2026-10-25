/**
 * @file src/platform/windows/vhf_gamepad_maintenance.h
 * @brief Preserve DS4 report cadence and recover stalled client motion streams.
 */
#pragma once

#include <chrono>

namespace platf::vhf_gamepad {
  struct ds4_maintenance_t {
    using clock = std::chrono::steady_clock;
    struct actions_t {
      bool keepalive;
      bool clear_gyro;
      bool request_motion;
    };

    void reset(clock::time_point now) {
      last_report = last_gyro = last_request = now;
      gyro_cleared = false;
    }
    void report_sent(clock::time_point now) { last_report = now; }
    void gyro_received(clock::time_point now) {
      last_gyro = now;
      gyro_cleared = false;
    }
    [[nodiscard]] actions_t poll(clock::time_point now) {
      using namespace std::chrono_literals;
      actions_t actions {
        now - last_report >= 100ms,
        !gyro_cleared && now - last_gyro > 750ms,
        now - last_gyro > 2s && now - last_request > 5s,
      };
      if (actions.keepalive) last_report = now;
      if (actions.clear_gyro) gyro_cleared = true;
      if (actions.request_motion) last_request = now;
      return actions;
    }

  private:
    clock::time_point last_report {}, last_gyro {}, last_request {};
    bool gyro_cleared = false;
  };
}
