/**
 * @file src/platform/windows/display.h
 * @brief Declarations for the Windows display backend.
 */
#pragma once

// standard includes
#include <atomic>
#include <optional>

// platform includes
#include <winsock2.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <Unknwn.h>
#include <winrt/windows.graphics.capture.h>
#include <wrl/client.h>

// local includes
#include "src/platform/common.h"
#include "src/platform/windows/ipc/pipes.h"
#include "src/platform/windows/ipc/process_handler.h"
#include "src/utility.h"
#include "src/video.h"

// Opaque LuminalVGD frame-ring / cursor-section handles (luminal_vgd.h);
// forward-declared so only display_vgd.cpp needs the FFI header.
struct VgdRingHandle;
struct VgdCursorHandle;

namespace platf::dxgi {
  extern const char *format_str[];

  // Add D3D11_CREATE_DEVICE_DEBUG here to enable the D3D11 debug runtime.
  // You should have a debugger like WinDbg attached to receive debug messages.
  auto constexpr D3D11_CREATE_DEVICE_FLAGS = 0;

  template<class T>
  void Release(T *dxgi) {
    dxgi->Release();
  }

  // SEH-wrapped Release for IDXGIOutputDuplication. The body is defined in
  // display_base.cpp under the same #if guards as the existing SEH wrappers.
  // Routing every dup_t teardown through this function (instead of the
  // unguarded Release<>) catches the use-after-free that fires from
  // dxgi.dll's CDXGIOutputDuplicationTonemapper destructor on Windows 11
  // Insider Canary builds (29570+) when capture used a 10-bit / HDR-capable
  // format. Without this, releasing the duplication AVs inside dxgi.dll and
  // takes the whole process down — observed deterministically at
  // dxgi!~CDXGIOutputDuplicationTonemapper+0x21 with a freed ID2D1Bitmap1.
  void seh_release_idxgi_duplication(IDXGIOutputDuplication *dup) noexcept;

  // Wrapper around D3D11CreateDevice with exponential-backoff retry for
  // post-TDR transient failures. Call sites that try to create a D3D11
  // device immediately after a NVENC encode-wait timeout (or any other
  // GPU-side hang) routinely see DXGI_ERROR_UNSUPPORTED (0x887A0004) or
  // DXGI_ERROR_DEVICE_REMOVED (0x887A0005) on the first attempt because
  // the driver is still mid-TDR-recovery. Without this wrapper the
  // encoder restart fails outright and the streaming session enters a
  // recovery loop the user has to kill. The backoff (1s → 2s → 4s → 8s,
  // capped at 4 attempts, ~15s total) gives the GPU time to come back
  // online before bailing. `call_site` is included in the failure log
  // so encoder-vs-DD-test failures stay distinguishable. Non-transient
  // failures (E_FAIL, E_INVALIDARG, etc.) return on the first attempt.
  HRESULT D3D11CreateDeviceWithRecovery(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context,
    const char *call_site) noexcept;

  /**
   * @brief Single-attempt D3D11 device-creation probe for the
   *        pre-flight check at session start.
   *
   * Unlike D3D11CreateDeviceWithRecovery (which retries with backoff
   * for up to ~15 s after a TDR), this is a fast yes/no health check:
   * one D3D11CreateDevice call against the default adapter, release
   * immediately, return the HRESULT verbatim. The streaming session
   * gate uses this to refuse a new session up front when the GPU
   * stack is wedged — gives the Moonlight client an immediate,
   * intelligible failure instead of the encoder thread hanging in
   * display init.
   *
   * Returns S_OK on healthy, the underlying HRESULT (typically
   * DXGI_ERROR_UNSUPPORTED 0x887A0004) on a wedged stack.
   */
  HRESULT D3D11ProbeDeviceHealth() noexcept;

  using factory1_t = util::safe_ptr<IDXGIFactory1, Release<IDXGIFactory1>>;
  using dxgi_t = util::safe_ptr<IDXGIDevice, Release<IDXGIDevice>>;

