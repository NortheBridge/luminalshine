/**
 * @file src/nvenc/nvenc_d3d11.h
 * @brief Declarations for abstract Direct3D11 NVENC encoder.
 */
#pragma once
#ifdef _WIN32

  // standard includes
  #include <comdef.h>
  #include <d3d11.h>

  // local includes
  #include "nvenc_base.h"

namespace nvenc {

  _COM_SMARTPTR_TYPEDEF(ID3D11Device, IID_ID3D11Device);
  _COM_SMARTPTR_TYPEDEF(ID3D11Texture2D, IID_ID3D11Texture2D);
  _COM_SMARTPTR_TYPEDEF(IDXGIDevice, IID_IDXGIDevice);
  _COM_SMARTPTR_TYPEDEF(IDXGIAdapter, IID_IDXGIAdapter);

  /**
   * @brief Abstract Direct3D11 NVENC encoder.
   *        Encapsulates common code used by native and interop implementations.
   */
  class nvenc_d3d11: public nvenc_base {
  public:
    explicit nvenc_d3d11(NV_ENC_DEVICE_TYPE device_type);
    ~nvenc_d3d11();

    /**
     * @brief Get input surface texture.
     * @return Input surface texture.
     */
    virtual ID3D11Texture2D *get_input_texture() = 0;

  protected:
    bool init_library(uint32_t api_version) override;
    bool wait_for_async_event(uint32_t timeout_ms) override;
    void reset_async_event() override;

    /**
     * @brief SEH-guarded ID3D11Device::GetDeviceRemovedReason — the
     *        corroboration source for nvenc_base::device_lost.
     *
     * Shared by both D3D11 subclasses, which each own their own device
     * pointer privately. SEH-guarded because the call is made precisely
     * when the graphics stack is suspect, and a faulting device can take
     * the query itself down; an access violation there must read as
     * "device is gone", not crash the encode thread.
     *
     * @return true only on a positive removal/reset verdict.
     */
    static bool d3d_device_lost(ID3D11Device *dev, std::uint32_t &out_reason);

  private:
    HMODULE dll = nullptr;
    uint32_t function_list_api_version = 0;
    uint32_t max_driver_api_version = 0;
  };

}  // namespace nvenc
#endif
