/**
 * @file src/pyrowave/pyrowave_encode.h
 * @brief Sunshine encode device + session for the PyroWave GPU wavelet codec.
 *
 * PyroWave is a Vulkan compute/fragment codec, so it does not use the avcodec
 * path. This device mirrors the role of platf::nvenc_encode_device_t: it takes a
 * captured platf::img_t in convert(), produces the encoded bitstream, and the
 * session hands one contiguous packet per frame to Sunshine's RTP layer (which
 * fragments it; the decoder reassembles and parses PyroWave's self-delimiting
 * block headers).
 *
 * The codec-driving core follows WiVRn's server/encoder/video_encoder_pyrowave.cpp.
 * Built when -DSUNSHINE_ENABLE_PYROWAVE=ON. Capture frames arrive as Windows
 * shared D3D11 textures and are sampled directly by Vulkan on the same adapter.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "src/platform/common.h"
#include "src/video.h"

#include "pyrowave_vk.h"

// Vendored PyroWave public headers (include dirs propagate from the linked lib).
#include "pyrowave/pyrowave_encoder.h"
#include "vk/allocation.h"

namespace pyrowave_enc {

  /**
   * @brief Encoded output for a single frame.
   */
  struct encoded_frame {
    std::vector<uint8_t> data;
    int64_t frame_index = 0;
    bool idr = true;  ///< True for full-refresh frames (every block present). With
                      ///< conditional replenishment enabled, most frames omit
                      ///< unchanged blocks and are sent as P-frames.
    int pacing_bitrate_kbps = 0;  ///< Per-frame egress target after adaptive scaling.
  };

  /**
   * @brief encode_device_t implementation backed by PyroWave::Encoder.
   *
   * Owns the Vulkan input image, the host-visible meta/bitstream buffers, and a
   * command buffer/fence. convert() uploads + records + submits the GPU encode;
   * encode_frame() waits, maps the results and packetizes.
   */
  class pyrowave_encode_device_t: public platf::encode_device_t {
  public:
    /**
     * @brief Construct an encode device.
     * @param ctx Shared Vulkan context (owns the device + VMA singleton).
     * @param width,height Stream dimensions.
     * @param bitrate_bps Target bitrate; per-frame byte budget = bitrate/(fps*8).
     * @param fps Frame rate.
     * @return nullptr on failure.
     */
    /// @param shard_payload_size Video payload bytes per RTP packet (block
    ///        packets are aligned to these boundaries; 0 disables alignment).
    /// @param quality_bias Extra bits of initial quantization ceiling (0-3).
    /// @param refresh_interval Conditional replenishment: every block is
    ///        re-sent at least once per N frames; 0 sends all blocks always.
    /// @param chroma444 Encode full-resolution chroma (4:4:4) instead of 4:2:0.
    /// @param adaptive_bitrate Client opt-in: temporarily lower the encode
    ///        bitrate (down to 50%) on observed loss, recovering when clean.
    static std::unique_ptr<pyrowave_encode_device_t> create(
      std::shared_ptr<pyrowave_vk::context> ctx,
      int width,
      int height,
      int64_t bitrate_bps,
      int fps,
      const video::sunshine_colorspace_t &colorspace,
      size_t shard_payload_size = 0,
      int quality_bias = 0,
      int refresh_interval = 0,
      bool chroma444 = false,
      bool adaptive_bitrate = false);

    ~pyrowave_encode_device_t() override;
    bool safe_to_destroy();
    size_t max_output_bytes = 32 * 1024 * 1024;

    /// Upload the captured image, record and submit the GPU encode.
    int convert(platf::img_t &img) override;

    /// Wait for the GPU, packetize, and return the frame bitstream.
    encoded_frame encode_frame(int64_t frame_index);

    /// Request that the next frame re-sends every block (a code-0 full frame).
    /// Wired to the client's IDR request: after packet loss the client asks for
    /// this so stale (keep-previous) blocks heal immediately.
    void request_full_refresh() {
      full_refresh_.store(true, std::memory_order_relaxed);
    }

    /// Record client-reported packet loss. The encoder thread consumes this
    /// before its next GPU submission; no control thread mutates rate state.
    void notify_packet_loss() {
      loss_reported_.store(true, std::memory_order_release);
    }

    void set_fec_video_scale(int percent) {
      fec_video_scale_percent_.store(std::clamp(percent, 1, 100), std::memory_order_release);
    }

  private:
    pyrowave_encode_device_t() = default;

    bool init(int width, int height, int64_t bitrate_bps, int fps);
    size_t prepare_adaptive_submission();  ///< consume loss, update ABR, snapshot pacing, return RDO target
    size_t adaptive_target_size() const;  ///< per-frame RDO budget incl. replenishment savings
    bool upload_image(platf::img_t &img);  ///< memcpy the BGRA capture into the source staging buffer.
    bool ensure_source(uint32_t src_w, uint32_t src_h);  ///< (re)create the GPU source image at capture res.
    bool ensure_cursor(uint32_t w, uint32_t h);  ///< (re)create the cursor image at the cursor size.

    std::shared_ptr<pyrowave_vk::context> ctx_;
    std::unique_ptr<PyroWave::Encoder> enc_;

    int width_ = 0;
    int height_ = 0;
    size_t target_size_ = 0;  ///< per-frame byte budget (base, from the bitrate)
    int base_bitrate_kbps_ = 0;

    // --- Aligned packetization + conditional replenishment ------------------
    size_t shard_payload_size_ = 0;  ///< RTP payload bytes per packet (0 = no alignment)
    int refresh_interval_ = 0;  ///< force-refresh period in frames (0 = replenishment off)
    std::vector<uint64_t> block_hashes_;  ///< per 32x32 block: hash of the last packet sent
    uint64_t frame_counter_ = 0;
    std::atomic<bool> full_refresh_ {true};  ///< next frame is a full (code-0, IDR) frame
    // Mirrors PyroWave::Encoder::sequence_count. The codec pre-increments
    // from 0 (its first frame carries sequence 1), so start at 0 and
    // pre-increment in convert() the same way.
    uint32_t seq_mirror_ = 0;
    bool logged_seq_mismatch_ = false;

    // Adaptive budget: spend replenishment savings on quality. Changed rarely
    // and in coarse steps so the RDO's quant decisions stay stable frame-to-
    // frame (stability is what makes block skipping effective).
    double budget_scale_ = 1.0;
    int low_usage_frames_ = 0;
    int quality_bias_ = 0;
    PyroWave::ChromaSubsampling chroma_ = PyroWave::ChromaSubsampling::Chroma420;
    bool capture_cursor_ = true;  ///< composite the hardware cursor into the frame
    // HDR10 stream (10-bit BT.2020 + PQ): the wavelet codec itself is
    // bit-depth agnostic; HDR means 16-bit unorm planes (10-bit precision),
    // BT.2020-NCL YCbCr math in rgb2yuv, and BT.2020/PQ flags in the
    // sequence header. Requires the zero-copy capture path (the 10-bit
    // scanout buffer arrives as an AR30/AB30 dma-buf).
    bool hdr_ = false;

    // Adaptive bitrate (client opt-in): every full-refresh request marks
    // unrecoverable loss, so back the encode bitrate off multiplicatively
    // (floor 50%) and recover slowly (~3.7%/s at 60 fps) while clean.
    bool adaptive_bitrate_ = false;
    double bitrate_scale_ = 1.0;
    std::atomic<bool> loss_reported_ {false};
    std::atomic<int> fec_video_scale_percent_ {100};
    int submitted_pacing_bitrate_kbps_ = 0;

    // NB: a client-requested refresh MUST be answered with a hard full
    // (code-0) frame: the requester may have missed the stream's initial full
    // frame entirely (startup loss does this routinely) and code-1 keep
    // frames can never initialize its coefficient state. An earlier "spread
    // the refresh over N keep frames" optimization broke exactly that and
    // left such clients permanently degraded.

    // Watchdog: detects a stuck encode (GPU hang or encoder bug) and logs it
    // loudly instead of silently wedging the session. PYROWAVE_WATCHDOG_ABORT=1
    // additionally aborts the process so a supervisor can restart Sunshine.
    void watchdog_proc();
    std::thread watchdog_;
    std::atomic<bool> watchdog_stop_ {false};
    std::atomic<int64_t> op_deadline_ms_ {0};  ///< 0 = idle
    std::atomic<const char *> op_name_ {""};

    // Vulkan input planes (separate R8 images: Y full-res, Cb/Cr half-res for
    // 4:2:0) + their views, passed as the encoder's ViewBuffers.
    image_allocation img_y_, img_cb_, img_cr_;
    vk::raii::ImageView view_y_ = nullptr;
    vk::raii::ImageView view_cb_ = nullptr;
    vk::raii::ImageView view_cr_ = nullptr;

    // Host-visible output buffers (meta + bitstream), with staging fallbacks.
    buffer_allocation meta_buf_, data_buf_;
    buffer_allocation meta_staging_, data_staging_;

    // GPU colour conversion: the captured BGRA frame is uploaded once to a sampled
    // image, then a compute shader (rgb2yuv) writes the three R8 planes with a
    // bilinear downscale to the encode resolution. Replaces the per-pixel CPU path.
    buffer_allocation bgra_staging_;        ///< host-visible BGRA upload buffer (capture res)
    image_allocation img_src_;              ///< sampled B8G8R8A8 source image (capture res)
    vk::raii::ImageView view_src_ = nullptr;
    vk::raii::Sampler sampler_ = nullptr;
    vk::raii::DescriptorSetLayout conv_dsl_ = nullptr;
    vk::raii::PipelineLayout conv_pl_ = nullptr;
    vk::raii::Pipeline conv_pipe_ = nullptr;
    vk::raii::DescriptorPool conv_dpool_ = nullptr;
    vk::raii::DescriptorSet conv_dset_ = nullptr;
    uint32_t src_w_ = 0, src_h_ = 0;        ///< current img_src_ dimensions

    // Zero-copy path: the current frame's imported DRM-PRIME capture image. Held as
    // a member so it outlives the in-flight GPU work (replaced next convert(), after
    // encode_frame() has waited the fence). rgb2yuv samples cur_import_.view instead
    // of img_src_ when this path is active.
    pyrowave_vk::imported_image cur_import_;

    // Cursor compositing (the dma-buf screen plane excludes the hardware cursor).
    image_allocation cursor_img_;            ///< premultiplied BGRA cursor, sampled in rgb2yuv
    vk::raii::ImageView cursor_view_ = nullptr;
    buffer_allocation cursor_staging_;       ///< host-visible cursor upload buffer
    uint32_t cursor_w_ = 0, cursor_h_ = 0;
    bool cursor_initialized_ = false;

    vk::raii::CommandPool cmd_pool_ = nullptr;
    vk::raii::CommandBuffer cmd_ = nullptr;
    vk::raii::Fence fence_ = nullptr;
    bool image_initialized_ = false;
    bool src_initialized_ = false;
    bool busy_ = false;
    bool logged_first_convert_ = false;
    bool logged_first_encode_ = false;
    uint64_t validate_log_count_ = 0;  ///< diagnostic: frames passed through bitstream validation
  };

}  // namespace pyrowave_enc
