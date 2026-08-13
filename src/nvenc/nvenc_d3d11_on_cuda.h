/**
 * @file src/nvenc/nvenc_d3d11_on_cuda.h
 * @brief Declarations for CUDA NVENC encoder with Direct3D11 input surfaces.
 */
#pragma once
#ifdef _WIN32
  // lib includes
  #include <ffnvcodec/dynlink_cuda.h>

  // local includes
  #include "nvenc_d3d11.h"

  #include <memory>

namespace platf {
  struct high_precision_timer;
}

namespace nvenc {

  /**
   * @brief Interop Direct3D11 on CUDA NVENC encoder.
   *        Input surface is Direct3D11, encoding is performed by CUDA.
   */
  class nvenc_d3d11_on_cuda final: public nvenc_d3d11 {
  public:
    /**
     * @param d3d_device Direct3D11 device that will create input surface texture.
     *                   CUDA encoding device will be derived from it.
     */
    explicit nvenc_d3d11_on_cuda(ID3D11Device *d3d_device);
    ~nvenc_d3d11_on_cuda();

    ID3D11Texture2D *get_input_texture() override;

  private:
    bool init_library(uint32_t api_version) override;

    bool create_and_register_input_buffer() override;

    bool synchronize_input_buffer() override;

    bool device_lost(std::uint32_t &out_reason) override {
      return d3d_device_lost(d3d_device, out_reason);
    }

    bool cuda_succeeded(CUresult result);

    bool cuda_failed(CUresult result);

    struct autopop_context {
      autopop_context(nvenc_d3d11_on_cuda &parent, CUcontext pushed_context):
          parent(parent),
          pushed_context(pushed_context) {
      }

      ~autopop_context();

      explicit operator bool() const {
        return pushed_context != nullptr;
      }

      nvenc_d3d11_on_cuda &parent;
      CUcontext pushed_context = nullptr;
    };

    autopop_context push_context();

    const ID3D11DevicePtr d3d_device;
    ID3D11Texture2DPtr d3d_input_texture;

    struct {
      tcuInit *cuInit;
      tcuD3D11GetDevice *cuD3D11GetDevice;
      tcuCtxCreate_v2 *cuCtxCreate;
      tcuCtxDestroy_v2 *cuCtxDestroy;
      tcuCtxPushCurrent_v2 *cuCtxPushCurrent;
      tcuCtxPopCurrent_v2 *cuCtxPopCurrent;
      tcuMemAllocPitch_v2 *cuMemAllocPitch;
      tcuMemFree_v2 *cuMemFree;
      tcuGraphicsD3D11RegisterResource *cuGraphicsD3D11RegisterResource;
      tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;
      tcuGraphicsMapResources *cuGraphicsMapResources;
      tcuGraphicsUnmapResources *cuGraphicsUnmapResources;
      tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;
      tcuMemcpy2D_v2 *cuMemcpy2D;
      // Optional stream-ordered interop entry points (loaded best-effort;
      // absence degrades synchronize_input_buffer to the synchronous path).
      tcuStreamCreate *cuStreamCreate;
      tcuStreamDestroy_v2 *cuStreamDestroy;
      tcuMemcpy2DAsync_v2 *cuMemcpy2DAsync;
      tcuStreamQuery *cuStreamQuery;
      HMODULE dll;
    } cuda_functions = {};

    CUresult last_cuda_error = CUDA_SUCCESS;
    CUcontext cuda_context = nullptr;
    CUgraphicsResource cuda_d3d_input_texture = nullptr;
    CUdeviceptr cuda_surface = 0;
    size_t cuda_surface_pitch = 0;

    /// Non-blocking stream carrying the per-frame map/copy/unmap sequence;
    /// completion is polled with the high-resolution timer instead of the
    /// host-blocking interop waits, which cost a full timer quantum each on
    /// OS builds that quantize them (3 calls x 15.6 ms = the 2026-08 hard
    /// ~21 fps encode cadence on the yuv444 10-bit path).
    CUstream interop_stream = nullptr;
    std::unique_ptr<platf::high_precision_timer> interop_poll_timer;

    /// 30 s phase breakdown of synchronize_input_buffer: which interop call
    /// actually hosts the quantized WDDM waits (map / copy submit / unmap /
    /// completion poll). Stream-ordering the sequence did not remove the
    /// stalls, so the next fix is chosen from these numbers, not from API
    /// documentation.
    std::chrono::steady_clock::time_point interop_phase_window_start {};
    std::uint64_t interop_phase_map_ns = 0;
    std::uint64_t interop_phase_copy_ns = 0;
    std::uint64_t interop_phase_unmap_ns = 0;
    std::uint64_t interop_phase_poll_ns = 0;
    std::uint32_t interop_phase_frames = 0;
  };

}  // namespace nvenc
#endif