  void set_last_wgc_adapter_luid(std::optional<LUID> luid);
  std::optional<LUID> get_last_wgc_adapter_luid();
  void set_dxgi_adapter_luid_override(std::optional<LUID> luid);
  std::optional<LUID> get_dxgi_adapter_luid_override();
  using dxgi1_t = util::safe_ptr<IDXGIDevice1, Release<IDXGIDevice1>>;
  using device_t = util::safe_ptr<ID3D11Device, Release<ID3D11Device>>;
  using device1_t = util::safe_ptr<ID3D11Device1, Release<ID3D11Device1>>;
  using device_ctx_t = util::safe_ptr<ID3D11DeviceContext, Release<ID3D11DeviceContext>>;
  using adapter_t = util::safe_ptr<IDXGIAdapter1, Release<IDXGIAdapter1>>;
  using output_t = util::safe_ptr<IDXGIOutput, Release<IDXGIOutput>>;
  using output1_t = util::safe_ptr<IDXGIOutput1, Release<IDXGIOutput1>>;
  using output5_t = util::safe_ptr<IDXGIOutput5, Release<IDXGIOutput5>>;
  using output6_t = util::safe_ptr<IDXGIOutput6, Release<IDXGIOutput6>>;
  using dup_t = util::safe_ptr<IDXGIOutputDuplication, seh_release_idxgi_duplication>;
  using texture2d_t = util::safe_ptr<ID3D11Texture2D, Release<ID3D11Texture2D>>;
  using texture1d_t = util::safe_ptr<ID3D11Texture1D, Release<ID3D11Texture1D>>;
  using resource_t = util::safe_ptr<IDXGIResource, Release<IDXGIResource>>;
  using resource1_t = util::safe_ptr<IDXGIResource1, Release<IDXGIResource1>>;
  using multithread_t = util::safe_ptr<ID3D11Multithread, Release<ID3D11Multithread>>;
  using vs_t = util::safe_ptr<ID3D11VertexShader, Release<ID3D11VertexShader>>;
  using ps_t = util::safe_ptr<ID3D11PixelShader, Release<ID3D11PixelShader>>;
  using blend_t = util::safe_ptr<ID3D11BlendState, Release<ID3D11BlendState>>;
  using input_layout_t = util::safe_ptr<ID3D11InputLayout, Release<ID3D11InputLayout>>;
  using render_target_t = util::safe_ptr<ID3D11RenderTargetView, Release<ID3D11RenderTargetView>>;
  using shader_res_t = util::safe_ptr<ID3D11ShaderResourceView, Release<ID3D11ShaderResourceView>>;
  using buf_t = util::safe_ptr<ID3D11Buffer, Release<ID3D11Buffer>>;
  using raster_state_t = util::safe_ptr<ID3D11RasterizerState, Release<ID3D11RasterizerState>>;
  using sampler_state_t = util::safe_ptr<ID3D11SamplerState, Release<ID3D11SamplerState>>;
  using blob_t = util::safe_ptr<ID3DBlob, Release<ID3DBlob>>;
  using depth_stencil_state_t = util::safe_ptr<ID3D11DepthStencilState, Release<ID3D11DepthStencilState>>;
  using depth_stencil_view_t = util::safe_ptr<ID3D11DepthStencilView, Release<ID3D11DepthStencilView>>;
  using keyed_mutex_t = util::safe_ptr<IDXGIKeyedMutex, Release<IDXGIKeyedMutex>>;

