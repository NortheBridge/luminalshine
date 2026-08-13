/**
 * @file src/nvenc/nvenc_d3d11_on_cuda.cpp
 * @brief Definitions for CUDA NVENC encoder with Direct3D11 input surfaces.
 */
#ifdef _WIN32
  // winsock2.h must precede windows.h (pulled in by d3d11.h via nvenc_d3d11.h)
  // so the later boost_process_shim.h winsock2 include doesn't emit a
  // header-order warning.
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>

  // this include
  #include "nvenc_d3d11_on_cuda.h"
  #include "nvenc_api.h"

  // local includes
  #include "nvenc_utils.h"
  #include "src/platform/common.h"

  #include <chrono>
  #include <thread>

  // ffnvcodec's minimal dynlink_cuda.h defines only CU_CTX_SCHED_BLOCKING_SYNC.
  // The value is fixed CUDA driver ABI (cuda.h, CUctx_flags).
  #ifndef CU_CTX_SCHED_YIELD
    #define CU_CTX_SCHED_YIELD 2
  #endif

namespace nvenc {

  nvenc_d3d11_on_cuda::nvenc_d3d11_on_cuda(ID3D11Device *d3d_device):
      nvenc_d3d11(NV_ENC_DEVICE_TYPE_CUDA),
      d3d_device(d3d_device) {
  }

