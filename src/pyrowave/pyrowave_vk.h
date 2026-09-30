/**
 * @file src/pyrowave/pyrowave_vk.h
 * @brief Minimal Vulkan context owner for the PyroWave GPU codec.
 *
 * PyroWave needs a live Vulkan device plus exactly one VMA `vk_allocator`
 * singleton (see third-party/pyrowave/README.md "Integration contract"). Sunshine
 * has no general-purpose Vulkan device exposed to the encoder layer, so we create
 * and own a self-contained one here. The encoder (and, by analogy, the
 * moonlight-qt decoder) drives PyroWave on this context.
 *
 * Scope note: deliberately contains no Sunshine capture/avcodec coupling.
 * Built only when SUNSHINE_ENABLE_PYROWAVE.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <mutex>

// Public headers from the vendored pyrowave static library (PUBLIC include dirs
// propagate through the linked `pyrowave` target).
#include <vulkan/vulkan_raii.hpp>
#include "vk/vk_allocator.h"

namespace pyrowave_vk {

  /**
   * @brief Required/optional Vulkan device features the codec relies on.
   */
  struct device_caps_t {
    bool shader_float16 = false;  ///< Enables the faster *_fp16 shader paths.
    bool timeline_semaphore = false;
    bool ycbcr_conversion = false;
    uint32_t compute_queue_family = UINT32_MAX;
  };

  /**
   * @brief A VkImage backed by an imported Windows shared texture (a GPU capture
   *        frame). RAII members free themselves; keep alive while the GPU uses it.
   */
  struct imported_image {
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::Image image = nullptr;
    vk::raii::ImageView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    vk::Format format = vk::Format::eUndefined;

    explicit operator bool() const {
      return *image != VK_NULL_HANDLE;
    }
  };

  /**
   * @brief Owns a Vulkan instance/device dedicated to PyroWave plus the VMA
   *        singleton the codec requires.
   *
   * Lifetime: construct once before any PyroWave::Encoder/Decoder and keep alive
   * for their entire lifetime. The `vk_allocator` is a process-global singleton,
   * so at most one `context` may exist per process.
   */
  class context {
  public:
    /**
     * @brief Create the Vulkan context.
     * @param prefer_luid Optional adapter LUID to match the capture GPU
     *        (so encoder input can later be imported without a cross-GPU copy).
     * @return nullptr on failure (no suitable device, missing extensions).
     */
    static std::unique_ptr<context> create(const uint8_t *prefer_luid = nullptr);

    ~context();

    context(const context &) = delete;
    context &operator=(const context &) = delete;

    vk::raii::PhysicalDevice &physical_device() {
      return phys_dev;
    }

    vk::raii::Device &device() {
      return dev;
    }

    const device_caps_t &caps() const {
      return caps_;
    }

    /// The compute queue PyroWave command buffers are submitted on.
    vk::raii::Queue &queue() {
      return compute_queue;
    }

    /**
     * @brief Import a D3D11 NT shared texture on the matching adapter.
     * The caller retains ownership of the handle. Sampling must acquire key 0
     * and external queue ownership, then release both before returning the image.
     */
    imported_image import_d3d11(void *handle, uint32_t width, uint32_t height, vk::Format format);

  private:
    context();
    std::unique_lock<std::mutex> lifetime_lock;

    vk::raii::Context ctx {};
    vk::raii::Instance inst = nullptr;
    vk::raii::PhysicalDevice phys_dev = nullptr;
    vk::raii::Device dev = nullptr;
    vk::raii::Queue compute_queue = nullptr;
    device_caps_t caps_;

    // The VMA singleton required by the vendored allocation layer. Must outlive
    // every PyroWave object; destroyed last.
    std::optional<vk_allocator> allocator;
  };

}  // namespace pyrowave_vk