  namespace video {
    using device_t = util::safe_ptr<ID3D11VideoDevice, Release<ID3D11VideoDevice>>;
    using ctx_t = util::safe_ptr<ID3D11VideoContext, Release<ID3D11VideoContext>>;
    using processor_t = util::safe_ptr<ID3D11VideoProcessor, Release<ID3D11VideoProcessor>>;
    using processor_out_t = util::safe_ptr<ID3D11VideoProcessorOutputView, Release<ID3D11VideoProcessorOutputView>>;
    using processor_in_t = util::safe_ptr<ID3D11VideoProcessorInputView, Release<ID3D11VideoProcessorInputView>>;
    using processor_enum_t = util::safe_ptr<ID3D11VideoProcessorEnumerator, Release<ID3D11VideoProcessorEnumerator>>;
  }  // namespace video

  class hwdevice_t;
  struct img_d3d_t;  // display_vram.h

  struct cursor_t {
    std::vector<std::uint8_t> img_data;

    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    int x, y;
    bool visible;
  };

  class gpu_cursor_t {
  public:
    gpu_cursor_t():
        cursor_view {0, 0, 0, 0, 0.0f, 1.0f} {};

    void set_pos(LONG topleft_x, LONG topleft_y, LONG display_width, LONG display_height, DXGI_MODE_ROTATION display_rotation, bool visible) {
      this->topleft_x = topleft_x;
      this->topleft_y = topleft_y;
      this->display_width = display_width;
      this->display_height = display_height;
      this->display_rotation = display_rotation;
      this->visible = visible;
      update_viewport();
    }

    void set_texture(LONG texture_width, LONG texture_height, texture2d_t &&texture) {
      this->texture = std::move(texture);
      this->texture_width = texture_width;
      this->texture_height = texture_height;
      update_viewport();
    }

    void update_viewport() {
      switch (display_rotation) {
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
          cursor_view.TopLeftX = topleft_x;
          cursor_view.TopLeftY = topleft_y;
          cursor_view.Width = texture_width;
          cursor_view.Height = texture_height;
          break;

        case DXGI_MODE_ROTATION_ROTATE90:
          cursor_view.TopLeftX = topleft_y;
          cursor_view.TopLeftY = display_width - texture_width - topleft_x;
          cursor_view.Width = texture_height;
          cursor_view.Height = texture_width;
          break;

        case DXGI_MODE_ROTATION_ROTATE180:
          cursor_view.TopLeftX = display_width - texture_width - topleft_x;
          cursor_view.TopLeftY = display_height - texture_height - topleft_y;
          cursor_view.Width = texture_width;
          cursor_view.Height = texture_height;
          break;

        case DXGI_MODE_ROTATION_ROTATE270:
          cursor_view.TopLeftX = display_height - texture_height - topleft_y;
          cursor_view.TopLeftY = topleft_x;
          cursor_view.Width = texture_height;
          cursor_view.Height = texture_width;
          break;
      }
    }

    texture2d_t texture;
    LONG texture_width;
    LONG texture_height;

    LONG topleft_x;
    LONG topleft_y;

    LONG display_width;
    LONG display_height;
    DXGI_MODE_ROTATION display_rotation;

    shader_res_t input_res;

    D3D11_VIEWPORT cursor_view;

    bool visible;
  };

  class display_base_t: public display_t {
  public:
    // Explicit destructor releases the COM members below in reverse-creation order under SEH.
    // When the underlying display device disappears mid-stream (e.g. a virtual display is removed
    // by sudovda while we still hold IDXGIOutput / ID3D11Device references), invoking Release on
    // those interfaces can access-violate inside dxgi.dll because internal lookup tables already
    // freed the kernel handle for that adapter. The default-generated destructor offers no
    // protection; we wrap each release in __try/__except so an AV during teardown is logged and
    // swallowed instead of becoming a process-killing crash.
    ~display_base_t() override;

    int init(const ::video::config_t &config, const std::string &display_name, bool skip_dd_test = false);

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override;

    factory1_t factory;
    adapter_t adapter;
    output_t output;
    device_t device;
    device_ctx_t device_ctx;
    DXGI_RATIONAL display_refresh_rate;
    int display_refresh_rate_rounded;

    DXGI_MODE_ROTATION display_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    int width_before_rotation;
    int height_before_rotation;