  nvenc_d3d11_on_cuda::~nvenc_d3d11_on_cuda() {
    if (encoder) {
      destroy_encoder();
    }

    if (cuda_context) {
      {
        auto autopop_context = push_context();

        // A failed frame can leave the copy targeting cuda_surface pending
        // on the interop stream (only reachable when the GPU wedged). Give
        // it a bounded drain so the cuMemFree below relies on documented
        // ordering instead of the driver's implicit synchronization.
        if (interop_stream && cuda_functions.cuStreamQuery) {
          const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
          while (cuda_functions.cuStreamQuery(interop_stream) == CUDA_ERROR_NOT_READY &&
                 std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }

        if (cuda_d3d_input_texture) {
          if (cuda_failed(cuda_functions.cuGraphicsUnregisterResource(cuda_d3d_input_texture))) {
            BOOST_LOG(error) << "NvEnc: cuGraphicsUnregisterResource() failed: error " << last_cuda_error;
          }
          cuda_d3d_input_texture = nullptr;
        }

        if (cuda_surface) {
          if (cuda_failed(cuda_functions.cuMemFree(cuda_surface))) {
            BOOST_LOG(error) << "NvEnc: cuMemFree() failed: error " << last_cuda_error;
          }
          cuda_surface = 0;
        }

        if (interop_stream) {
          if (cuda_functions.cuStreamDestroy && cuda_failed(cuda_functions.cuStreamDestroy(interop_stream))) {
            BOOST_LOG(error) << "NvEnc: cuStreamDestroy() failed: error " << last_cuda_error;
          }
          interop_stream = nullptr;
        }
      }

      if (cuda_failed(cuda_functions.cuCtxDestroy(cuda_context))) {
        BOOST_LOG(error) << "NvEnc: cuCtxDestroy() failed: error " << last_cuda_error;
      }
      cuda_context = nullptr;
    }

    if (cuda_functions.dll) {
      FreeLibrary(cuda_functions.dll);
      cuda_functions = {};
    }
  }

  ID3D11Texture2D *nvenc_d3d11_on_cuda::get_input_texture() {
    return d3d_input_texture.GetInterfacePtr();
  }

  bool nvenc_d3d11_on_cuda::init_library(uint32_t api_version) {
    if (!nvenc_d3d11::init_library(api_version)) {
      return false;
    }

    if (device != nullptr) {
      return true;
    }

    constexpr auto dll_name = "nvcuda.dll";

    if (!cuda_functions.dll) {
      cuda_functions.dll = LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    if (cuda_functions.dll) {
      auto load_function = [&]<typename T>(T &location, auto symbol) -> bool {
        location = (T) GetProcAddress(cuda_functions.dll, symbol);
        return location != nullptr;
      };
      if (!load_function(cuda_functions.cuInit, "cuInit") ||
          !load_function(cuda_functions.cuD3D11GetDevice, "cuD3D11GetDevice") ||
          !load_function(cuda_functions.cuCtxCreate, "cuCtxCreate_v2") ||
          !load_function(cuda_functions.cuCtxDestroy, "cuCtxDestroy_v2") ||
          !load_function(cuda_functions.cuCtxPushCurrent, "cuCtxPushCurrent_v2") ||
          !load_function(cuda_functions.cuCtxPopCurrent, "cuCtxPopCurrent_v2") ||
          !load_function(cuda_functions.cuMemAllocPitch, "cuMemAllocPitch_v2") ||
          !load_function(cuda_functions.cuMemFree, "cuMemFree_v2") ||
          !load_function(cuda_functions.cuGraphicsD3D11RegisterResource, "cuGraphicsD3D11RegisterResource") ||
          !load_function(cuda_functions.cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource") ||
          !load_function(cuda_functions.cuGraphicsMapResources, "cuGraphicsMapResources") ||
          !load_function(cuda_functions.cuGraphicsUnmapResources, "cuGraphicsUnmapResources") ||
          !load_function(cuda_functions.cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray") ||
          !load_function(cuda_functions.cuMemcpy2D, "cuMemcpy2D_v2")) {
        BOOST_LOG(error) << "NvEnc: missing CUDA functions in " << dll_name;
        FreeLibrary(cuda_functions.dll);
        cuda_functions = {};
      } else if (!load_function(cuda_functions.cuStreamCreate, "cuStreamCreate") ||
                 !load_function(cuda_functions.cuStreamDestroy, "cuStreamDestroy_v2") ||
                 !load_function(cuda_functions.cuMemcpy2DAsync, "cuMemcpy2DAsync_v2") ||
                 !load_function(cuda_functions.cuStreamQuery, "cuStreamQuery")) {
        // Optional stream-ordered set: every driver this host supports has
        // them, but their absence must degrade to the synchronous interop
        // path, not disable the encoder.
        BOOST_LOG(warning) << "NvEnc: stream-ordered CUDA interop unavailable; using synchronous interop.";
        cuda_functions.cuStreamCreate = nullptr;
        cuda_functions.cuStreamDestroy = nullptr;
        cuda_functions.cuMemcpy2DAsync = nullptr;
        cuda_functions.cuStreamQuery = nullptr;
      }
    } else {
      BOOST_LOG(debug) << "NvEnc: couldn't load CUDA dynamic library " << dll_name;
    }

    if (cuda_functions.dll) {
      IDXGIDevicePtr dxgi_device;
      IDXGIAdapterPtr dxgi_adapter;
      if (d3d_device &&
          SUCCEEDED(d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
          SUCCEEDED(dxgi_device->GetAdapter(&dxgi_adapter))) {
        CUdevice cuda_device;
        // SCHED_YIELD, not SCHED_BLOCKING_SYNC: synchronize_input_buffer()
        // performs three synchronous CUDA calls per frame (map, copy,
        // unmap), and on OS builds that quantize blocking-primitive wakes
        // to the timer interrupt (observed on Windows 11 Canary, which also
        // ignores timer-resolution raises), BLOCKING_SYNC cost a full
        // ~15.6 ms quantum per call — a hard 3x15.6 = 47 ms/frame encode
        // cadence (~21 fps) on the yuv444 10-bit path while the GPU work
        // being awaited measured sub-millisecond. YIELD spin-yields instead:
        // wake latency is scheduler-granular and the CPU cost is bounded by
        // those sub-millisecond waits on this format's path only.
        if (cuda_succeeded(cuda_functions.cuInit(0)) &&
            cuda_succeeded(cuda_functions.cuD3D11GetDevice(&cuda_device, dxgi_adapter)) &&
            cuda_succeeded(cuda_functions.cuCtxCreate(&cuda_context, CU_CTX_SCHED_YIELD, cuda_device)) &&
            cuda_succeeded(cuda_functions.cuCtxPopCurrent(&cuda_context))) {
          device = cuda_context;
        } else {
          BOOST_LOG(error) << "NvEnc: couldn't create CUDA interop context: error " << last_cuda_error;
        }
      } else {
        BOOST_LOG(error) << "NvEnc: couldn't get DXGI adapter for CUDA interop";
      }
    }

    return device != nullptr;
  }

  bool nvenc_d3d11_on_cuda::create_and_register_input_buffer() {
    if (encoder_params.buffer_format != NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
      BOOST_LOG(error) << "NvEnc: CUDA interop is expected to be used only for 10-bit 4:4:4 encoding";
      return false;
    }

    if (!d3d_input_texture) {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = encoder_params.width;
      desc.Height = encoder_params.height * 3;  // Planar YUV
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = dxgi_format_from_nvenc_format(encoder_params.buffer_format);
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;

      if (d3d_device->CreateTexture2D(&desc, nullptr, &d3d_input_texture) != S_OK) {
        BOOST_LOG(error) << "NvEnc: couldn't create input texture";
        return false;
      }
    }

    {
      auto autopop_context = push_context();
      if (!autopop_context) {
        return false;
      }

      if (!cuda_d3d_input_texture) {
        if (cuda_failed(cuda_functions.cuGraphicsD3D11RegisterResource(
              &cuda_d3d_input_texture,
              d3d_input_texture,
              CU_GRAPHICS_REGISTER_FLAGS_NONE
            ))) {
          BOOST_LOG(error) << "NvEnc: cuGraphicsD3D11RegisterResource() failed: error " << last_cuda_error;
          return false;
        }
      }

      if (!cuda_surface) {
        if (cuda_failed(cuda_functions.cuMemAllocPitch(
              &cuda_surface,
              &cuda_surface_pitch,
              // Planar 16-bit YUV
              encoder_params.width * 2,
              encoder_params.height * 3,
              16
            ))) {
          BOOST_LOG(error) << "NvEnc: cuMemAllocPitch() failed: error " << last_cuda_error;
          return false;
        }
      }

      if (!interop_stream && cuda_functions.cuStreamCreate) {
        if (cuda_failed(cuda_functions.cuStreamCreate(&interop_stream, CU_STREAM_NON_BLOCKING))) {
          BOOST_LOG(warning) << "NvEnc: cuStreamCreate() failed (error " << last_cuda_error
                             << "); falling back to synchronous CUDA interop.";
          interop_stream = nullptr;
        }
      }
      if (!interop_poll_timer) {
        interop_poll_timer = platf::create_high_precision_timer();
      }
    }

    if (!registered_input_buffer) {
      NV_ENC_REGISTER_RESOURCE register_resource = {api::register_resource_version(selected_api_version)};
      register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      register_resource.width = encoder_params.width;
      register_resource.height = encoder_params.height;
      register_resource.pitch = cuda_surface_pitch;
      register_resource.resourceToRegister = (void *) cuda_surface;
      register_resource.bufferFormat = encoder_params.buffer_format;
      register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

      if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &register_resource))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterResource() failed: " << last_nvenc_error_string;
        return false;
      }

      registered_input_buffer = register_resource.registeredResource;
    }

    return true;
  }

  bool nvenc_d3d11_on_cuda::synchronize_input_buffer() {
    auto autopop_context = push_context();
    if (!autopop_context) {
      return false;
    }

    // Stream-ordered interop: enqueue map/copy/unmap on the non-blocking
    // stream and poll completion with the high-resolution timer. The legacy
    // synchronous sequence parked the host in a WDDM interop wait THREE
    // times per frame, and OS builds that quantize those wakes to the timer
    // interrupt (observed on Windows 11 Canary, where timer-resolution
    // raises and CUDA scheduling flags are both ignored) turned that into a
    // hard 3 x 15.6 = 47 ms/frame encode cadence (~21 fps) on the yuv444
    // 10-bit path — for sub-millisecond GPU work. Falls back to the
    // synchronous sequence when the stream, entry points, or timer are
    // unavailable.
    const bool stream_ordered = interop_stream && cuda_functions.cuMemcpy2DAsync &&
                                cuda_functions.cuStreamQuery &&
                                interop_poll_timer && *interop_poll_timer;
    const CUstream stream = stream_ordered ? interop_stream : nullptr;

    const auto phase_started = std::chrono::steady_clock::now();
    if (cuda_failed(cuda_functions.cuGraphicsMapResources(1, &cuda_d3d_input_texture, stream))) {
      BOOST_LOG(error) << "NvEnc: cuGraphicsMapResources() failed: error " << last_cuda_error;
      return false;
    }
    const auto phase_mapped = std::chrono::steady_clock::now();

    auto unmap = [&]() -> bool {
      if (cuda_failed(cuda_functions.cuGraphicsUnmapResources(1, &cuda_d3d_input_texture, stream))) {
        BOOST_LOG(error) << "NvEnc: cuGraphicsUnmapResources() failed: error " << last_cuda_error;
        return false;
      }
      return true;
    };
    auto unmap_guard = util::fail_guard(unmap);

    CUarray input_texture_array;
    if (cuda_failed(cuda_functions.cuGraphicsSubResourceGetMappedArray(&input_texture_array, cuda_d3d_input_texture, 0, 0))) {
      BOOST_LOG(error) << "NvEnc: cuGraphicsSubResourceGetMappedArray() failed: error " << last_cuda_error;
      return false;
    }

    {
      CUDA_MEMCPY2D copy_params = {};
      copy_params.srcMemoryType = CU_MEMORYTYPE_ARRAY;
      copy_params.srcArray = input_texture_array;
      copy_params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy_params.dstDevice = cuda_surface;
      copy_params.dstPitch = cuda_surface_pitch;
      // Planar 16-bit YUV
      copy_params.WidthInBytes = encoder_params.width * 2;
      copy_params.Height = encoder_params.height * 3;

      const CUresult copy_result = stream_ordered ?
                                     cuda_functions.cuMemcpy2DAsync(&copy_params, stream) :
                                     cuda_functions.cuMemcpy2D(&copy_params);
      if (cuda_failed(copy_result)) {
        BOOST_LOG(error) << "NvEnc: cuMemcpy2D" << (stream_ordered ? "Async" : "")
                         << "() failed: error " << last_cuda_error;
        return false;
      }
    }
    const auto phase_copied = std::chrono::steady_clock::now();

    unmap_guard.disable();
    if (!unmap()) {
      return false;
    }
    const auto phase_unmapped = std::chrono::steady_clock::now();

    if (stream_ordered) {
      // NVENC consumes cuda_surface right after this returns; the copy must
      // be complete first. Poll instead of blocking: cuStreamQuery is a
      // non-waiting status read, and the poll sleep uses the high-resolution
      // waitable timer — the one wait primitive this OS build does not
      // quantize. The deadline mirrors the encoder's own hang philosophy:
      // a stream that needs a full second for a sub-millisecond copy means
      // the GPU is wedged, and failing the frame routes into the normal
      // encoder error/reinit path.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (true) {
        const CUresult status = cuda_functions.cuStreamQuery(interop_stream);
        if (status == CUDA_SUCCESS) {
          break;
        }
        if (status != CUDA_ERROR_NOT_READY) {
          last_cuda_error = status;
          BOOST_LOG(error) << "NvEnc: cuStreamQuery() failed: error " << last_cuda_error;
          return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          BOOST_LOG(error) << "NvEnc: CUDA interop stream did not complete within 1 s; failing this frame.";
          return false;
        }
        interop_poll_timer->sleep_for(std::chrono::microseconds(500));
      }
    }

    {
      const auto phase_done = std::chrono::steady_clock::now();
      const auto ns = [](auto a, auto b) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      };
      interop_phase_map_ns += ns(phase_started, phase_mapped);
      interop_phase_copy_ns += ns(phase_mapped, phase_copied);
      interop_phase_unmap_ns += ns(phase_copied, phase_unmapped);
      interop_phase_poll_ns += ns(phase_unmapped, phase_done);
      ++interop_phase_frames;
      if (interop_phase_window_start == std::chrono::steady_clock::time_point {}) {
        interop_phase_window_start = phase_done;
      } else if (phase_done - interop_phase_window_start >= std::chrono::seconds(30)) {
        BOOST_LOG(info) << "CUDA interop phases (" << (stream_ordered ? "stream-ordered" : "synchronous")
                        << "): " << interop_phase_frames << " frames; avg ms: map="
                        << (interop_phase_map_ns / 1e6 / interop_phase_frames) << " copy-submit="
                        << (interop_phase_copy_ns / 1e6 / interop_phase_frames) << " unmap="
                        << (interop_phase_unmap_ns / 1e6 / interop_phase_frames) << " poll="
                        << (interop_phase_poll_ns / 1e6 / interop_phase_frames) << ".";
        interop_phase_window_start = phase_done;
        interop_phase_map_ns = interop_phase_copy_ns = interop_phase_unmap_ns = interop_phase_poll_ns = 0;
        interop_phase_frames = 0;
      }
    }

    return true;
  }

  bool nvenc_d3d11_on_cuda::cuda_succeeded(CUresult result) {
    last_cuda_error = result;
    return result == CUDA_SUCCESS;
  }

  bool nvenc_d3d11_on_cuda::cuda_failed(CUresult result) {
    last_cuda_error = result;
    return result != CUDA_SUCCESS;
  }

  nvenc_d3d11_on_cuda::autopop_context::~autopop_context() {
    if (pushed_context) {
      CUcontext popped_context;
      if (parent.cuda_failed(parent.cuda_functions.cuCtxPopCurrent(&popped_context))) {
        BOOST_LOG(error) << "NvEnc: cuCtxPopCurrent() failed: error " << parent.last_cuda_error;
      }
    }
  }

  nvenc_d3d11_on_cuda::autopop_context nvenc_d3d11_on_cuda::push_context() {
    if (cuda_context &&
        cuda_succeeded(cuda_functions.cuCtxPushCurrent(cuda_context))) {
      return {*this, cuda_context};
    } else {
      BOOST_LOG(error) << "NvEnc: cuCtxPushCurrent() failed: error " << last_cuda_error;
      return {*this, nullptr};
    }
  }

}  // namespace nvenc
#endif
