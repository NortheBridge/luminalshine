/**
 * @file src/nvenc/nvenc_base.h
 * @brief Declarations for abstract platform-agnostic base of standalone NVENC encoder.
 */
#pragma once

// lib includes
#include <chrono>
#include <ffnvcodec/nvEncodeAPI.h>

// local includes
#include "nvenc_colorspace.h"
#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"
#include "src/logging.h"
#include "src/video.h"

/**
 * @brief Standalone NVENC encoder
 */
namespace nvenc {

  /**
   * @brief Abstract platform-agnostic base of standalone NVENC encoder.
   *        Derived classes perform platform-specific operations.
   */
  class nvenc_base {
  public:
    /**
     * @param device_type Underlying device type used by derived class.
     */
    explicit nvenc_base(NV_ENC_DEVICE_TYPE device_type);
    virtual ~nvenc_base();

    nvenc_base(const nvenc_base &) = delete;
    nvenc_base &operator=(const nvenc_base &) = delete;

    /**
     * @brief Create the encoder.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    bool create_encoder(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format);

    /**
     * @brief Destroy the encoder.
     *        Derived classes classes call it in the destructor.
     */
    void destroy_encoder();

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Frame index that uniquely identifies the frame.
     *        Afterwards serves as parameter for `invalidate_ref_frames()`.
     *        No restrictions on the first frame index, but later frame indexes must be subsequent.
     * @param force_idr Whether to encode frame as forced IDR.
     * @return Encoded frame.
     */
    nvenc_encoded_frame encode_frame(uint64_t frame_index, bool force_idr);

    /**
     * @brief Perform reference frame invalidation (RFI) procedure.
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error.
     *         After error next frame must be encoded with `force_idr = true`.
     */
    bool invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame);

  protected:
    /**
     * @brief Required. Used for loading NvEnc library and setting `nvenc` variable with `NvEncodeAPICreateInstance()`.
     *        Called during `create_encoder()` if `nvenc` variable is not initialized.
     * @return `true` on success, `false` on error
     */
    virtual bool init_library(uint32_t api_version) = 0;

    /**
     * @brief Required. Used for creating outside-facing input surface,
     *        registering this surface with `nvenc->nvEncRegisterResource()` and setting `registered_input_buffer` variable.
     *        Called during `create_encoder()`.
     * @return `true` on success, `false` on error
     */
    virtual bool create_and_register_input_buffer() = 0;

    /**
     * @brief Optional. Override if you must perform additional operations on the registered input surface in the beginning of `encode_frame()`.
     *        Typically used for interop copy.
     * @return `true` on success, `false` on error
     */
    virtual bool synchronize_input_buffer() {
      return true;
    }

    /**
     * @brief Optional. Override if you want to create encoder in async mode.
     *        In this case must also set `async_event_handle` variable.
     * @param timeout_ms Wait timeout in milliseconds
     * @return `true` on success, `false` on timeout or error
     */
    virtual bool wait_for_async_event(uint32_t timeout_ms) {
      return false;
    }

    /**
     * @brief Optional. Override to put `async_event_handle` back into the
     *        unsignaled state before issuing the next nvEncEncodePicture.
     *
     *        Why this exists: the async completion event is created
     *        auto-reset. A successful wait on it clears the signal, but a
     *        TIMED-OUT wait does not — the signal latches. If NVENC then
     *        completes the picture after our timeout window elapsed, the
     *        event becomes signaled with no consumer. The very next
     *        encode_frame's wait would return WAIT_OBJECT_0 immediately on
     *        that stale signal, before NVENC has actually finished the new
     *        picture, and the subsequent nvEncLockBitstream would read an
     *        uncommitted bitstream — undefined behavior, observed as
     *        bitstream corruption that subsequently trips the heap
     *        validator on RTX 40/50 with split-frame AV1.
     *
     *        Default is a no-op (sync-mode encoders don't need it).
     */
    virtual void reset_async_event() {
    }

    /**
     * @brief Authoritative device-loss check — the corroboration gate for
     *        the encode-wait timeout.
     *
     * An elapsed encode wait is neither necessary nor sufficient evidence
     * of a GPU hang: a healthy but deeply-queued GPU can legitimately take
     * seconds, and a real TDR surfaces as device-removed regardless of how
     * long we waited. Only the device's own verdict may promote a stall to
     * a TDR-class event, so the timeout path consults this before calling
     * tdr::mark_event.
     *
     * Default is "no opinion", which never corroborates — a backend that
     * cannot ask its device must not be able to declare the GPU dead.
     *
     * @param out_reason Receives the platform failure code when available
     *                   (HRESULT bits on Windows), 0 otherwise.
     * @return true only when the device positively reports removal/reset.
     */
    virtual bool device_lost(std::uint32_t &out_reason) {
      out_reason = 0;
      return false;
    }

    bool nvenc_failed(NVENCSTATUS status);

    /// Outcome of the sliced encode-completion wait.
    enum class encode_wait_result {
      completed,  ///< The GPU signalled; the bitstream is ready.
      device_lost,  ///< The device reported removal mid-wait — a real fault, detected within one slice.
      aborted,  ///< Process teardown asked us to stop waiting.
      timed_out,  ///< The full deadline elapsed with no verdict from the device.
    };

