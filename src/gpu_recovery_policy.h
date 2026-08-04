/**
 * @file src/gpu_recovery_policy.h
 * @brief Process-lifetime containment after a GPU/display recovery edge.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace gpu_recovery_policy {
  inline std::atomic<bool> g_force_d3d11 {false};
  inline std::atomic<std::uint64_t> g_abort_generation {0};

  inline bool force_d3d11() noexcept {
    return g_force_d3d11.load(std::memory_order_acquire);
  }

  inline std::uint64_t abort_generation() noexcept {
    return g_abort_generation.load(std::memory_order_acquire);
  }

  inline bool open_d3d11_circuit() noexcept {
    const bool first = !g_force_d3d11.exchange(true, std::memory_order_acq_rel);
    g_abort_generation.fetch_add(1, std::memory_order_acq_rel);
    return first;
  }
}  // namespace gpu_recovery_policy
