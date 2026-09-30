#include "backend.h"
#include "pyrowave_encode.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}

namespace pyrowave {
  namespace {
    class session_t final: public video::encode_session_t {
    public:
      explicit session_t(std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t> input): device(std::move(input)) {}
      ~session_t() override {
        // A timed-out GPU submission may still reference imported capture memory.
        // Keep that memory alive until the isolated worker exits.
        if (device && !device->safe_to_destroy()) {
          BOOST_LOG(error) << "PyroWave GPU still busy; retaining resources until worker exit";
          (void) device.release();
        }
      }
      int convert(platf::img_t &img) override { return device->convert(img); }
      void request_idr_frame() override { device->request_full_refresh(); }
      void request_normal_frame() override {}
      void invalidate_ref_frames(std::int64_t, std::int64_t) override { request_idr_frame(); }
      std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t> device;
    };
  }

  bool available() {
    // Probe independently: never replace/poison the H.264/HEVC/AV1 snapshot.
    static const bool supported = [] {
      auto ctx = pyrowave_vk::context::create();
      if (!ctx) return false;
      std::shared_ptr<pyrowave_vk::context> shared(std::move(ctx));
      video::config_t config {};
      config.encoderCscMode = 2;
      for (bool hdr : {false, true}) {
        config.dynamicRange = hdr ? 1 : 0;
        auto colors = video::colorspace_from_client_config(config, hdr);
        auto device = pyrowave_enc::pyrowave_encode_device_t::create(shared, 128, 128, 8'000'000, 60, colors);
        if (!device) return false;
      }
      return true;
    }();
    return supported;
  }

  video::encoder_t &encoder() {
    static video::encoder_t result = [] {
      auto formats = std::make_unique<video::encoder_platform_formats_t>();
      formats->dev_type = platf::mem_type_e::dxgi;
      formats->pix_fmt_8bit = platf::pix_fmt_e::nv12;
      formats->pix_fmt_10bit = platf::pix_fmt_e::p010;
      // No legacy codec capabilities: this object is only dispatched explicitly.
      return video::encoder_t {"pyrowave", std::move(formats), {}, {}, {}, 0};
    }();
    return result;
  }

  std::unique_ptr<platf::encode_device_t> make_device(platf::display_t &display, const video::config_t &config) {
    auto *d3d = dynamic_cast<platf::dxgi::display_base_t *>(&display);
    if (!d3d || !d3d->adapter || config.chromaSamplingType != 0 ||
        !valid_session(config.width, config.height, config.framerate, config.bitrate, config.pyrowave_packet_size)) return {};
    // Never label an SDR source as HDR. Existing setup must first enable HDR.
    if (config.dynamicRange && !display.is_hdr()) return {};
    DXGI_ADAPTER_DESC1 adapter {};
    if (FAILED(d3d->adapter->GetDesc1(&adapter))) return {};
    auto context = pyrowave_vk::context::create(reinterpret_cast<const uint8_t *>(&adapter.AdapterLuid));
    if (!context) return {};
    auto colors = video::colorspace_from_client_config(config, display.is_hdr());
    auto device = pyrowave_enc::pyrowave_encode_device_t::create(
      std::move(context), config.width, config.height, std::int64_t(config.bitrate) * 1000,
      config.framerate, colors, config.pyrowave_packet_size - sizeof(NV_VIDEO_PACKET));
    if (device) {
      device->colorspace = colors;
      device->max_output_bytes = max_frame_bytes(config.pyrowave_packet_size);
    }
    return device;
  }

  std::unique_ptr<video::encode_session_t> make_session(std::unique_ptr<platf::encode_device_t> device) {
    auto *typed = dynamic_cast<pyrowave_enc::pyrowave_encode_device_t *>(device.get());
    if (!typed) return {};
    device.release();
    return std::make_unique<session_t>(std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t>(typed));
  }

  video::packet_t encode(video::encode_session_t &session, std::int64_t frame) {
    auto *typed = dynamic_cast<session_t *>(&session);
    if (!typed) return {};
    auto result = typed->device->encode_frame(frame);
    if (result.data.empty()) return {};
    return std::make_unique<video::packet_raw_generic>(std::move(result.data), frame, true);
  }
}