    int client_frame_rate;
    DXGI_RATIONAL client_frame_rate_strict;

    DXGI_FORMAT capture_format;
    D3D_FEATURE_LEVEL feature_level;

    std::unique_ptr<high_precision_timer> timer = create_high_precision_timer();

    typedef enum _D3DKMT_SCHEDULINGPRIORITYCLASS {
      D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE,  ///< Idle priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL,  ///< Below normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL,  ///< Normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL,  ///< Above normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH,  ///< High priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME  ///< Realtime priority class
    } D3DKMT_SCHEDULINGPRIORITYCLASS;

    typedef UINT D3DKMT_HANDLE;

    typedef struct _D3DKMT_OPENADAPTERFROMLUID {
      LUID AdapterLuid;
      D3DKMT_HANDLE hAdapter;
    } D3DKMT_OPENADAPTERFROMLUID;

    typedef struct _D3DKMT_WDDM_2_7_CAPS {
      union {
        struct
        {
          UINT HwSchSupported : 1;
          UINT HwSchEnabled : 1;
          UINT HwSchEnabledByDefault : 1;
          UINT IndependentVidPnVSyncControl : 1;
          UINT Reserved : 28;
        };

        UINT Value;
      };
    } D3DKMT_WDDM_2_7_CAPS;

    typedef struct _D3DKMT_QUERYADAPTERINFO {
      D3DKMT_HANDLE hAdapter;
      UINT Type;
      VOID *pPrivateDriverData;
      UINT PrivateDriverDataSize;
    } D3DKMT_QUERYADAPTERINFO;

    const UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;

    typedef struct _D3DKMT_CLOSEADAPTER {
      D3DKMT_HANDLE hAdapter;
    } D3DKMT_CLOSEADAPTER;

    typedef NTSTATUS(WINAPI *PD3DKMTSetProcessSchedulingPriorityClass)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS);
    typedef NTSTATUS(WINAPI *PD3DKMTOpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID *);
    typedef NTSTATUS(WINAPI *PD3DKMTQueryAdapterInfo)(D3DKMT_QUERYADAPTERINFO *);
    typedef NTSTATUS(WINAPI *PD3DKMTCloseAdapter)(D3DKMT_CLOSEADAPTER *);

    virtual bool is_hdr() override;
    virtual bool get_hdr_metadata(SS_HDR_METADATA &metadata) override;

    const char *dxgi_format_to_string(DXGI_FORMAT format);
    const char *colorspace_to_string(DXGI_COLOR_SPACE_TYPE type);
    virtual std::vector<DXGI_FORMAT> get_supported_capture_formats() = 0;

