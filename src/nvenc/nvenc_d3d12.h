/** Native Direct3D 12 NVENC backend with explicit producer/consumer fences. */
#pragma once
#ifdef _WIN32
  #include <comdef.h>
  #include <d3d11.h>
  #include <d3d11on12.h>
  #include <d3d12.h>

  #include "nvenc_d3d11.h"

namespace nvenc {
  _COM_SMARTPTR_TYPEDEF(ID3D11DeviceContext, IID_ID3D11DeviceContext);
  _COM_SMARTPTR_TYPEDEF(ID3D11On12Device, IID_ID3D11On12Device);
  _COM_SMARTPTR_TYPEDEF(ID3D12Device, IID_ID3D12Device);
  _COM_SMARTPTR_TYPEDEF(ID3D12CommandQueue, IID_ID3D12CommandQueue);
  _COM_SMARTPTR_TYPEDEF(ID3D12Resource, IID_ID3D12Resource);
  _COM_SMARTPTR_TYPEDEF(ID3D12Fence, IID_ID3D12Fence);

  class nvenc_d3d12 final: public nvenc_d3d11 {
  public:
    explicit nvenc_d3d12(IDXGIAdapter *adapter);
    ~nvenc_d3d12();

    bool valid() const;
    ID3D11Device *d3d11_device() const;
    ID3D11DeviceContext *d3d11_context() const;
    ID3D11Texture2D *get_input_texture() override;

    // Bracket D3D11On12 rendering into the native D3D12 NVENC input.
    bool begin_conversion();
    bool end_conversion();

  private:
    bool create_and_register_input_buffer() override;
    bool create_output_buffer() override;
    void destroy_output_buffer() override;
    void prepare_picture_resources(NV_ENC_INPUT_PTR, NV_ENC_BUFFER_FORMAT,
                                   NV_ENC_PIC_PARAMS &, NV_ENC_LOCK_BITSTREAM &) override;
    void cleanup_rejected_initialize() override;
    bool device_lost(std::uint32_t &out_reason) override;

    ID3D12DevicePtr d3d12_device_;
    ID3D12CommandQueuePtr queue_;
    ID3D11DevicePtr d3d11_device_;
    ID3D11DeviceContextPtr d3d11_context_;
    ID3D11On12DevicePtr on12_;
    ID3D12ResourcePtr input_;
    ID3D11Texture2DPtr wrapped_input_;
    ID3D12ResourcePtr output_;
    ID3D12FencePtr input_fence_;
    ID3D12FencePtr output_fence_;
    NV_ENC_REGISTERED_PTR registered_output_ = nullptr;
    NV_ENC_INPUT_PTR mapped_output_ = nullptr;
    NV_ENC_INPUT_RESOURCE_D3D12 input_desc_ {};
    NV_ENC_OUTPUT_RESOURCE_D3D12 output_desc_ {};
    uint64_t conversion_ready_ = 0;
    uint64_t input_reusable_ = 0;
    uint64_t output_ready_ = 0;
    bool acquired_ = false;
  };
}
#endif