    /**
     * @brief Wait for the async completion event, sliced, doing real work
     *        per slice.
     *
     * Slicing is what lets the deadline be OS-scale (seconds) without the
     * encode thread becoming unresponsive for that long — but only because
     * each slice boundary is a decision point:
     *
     *  - the device is re-asked whether it has been lost, so a GENUINE
     *    fault is caught within one slice (~100 ms) rather than after the
     *    whole deadline. That preserves the TDR lifeboat's reaction window
     *    and gets the LuminalVGD ring reader releasing shared textures
     *    promptly, which is the behaviour the pre-change 100 ms timeout
     *    accidentally provided and which must not be lost;
     *  - process teardown is observed, so a stall cannot hold shutdown past
     *    the force-shutdown watchdog.
     *
     * Costs nothing on a healthy frame: the event returns immediately on
     * signal, so a normal frame is still one syscall and no device query.
     *
     * @param out_reason Receives the device failure code on `device_lost`.
     */
    encode_wait_result wait_for_encode_completion(std::uint32_t &out_reason);

    /**
     * @brief Record a hard (full-deadline) stall and report whether the
     *        repeated-stall circuit breaker has tripped.
     *
     * Process-wide, since a wedged GPU affects every session. Exists so a
     * genuinely hung device still escalates on backends that cannot answer
     * `device_lost` — and on machines with TdrLevel=0, where the OS never
     * resets the GPU so the device never reports removal at all.
     */
    bool note_hard_stall_and_check_breaker();

    const NV_ENC_DEVICE_TYPE device_type;

    void *encoder = nullptr;

    struct {
      uint32_t width = 0;
      uint32_t height = 0;
      NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
      uint32_t ref_frames_in_dpb = 0;
      bool rfi = false;
    } encoder_params;

    std::string last_nvenc_error_string;

    // Derived classes set these variables
    void *device = nullptr;  ///< Platform-specific handle of encoding device.
                             ///< Should be set in constructor or `init_library()`.
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< Function pointers list produced by `NvEncodeAPICreateInstance()`.
                                                         ///< Should be set in `init_library()`.
    NV_ENC_REGISTERED_PTR registered_input_buffer = nullptr;  ///< Platform-specific input surface registered with `NvEncRegisterResource()`.
                                                              ///< Should be set in `create_and_register_input_buffer()`.
    void *async_event_handle = nullptr;  ///< (optional) Platform-specific handle of event object event.
                                         ///< Can be set in constructor or `init_library()`, must override `wait_for_async_event()`.
    uint32_t selected_api_version = 0;  ///< API version selected after runtime probing.
    NVENCSTATUS last_nvenc_status = NV_ENC_SUCCESS;

  private:
    NV_ENC_OUTPUT_PTR output_bitstream = nullptr;

    struct {
      uint64_t last_encoded_frame_index = 0;
      bool rfi_needs_confirmation = false;
      std::pair<uint64_t, uint64_t> last_rfi_range;
      logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "NvEnc: encoded frame sizes in kB", ""};
      // Telemetry for the encode-wait-timeout diagnostic. Populated on each
      // successful encode_frame so the timeout site can report "how far did
      // we get and when did we last succeed" without grepping through the
      // surrounding debug log. Stored as steady_clock to remain monotonic
      // across system clock changes.
      std::chrono::steady_clock::time_point session_start_time = std::chrono::steady_clock::time_point::min();
      std::chrono::steady_clock::time_point last_successful_encode_time = std::chrono::steady_clock::time_point::min();
      // True when the most recent nvEncEncodePicture in async mode reached
      // the wait_for_async_event timeout instead of a successful signal —
      // meaning the GPU may still be queued to write into output_bitstream
      // and read from the registered input texture. destroy_encoder uses
      // this to decide whether it must drain the async completion event
      // before freeing those resources, or whether a clean teardown can
      // skip the drain (typical case: the last encode_frame call consumed
      // the event signal, so there's no in-flight work to wait for and the
      // drain would just stall for the full timeout on every shutdown).
      bool has_pending_async = false;
      // Cached at create_encoder time from the matching nvenc_config
      // fields. Per-session immutable: these are derived from the machine's
      // OS configuration (see encode_wait_deadline_from_tdr), which only
      // changes on reboot, so re-reading mid-session could not observe a
      // different value anyway.
      uint32_t encode_wait_timeout_ms = 7000;
      uint32_t encode_wait_poll_slice_ms = 100;
      uint32_t encode_stall_warn_ms = 500;
      // Set once per session the first time a wait crosses the soft
      // threshold, so the warning is one line per session rather than one
      // per slow frame on a chronically-loaded machine.
      bool warned_slow_encode_wait = false;
      // Set whenever nvEncMapInputResource succeeds; cleared when the
      // matching nvEncUnmapInputResource succeeds. On the encode-wait
      // timeout path encode_frame deliberately does NOT unmap (the GPU
      // may still be queued to read from this resource), so this field
      // carries the still-mapped handle from the timed-out frame across
      // to destroy_encoder, which unmaps it after the async drain
      // completes. Without the deferred unmap, the GPU's deferred read
      // from a now-unmapped resource is undefined behavior — observed as
      // the same heap-corruption fast-fail the destroy_encoder drain
      // alone closes the bulk of, but extending the safe-handle window
      // through the input-resource lifecycle too. Holds NV_ENC_INPUT_PTR
      // (an opaque void* in the NVENC ABI).
      void *pending_mapped_resource = nullptr;
    } encoder_state;
  };

  /**
   * @brief Process-wide count of encoder-teardown drain timeouts that
   *        leaked NVENC-registered resources. Non-zero means the bounded
   *        per-process registration pool is shrinking; session teardown
   *        uses this to schedule a clean host restart once idle.
   */
  std::uint64_t drain_leak_count();

  /**
   * @brief Tell in-flight encode waits to stop waiting.
   *
   * Called from main's teardown, alongside the other notify_shutdown
   * hooks. Without it an encode stalled against the OS-scale deadline
   * would hold shutdown for seconds and could outlast the 10 s
   * force-shutdown watchdog.
   */
  void notify_shutdown();

}  // namespace nvenc