  protected:
    int get_pixel_pitch() {
      return (capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    }

    virtual capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) = 0;
    virtual capture_e release_snapshot() = 0;
    virtual int complete_img(img_t *img, bool dummy) = 0;
  };

  /**
   * Display component for devices that use software encoders.
   */
  class display_ram_t: public display_base_t {
  public:
    std::shared_ptr<img_t> alloc_img() override;
    int dummy_img(img_t *img) override;
    int complete_img(img_t *img, bool dummy) override;
    std::vector<DXGI_FORMAT> get_supported_capture_formats() override;

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    D3D11_MAPPED_SUBRESOURCE img_info;
    texture2d_t texture;
  };

  /**
   * Display component for devices that use hardware encoders.
   */
  class display_vram_t: public display_base_t, public std::enable_shared_from_this<display_vram_t> {
  public:
    std::shared_ptr<img_t> alloc_img() override;
    int dummy_img(img_t *img_base) override;
    int complete_img(img_t *img_base, bool dummy) override;
    std::vector<DXGI_FORMAT> get_supported_capture_formats() override;

    bool is_codec_supported(std::string_view name, const ::video::config_t &config) override;

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) override;

    std::atomic<uint32_t> next_image_id;
  };

  /**
   * Display duplicator that uses the DirectX Desktop Duplication API.
   */
  class duplication_t {
  public:
    dup_t dup;
    bool has_frame {};
    std::chrono::steady_clock::time_point last_protected_content_warning_time {};

    // Canary-channel triage instrumentation. These exist solely so we can
    // distinguish "capture pipeline never produced a frame" from "capture
    // pipeline is producing frames but every frame is empty" — the two
    // failure modes that present identically (black screen on the client)
    // and that the WER crash dumps cannot disambiguate. None of these
    // affect control flow; if they trigger nothing else will fire either.
    bool first_frame_logged {};                   ///< First S_OK from AcquireNextFrame has been logged.
    bool first_nonempty_frame_logged {};          ///< First S_OK with AccumulatedFrames > 0 has been logged.
    uint32_t consecutive_empty_frames {};         ///< Streak of S_OK results with AccumulatedFrames == 0.
    std::chrono::steady_clock::time_point init_time {};
    std::chrono::steady_clock::time_point last_empty_streak_warning_time {};

    int init(display_base_t *display, const ::video::config_t &config);
    // SPECULATIVE (Win11 Insider 29570 dxgi.dll AV mitigation):
    // device is checked via GetDeviceRemovedReason() before each AcquireNextFrame,
    // and the actual DXGI dispatch is bracketed by SEH so a freed-object access
    // violation is converted to capture_e::reinit instead of crashing the process.
    // Pre-existing callers that pass nullptr fall back to the previous behavior.
    capture_e next_frame(DXGI_OUTDUPL_FRAME_INFO &frame_info, std::chrono::milliseconds timeout, resource_t::pointer *res_p, ID3D11Device *device = nullptr);
    capture_e reset(dup_t::pointer dup_p = dup_t::pointer());
    capture_e release_frame();

    ~duplication_t();
  };

  /**
   * Display backend that uses DDAPI with a software encoder.
   */
  class display_ddup_ram_t: public display_ram_t {
  public:
    int init(const ::video::config_t &config, const std::string &display_name);
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e release_snapshot() override;

    duplication_t dup;
    cursor_t cursor;
  };

  /**
   * Display backend that uses DDAPI with a hardware encoder.
   */
  class display_ddup_vram_t: public display_vram_t {
  public:
    int init(const ::video::config_t &config, const std::string &display_name);
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e release_snapshot() override;

    duplication_t dup;
    sampler_state_t sampler_linear;

    blend_t blend_alpha;
    blend_t blend_invert;
    blend_t blend_disable;

    ps_t cursor_ps;
    vs_t cursor_vs;

    gpu_cursor_t cursor_alpha;
    gpu_cursor_t cursor_xor;

    texture2d_t old_surface_delayed_destruction;
    std::chrono::steady_clock::time_point old_surface_timestamp;
    std::variant<std::monostate, texture2d_t, std::shared_ptr<platf::img_t>> last_frame_variant;
  };

  /**
   * Display backend that uses Windows.Graphics.Capture with a software encoder.
   * This now always uses IPC implementation via display_wgc_ipc_ram_t.
   */
  class display_wgc_ram_t {
  public:
    /**
     * @brief Factory method for initializing WGC RAM capture backend.
     * Always returns the IPC implementation for Windows.Graphics.Capture using a software encoder.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return Shared pointer to the initialized display backend.
     */
    static std::shared_ptr<display_t> create(const ::video::config_t &config, const std::string &display_name);
  };

  /**
   * @class display_wgc_vram_t
   * @brief Factory class for initializing Windows.Graphics.Capture (WGC) display backends using VRAM.
   * Provides a static factory method to create and initialize a display backend for capturing
   * displays via the Windows.Graphics.Capture API, utilizing hardware encoding when available.
   */
  class display_wgc_vram_t {
  public:
    /**
     * @brief Factory method for initializing WGC VRAM capture backend.
     * Always returns the IPC implementation for Windows.Graphics.Capture using a hardware encoder.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return Shared pointer to the initialized display backend.
     */
    static std::shared_ptr<display_t> create(const ::video::config_t &config, const std::string &display_name);
  };

  /**
   * @class display_wgc_ipc_vram_t
   * @brief Display capture backend using Windows.Graphics.Capture (WGC) via a separate capture process.
   * This backend utilizes a separate capture process and synchronizes frames to Sunshine,
   * allowing screen capture even when running as a SYSTEM service.
   */
  class display_wgc_ipc_vram_t: public display_vram_t {
    // Cache for frame forwarding when no new frame is available
    std::shared_ptr<platf::img_t> last_cached_frame;

  public:
    /**
     * @brief Constructs a new display_wgc_ipc_vram_t object.
     * Initializes the WGC IPC VRAM display backend for hardware encoding.
     * Sets up internal state and prepares for display capture via IPC.
     */
    display_wgc_ipc_vram_t();

    /**
     * @brief Destructor for display_wgc_ipc_vram_t.
     * Cleans up resources and IPC session associated with the WGC IPC VRAM display backend.
     */
    ~display_wgc_ipc_vram_t() override;

    /**
     * @brief Factory method to create a WGC IPC VRAM display instance or fallback.
     * Chooses the appropriate backend based on the current system state and configuration.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return Instance of the display backend, using WGC IPC if available, or a secure desktop fallback if not.
     */
    static std::shared_ptr<display_t> create(const ::video::config_t &config, const std::string &display_name);

    /**
     * @brief Initializes the WGC IPC VRAM display backend.
     * Sets up the display backend with the provided configuration and display name.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return 0 on success, negative on failure.
     */
    int init(const ::video::config_t &config, const std::string &display_name);

    /**
     * @brief Captures a snapshot of the display.
     * @param pull_free_image_cb Callback to pull a free image buffer.
     * @param img_out Output parameter for the captured image.
     * @param timeout Maximum time to wait for a frame.
     * @param cursor_visible Whether the cursor should be included in the capture.
     * @return Status of the capture operation.
     */
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;

    /**
     * @brief Fills an image with dummy data.
     * Used for testing or fallback scenarios where a real image is not available.
     * @param img_base Pointer to the image to fill.
     * @return 0 on success, negative on failure.
     */
    int dummy_img(platf::img_t *img_base) override;

  protected:
    /**
     * @brief Acquires the next frame from the display.
     * @param timeout Maximum time to wait for a frame.
     * @param src Output parameter for the source texture.
     * @param frame_qpc Output parameter for the frame's QPC timestamp.
     * @param cursor_visible Whether the cursor should be included in the capture.
     * @return Status of the frame acquisition operation.
     */
    capture_e acquire_next_frame(std::chrono::milliseconds timeout, texture2d_t &src, uint64_t &frame_qpc, bool cursor_visible);

    /**
     * @brief Releases resources or state after a snapshot.
     * @return Status of the release operation.
     */
    capture_e release_snapshot() override;

  private:
    std::unique_ptr<class ipc_session_t> _ipc_session;
    ::video::config_t _config;
    std::string _display_name;
    bool _session_initialized_logged = false;
    bool _frame_locked = false;
  };

  /**
   * @class display_vgd_vram_t
   * @brief Capture backend that consumes the LuminalVGD driver's shared-texture
   * frame ring directly (no WGC helper process, no desktop-capture API).
   *
   * The driver publishes every composed frame of its virtual monitor into a
   * small ring of named keyed-mutex shared textures. This backend claims the
   * freshest published slot (a cross-process CAS keeps the driver's writer off
   * it), GPU-copies the pixels into a pooled img_d3d_t under the slot's keyed
   * mutex, and releases the slot — claims are never held across encode.
   */
  class display_vgd_vram_t: public display_vram_t {
  public:
    ~display_vgd_vram_t() override;

    /**
     * @brief Factory: returns a ring-backed display for `display_name`, or
     * nullptr when the display is not a live LuminalVGD monitor (callers fall
     * back to WGC/DDA).
     */
    static std::shared_ptr<display_t> create(const ::video::config_t &config, const std::string &display_name);

    int init(const ::video::config_t &config, const std::string &display_name);

    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;

    int dummy_img(platf::img_t *img_base) override;

  protected:
    capture_e release_snapshot() override;

  private:
    struct slot_texture_t {
      texture2d_t texture;
      keyed_mutex_t mutex;
    };

    /// Open (or fetch the cached) named shared texture for a ring slot of the
    /// current generation. Returns nullptr on open failure.
    slot_texture_t *slot_texture(uint32_t generation, uint32_t slot);

    ::VgdRingHandle *_ring = nullptr;
    uint64_t _session_id = 0;
    uint32_t _ring_slots = 0;
    /// Generation the cached slot textures were opened for; a bump retires them.
    uint32_t _texture_generation = 0;
    std::vector<slot_texture_t> _slot_textures;
    uint64_t _last_sequence = 0;
    std::string _display_name;

    // Broken-ring detection (see display_vgd.cpp): consecutive texture-open
    // failures, and how long a newer-than-delivered frame has sat unclaimed.
    int _open_failures = 0;
    std::optional<std::chrono::steady_clock::time_point> _undelivered_since;
    // How long the driver's heartbeat has been stale (swapchain unassigned
    // during mode transitions stops the worker without marking the ring).
    std::optional<std::chrono::steady_clock::time_point> _heartbeat_stale_since;

    // --- Hardware-cursor plane (driver caps::HW_CURSOR) ---------------
    // With a cursor-capable driver, frames arrive cursor-free (the driver
    // owns the cursor plane and republishes shape/position into a shared
    // section). The blend machinery mirrors display_ddup_vram_t's; shape
    // conversion reuses the display_vram.cpp utilities.

    /// Initialize the blend states/shaders (only when `_cursor` opened).
    int init_cursor_render(const ::video::config_t &config);
    /// Pull shape/position from the cursor section into the GPU cursors.
    void sync_cursor();
    /// Draw the cursor into a pooled image's render target (mirrors the
    /// DDA blend_cursor lambda; caller holds the image's key-0 mutex).
    void blend_cursor_into(img_d3d_t &d3d_img);

    ::VgdCursorHandle *_cursor = nullptr;
    sampler_state_t _cursor_sampler;
    blend_t _blend_alpha;
    blend_t _blend_invert;
    blend_t _blend_disable;
    ps_t _cursor_ps;
    vs_t _cursor_vs;
    gpu_cursor_t _cursor_alpha;
    gpu_cursor_t _cursor_xor;
    std::vector<std::uint8_t> _cursor_shape_buf;
    /// Shape generation currently uploaded to the GPU cursors.
    uint32_t _cursor_shape_generation = 0;

    // Cursor state baked into the last delivered image. While the desktop
    // is idle the driver publishes nothing on cursor motion (the cursor is
    // its own plane), so a live-state difference triggers a redelivery of
    // `_last_frame` with a fresh blend.
    int32_t _delivered_cursor_x = 0;
    int32_t _delivered_cursor_y = 0;
    bool _delivered_cursor_visible = false;
    uint32_t _delivered_cursor_generation = 0;
    /// Cursor-free copy of the last delivered frame (cursor-only updates).
    texture2d_t _last_frame;
  };

  class display_wgc_ipc_ram_t: public display_ram_t {
  public:
    /**
     * @brief Constructs a new display_wgc_ipc_ram_t object.
     * Initializes internal state for the WGC IPC RAM display backend.
     */
    display_wgc_ipc_ram_t();

    /**
     * @brief Destructor for display_wgc_ipc_ram_t.
     * Cleans up resources associated with the WGC IPC RAM display backend.
     */
    ~display_wgc_ipc_ram_t() override;

    /**
     * @brief Factory method to create a WGC IPC RAM display instance or fallback.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return Instance of the display backend.
     */
    static std::shared_ptr<display_t> create(const ::video::config_t &config, const std::string &display_name);

    /**
     * @brief Initializes the WGC IPC RAM display backend.
     * @param config Video configuration parameters.
     * @param display_name Name of the display to capture.
     * @return 0 on success, negative on failure.
     */
    int init(const ::video::config_t &config, const std::string &display_name);

    /**
     * @brief Captures a snapshot of the display.
     * @param pull_free_image_cb Callback to pull a free image buffer.
     * @param img_out Output parameter for the captured image.
     * @param timeout Maximum time to wait for a frame.
     * @param cursor_visible Whether the cursor should be included in the capture.
     * @return Status of the capture operation.
     */
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;

    /**
     * @brief Fills an image with dummy data.
     * Used for testing or fallback scenarios where a real image is not available.
     * @param img_base Pointer to the image to fill.
     * @return 0 on success, negative on failure.
     */
    int dummy_img(platf::img_t *img_base) override;

  protected:
    /**
     * @brief Releases resources or state after a snapshot.
     * @return Status of the release operation.
     */
    capture_e release_snapshot() override;

  private:
    /**
     * @brief IPC session for communication with capture helper.
     */
    std::unique_ptr<class ipc_session_t> _ipc_session;
    /**
     * @brief Video configuration used for capture.
     */
    ::video::config_t _config;
    /**
     * @brief Name of the display being captured.
     */
    std::string _display_name;

    /**
     * @brief Last width of the staging texture for the base class texture.
     */
    UINT _last_width = 0;
    /**
     * @brief Last height of the staging texture for the base class texture.
     */
    UINT _last_height = 0;
    /**
     * @brief Last DXGI format of the staging texture for the base class texture.
     */
    DXGI_FORMAT _last_format = DXGI_FORMAT_UNKNOWN;

    /**
     * @brief Cache for frame forwarding when no new frame is available, only used in constant capture mode.
     */
    std::shared_ptr<platf::img_t> last_cached_frame;
  };

  /**
   * @brief Temporary DXGI VRAM display backend for secure desktop scenarios.
   * This display backend uses DXGI duplication for capturing the screen when secure desktop is active.
   * It periodically checks if secure desktop is no longer active and, if so, can swap back to WGC.
   */
  class temp_dxgi_vram_t: public display_ddup_vram_t {
  private:
    std::chrono::steady_clock::time_point _last_check_time;
    static constexpr std::chrono::seconds CHECK_INTERVAL {2};  // Check every 2 seconds

  public:
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
  };

  /**
   * @brief Display backend that uses DXGI duplication for secure desktop scenarios.
   * This display can detect when secure desktop is no longer active and swap back to WGC.
   */
  class temp_dxgi_ram_t: public display_ddup_ram_t {
  private:
    /**
     * @brief The last time a check for secure desktop status was performed.
     */
    std::chrono::steady_clock::time_point _last_check_time;

    /**
     * @brief Interval between secure desktop status checks (every 2 seconds).
     */
    static constexpr std::chrono::seconds CHECK_INTERVAL {2};

  public:
    /**
     * @brief Captures a snapshot of the display using DXGI duplication.
     * This method attempts to capture the current frame from the display, handling secure desktop scenarios.
     * If secure desktop is no longer active, it can swap back to WGC.
     * @param pull_free_image_cb Callback to pull a free image buffer.
     * @param img_out Output parameter for the captured image.
     * @param timeout Maximum time to wait for a frame.
     * @param cursor_visible Whether the cursor should be included in the capture.
     * @return Status of the capture operation.
     */
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
  };

  // Type aliases for WGC data structures
  using shared_handle_data_t = platf::dxgi::shared_handle_data_t;
  using config_data_t = platf::dxgi::config_data_t;

}  // namespace platf::dxgi
