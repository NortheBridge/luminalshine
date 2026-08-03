#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <atomic>
  #include "nvenc_d3d12.h"
  #include "nvenc_api.h"
  #include "nvenc_utils.h"
  #include "src/logging.h"

namespace nvenc {
  namespace {
    std::atomic_bool d3d12_nvenc_rejected {false};
    D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
      D3D12_HEAP_PROPERTIES p {};
      p.Type = type;
      p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      p.CreationNodeMask = 1;
      p.VisibleNodeMask = 1;
      return p;
    }
  }

  nvenc_d3d12::nvenc_d3d12(IDXGIAdapter *adapter): nvenc_d3d11(NV_ENC_DEVICE_TYPE_DIRECTX) {
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device_)))) {
      BOOST_LOG(warning) << "NvEnc D3D12: D3D12CreateDevice failed; retaining D3D11 fallback";
      return;
    }
    D3D12_COMMAND_QUEUE_DESC q {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d3d12_device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)))) {
      return;
    }
    IUnknown *queues[] {queue_.GetInterfacePtr()};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (FAILED(D3D11On12CreateDevice(d3d12_device_, flags, nullptr, 0, queues, 1, 0,
                                     &d3d11_device_, &d3d11_context_, nullptr))) {
      BOOST_LOG(warning) << "NvEnc D3D12: D3D11On12CreateDevice failed; retaining D3D11 fallback";
      return;
    }
    d3d11_device_->QueryInterface(IID_PPV_ARGS(&on12_));
    device = d3d12_device_.GetInterfacePtr();
  }

  nvenc_d3d12::~nvenc_d3d12() {
    if (encoder) destroy_encoder();
  }

  bool nvenc_d3d12::valid() const { return !d3d12_nvenc_rejected.load() && d3d12_device_ && queue_ && on12_; }
  ID3D11Device *nvenc_d3d12::d3d11_device() const { return d3d11_device_; }
  ID3D11DeviceContext *nvenc_d3d12::d3d11_context() const { return d3d11_context_; }
  ID3D11Texture2D *nvenc_d3d12::get_input_texture() { return wrapped_input_; }

  bool nvenc_d3d12::create_and_register_input_buffer() {
    const auto format = dxgi_format_from_nvenc_format(encoder_params.buffer_format);
    if (format == DXGI_FORMAT_UNKNOWN || encoder_params.buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
      return false;
    }
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = encoder_params.width;
    desc.Height = encoder_params.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto hp = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d3d12_device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&input_)))) return false;
    if (FAILED(d3d12_device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&input_fence_)))) return false;

    D3D11_RESOURCE_FLAGS rf {D3D11_BIND_RENDER_TARGET, 0, 0, 0};
    ID3D11Resource *wrapped = nullptr;
    if (FAILED(on12_->CreateWrappedResource(input_, &rf, D3D12_RESOURCE_STATE_COMMON,
          D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&wrapped)))) return false;
    wrapped->QueryInterface(IID_PPV_ARGS(&wrapped_input_));
    wrapped->Release();

    NV_ENC_FENCE_POINT_D3D12 reg_fence {NV_ENC_FENCE_POINT_D3D12_VER};
    reg_fence.pFence = input_fence_.GetInterfacePtr();
    NV_ENC_REGISTER_RESOURCE rr {api::register_resource_version(selected_api_version)};
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.width = encoder_params.width;
    rr.height = encoder_params.height;
    rr.resourceToRegister = input_.GetInterfacePtr();
    rr.bufferFormat = encoder_params.buffer_format;
    rr.bufferUsage = NV_ENC_INPUT_IMAGE;
    rr.pInputFencePoint = &reg_fence;
    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &rr))) return false;
    registered_input_buffer = rr.registeredResource;
    return true;
  }

  bool nvenc_d3d12::create_output_buffer() {
    uint64_t bytes_per_frame = uint64_t(encoder_params.width) * encoder_params.height;
    switch (encoder_params.buffer_format) {
      case NV_ENC_BUFFER_FORMAT_NV12:
        bytes_per_frame = bytes_per_frame * 3 / 2;
        break;
      case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        bytes_per_frame *= 3;
        break;
      case NV_ENC_BUFFER_FORMAT_AYUV:
      case NV_ENC_BUFFER_FORMAT_YUV444:
      case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
        bytes_per_frame *= 4;
        break;
      default:
        // Conservative for future packed formats: the output allocation is
        // cheap readback memory and must never be undersized.
        bytes_per_frame *= 4;
        break;
    }
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (2 * bytes_per_frame + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1) &
                 ~(uint64_t(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) - 1);
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hp = heap(D3D12_HEAP_TYPE_READBACK);
    if (FAILED(d3d12_device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output_)))) return false;
    if (FAILED(d3d12_device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&output_fence_)))) return false;
    NV_ENC_FENCE_POINT_D3D12 reg_fence {NV_ENC_FENCE_POINT_D3D12_VER};
    reg_fence.pFence = output_fence_.GetInterfacePtr();
    NV_ENC_REGISTER_RESOURCE rr {api::register_resource_version(selected_api_version)};
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.width = static_cast<uint32_t>(desc.Width);
    rr.height = 1;
    rr.resourceToRegister = output_.GetInterfacePtr();
    rr.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
    rr.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;
    rr.pInputFencePoint = &reg_fence;
    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &rr))) return false;
    registered_output_ = rr.registeredResource;
    NV_ENC_MAP_INPUT_RESOURCE map {api::map_input_resource_version(selected_api_version)};
    map.registeredResource = registered_output_;
    if (nvenc_failed(nvenc->nvEncMapInputResource(encoder, &map))) return false;
    mapped_output_ = map.mappedResource;
    output_bitstream = reinterpret_cast<NV_ENC_OUTPUT_PTR>(&output_desc_);
    return true;
  }

  void nvenc_d3d12::destroy_output_buffer() {
    output_bitstream = nullptr;
    if (mapped_output_) {
      nvenc->nvEncUnmapInputResource(encoder, mapped_output_);
      mapped_output_ = nullptr;
    }
    if (registered_output_) {
      nvenc->nvEncUnregisterResource(encoder, registered_output_);
      registered_output_ = nullptr;
    }
    output_.Release(); output_fence_.Release();
  }

  bool nvenc_d3d12::begin_conversion() {
    if (!wrapped_input_) return false;
    if (input_reusable_ && FAILED(queue_->Wait(input_fence_, input_reusable_))) return false;
    ID3D11Resource *r = wrapped_input_;
    on12_->AcquireWrappedResources(&r, 1);
    acquired_ = true;
    return true;
  }

  bool nvenc_d3d12::end_conversion() {
    if (!acquired_) return false;
    ID3D11Resource *r = wrapped_input_;
    on12_->ReleaseWrappedResources(&r, 1);
    d3d11_context_->Flush();
    conversion_ready_ = input_reusable_ + 1;
    acquired_ = false;
    return SUCCEEDED(queue_->Signal(input_fence_, conversion_ready_));
  }

  void nvenc_d3d12::prepare_picture_resources(NV_ENC_INPUT_PTR mapped, NV_ENC_BUFFER_FORMAT fmt,
                                               NV_ENC_PIC_PARAMS &pic, NV_ENC_LOCK_BITSTREAM &lock) {
    input_reusable_ = conversion_ready_ + 1;
    input_desc_ = {NV_ENC_INPUT_RESOURCE_D3D12_VER};
    input_desc_.pInputBuffer = mapped;
    input_desc_.inputFencePoint = {NV_ENC_FENCE_POINT_D3D12_VER};
    input_desc_.inputFencePoint.pFence = input_fence_.GetInterfacePtr();
    input_desc_.inputFencePoint.waitValue = conversion_ready_;
    input_desc_.inputFencePoint.signalValue = input_reusable_;
    input_desc_.inputFencePoint.bWait = 1;
    input_desc_.inputFencePoint.bSignal = 1;
    output_desc_ = {NV_ENC_OUTPUT_RESOURCE_D3D12_VER};
    output_desc_.pOutputBuffer = mapped_output_;
    output_desc_.outputFencePoint = {NV_ENC_FENCE_POINT_D3D12_VER};
    output_desc_.outputFencePoint.pFence = output_fence_.GetInterfacePtr();
    output_desc_.outputFencePoint.signalValue = ++output_ready_;
    output_desc_.outputFencePoint.bSignal = 1;
    pic.inputBuffer = reinterpret_cast<NV_ENC_INPUT_PTR>(&input_desc_);
    pic.bufferFmt = fmt;
    pic.outputBitstream = reinterpret_cast<NV_ENC_OUTPUT_PTR>(&output_desc_);
    lock.outputBitstream = reinterpret_cast<NV_ENC_OUTPUT_PTR>(&output_desc_);
  }

  bool nvenc_d3d12::device_lost(std::uint32_t &reason) {
    reason = 0;
    if (!d3d12_device_) return false;
    const HRESULT hr = d3d12_device_->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) return false;
    reason = static_cast<uint32_t>(hr);
    return true;
  }

  void nvenc_d3d12::cleanup_rejected_initialize() {
    // At least one NVIDIA driver branch raises a non-C++ SEH exception when
    // DestroyEncoder is called after a rejected D3D12 InitializeEncoder.
    // Abandon this never-initialized session and permanently gate D3D12 off
    // for the process; the driver/process owns reclamation, while subsequent
    // probes and real sessions take the safe D3D11 path.
    encoder = nullptr;
    d3d12_nvenc_rejected.store(true);
  }
}
#endif
