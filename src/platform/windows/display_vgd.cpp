/**
 * @file src/platform/windows/display_vgd.cpp
 * @brief Capture backend consuming the LuminalVGD driver's frame ring.
 *
 * The LuminalVGD IddCx driver GPU-copies every composed frame of its virtual
 * monitor into a ring of named keyed-mutex shared textures and publishes slot
 * metadata through a shared section (see src/drivers/luminal-display,
 * docs/DESIGN.md). This backend is the ring's reader:
 *
 *   claim freshest slot (cross-process CAS PUBLISHED -> READING)
 *     -> AcquireSync(1, 100 ms) on the slot texture's keyed mutex
 *     -> CopyResource into a pooled img_d3d_t (own mutex, key-0 convention)
 *     -> ReleaseSync(1), release the claim
 *
 * The claim, not the mutex, is what keeps the driver's writer off the slot;
 * both are held only for the duration of one GPU copy, so the ring never
 * back-pressures on the encoder. Slot textures are opened by generation-
 * stamped name once and cached; a generation bump (mode change, driver
 * rebuild) retires the cache.
 *
 * Deliberately copy-based for now: adopting slot textures zero-copy into the
 * encoder path would mix the ring's 0/1 keyed-mutex protocol with the
 * encoder's key-0 convention (texture_lock_helper / encode devices acquire
 * key 0) and hold claims across encode. Revisit once the encoder-side key
 * plumbing is parameterized.
 */

// standard includes
#include <atomic>
#include <chrono>
#include <thread>

// platform includes
#include <winsock2.h>

// local includes
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/display_vram.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/virtual_display_vgd.h"

#include <luminal_vgd.h>

namespace platf::dxgi {

  using namespace std::literals;

  namespace {

    // Mirrors luminal-driver-proto KMTX_* — the reader side of the ring's
    // keyed-mutex protocol (key 0 = driver pre-first-publish, key 1 after;
    // both sides bound every acquire).
    constexpr uint64_t kVgdMutexKeyHost = 1;
    constexpr uint32_t kVgdMutexTimeoutMs = 100;

    // proto ring_state::*
    constexpr uint32_t kRingStateActive = 1;
    constexpr uint32_t kRingStateRebuilding = 2;
    constexpr uint32_t kRingStateDead = 3;

    /// Driver heartbeats the ring header at least every 500 ms even when no
    /// frames flow; treat several missed beats as the worker being stopped.
    constexpr auto kHeartbeatStale = std::chrono::milliseconds(2000);

    /// How long to wait out a stale heartbeat before reinitializing: mode
    /// transitions unassign the swapchain (worker stops, heartbeat halts)
    /// for a few seconds; only a persistently silent ring is worth the
    /// cost of a capture teardown.
    constexpr auto kHeartbeatReinitGrace = std::chrono::seconds(10);

    /// Circuit breaker: a session whose ring consistently fails to deliver
    /// (texture opens failing, or frames publishing that we can never claim)
    /// is remembered here so the factory's next attempt refuses and the
    /// stream falls back to WGC/DDA instead of staying black.
    std::atomic<uint64_t> g_broken_session {0};

    /// Consecutive slot-texture open failures before giving up on the ring.
    constexpr int kMaxOpenFailures = 20;

    /// If the driver is publishing frames but none reach us for this long,
    /// the reader side is broken (claim CAS or texture path) — bail out.
    constexpr auto kDeliveryStall = std::chrono::seconds(5);

    // SEH-isolated mutex ops: the virtual display (and with it the D3D
    // device backing the shared texture) can vanish mid-stream; keyed-mutex
    // calls on a dead device may access-violate inside dxgi.dll. Same
    // compiler split as display_wgc.cpp's seh_release_ipc_frame_.
#if defined(_MSC_VER) || defined(__clang__)
    HRESULT seh_acquire_sync(IDXGIKeyedMutex *mutex, uint64_t key, DWORD timeout_ms) noexcept {
      __try {
        return mutex->AcquireSync(key, timeout_ms);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return DXGI_ERROR_DEVICE_REMOVED;
      }
    }

    HRESULT seh_release_sync(IDXGIKeyedMutex *mutex, uint64_t key) noexcept {
      __try {
        return mutex->ReleaseSync(key);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return DXGI_ERROR_DEVICE_REMOVED;
      }
    }
#else
    HRESULT seh_acquire_sync(IDXGIKeyedMutex *mutex, uint64_t key, DWORD timeout_ms) noexcept {
      return mutex->AcquireSync(key, timeout_ms);
    }

    HRESULT seh_release_sync(IDXGIKeyedMutex *mutex, uint64_t key) noexcept {
      return mutex->ReleaseSync(key);
    }
#endif

    /// 16-byte-aligned immutable constant buffer (display_vram.cpp's
    /// make_buffer is file-local there).
    template<class T>
    buf_t make_cbuffer(device_t::pointer device, const T &t) {
      static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");
      D3D11_BUFFER_DESC buffer_desc {
        sizeof(T),
        D3D11_USAGE_IMMUTABLE,
        D3D11_BIND_CONSTANT_BUFFER
      };
      D3D11_SUBRESOURCE_DATA init_data {&t};

      buf_t::pointer buf_p;
      auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
      if (status) {
        BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }
      return buf_t {buf_p};
    }

  }  // namespace

  display_vgd_vram_t::~display_vgd_vram_t() {
    _slot_textures.clear();
    if (_cursor) {
      vgd_cursor_close(_cursor);
      _cursor = nullptr;
    }
    if (_ring) {
      vgd_ring_close(_ring);
      _ring = nullptr;
    }
  }

  std::shared_ptr<display_t> display_vgd_vram_t::create(const ::video::config_t &config, const std::string &display_name) {
    auto disp = std::make_shared<display_vgd_vram_t>();
    if (disp->init(config, display_name)) {
      return nullptr;
    }
    return disp;
  }

  int display_vgd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    auto target = VDISPLAY::vgd::ring_target_for_display(display_name);
    if (!target) {
      // Not a LuminalVGD monitor (or no live session) — caller falls back.
      return -1;
    }
    if (g_broken_session.load(std::memory_order_acquire) == target->session_id) {
      BOOST_LOG(warning) << "LuminalVGD capture: ring of session 0x" << std::hex
                         << target->session_id << std::dec
                         << " marked broken earlier; using fallback capture.";
      return -1;
    }

    if (display_base_t::init(config, display_name, true /* skip_dd_test: ring capture doesn't use Desktop Duplication */)) {
      return -1;
    }

    // The driver creates the ring section at monitor plug; capture starts
    // well after that, but tolerate a beat of PnP lag.
    VgdRingHandle *ring = nullptr;
    for (int attempt = 0; attempt < 10 && !ring; ++attempt) {
      ring = vgd_ring_open(target->session_id, target->ring_slots);
      if (!ring) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (!ring) {
      BOOST_LOG(warning) << "LuminalVGD capture: ring section did not appear for session 0x"
                         << std::hex << target->session_id << std::dec << "; falling back.";
      return -1;
    }

    _ring = ring;
    _session_id = target->session_id;
    _ring_slots = target->ring_slots;
    _display_name = display_name;
    capture_format = DXGI_FORMAT_UNKNOWN;  // latched from the first claimed frame

    // Hardware-cursor plane: with a cursor-capable driver (build >= 4) the
    // cursor is NOT in the frames — it must be blended here. A missing
    // section means an older driver still composing the cursor; both are
    // fine. What is NOT fine is a cursor-capable driver with broken blend
    // resources (the stream would simply have no cursor), so that falls
    // back to WGC, which draws the cursor itself.
    _cursor = vgd_cursor_open(_session_id);
    if (_cursor) {
      if (init_cursor_render(config)) {
        BOOST_LOG(warning) << "LuminalVGD capture: cursor blend init failed; "
                              "falling back so the stream keeps a cursor.";
        return -1;
      }
      BOOST_LOG(info) << "LuminalVGD capture: hardware-cursor plane active "
                         "(blending at encode time).";
    } else {
      BOOST_LOG(info) << "LuminalVGD capture: no cursor section (pre-cursor "
                         "driver); cursor arrives composed into frames.";
    }

    BOOST_LOG(info) << "LuminalVGD capture: consuming frame ring of session 0x"
                    << std::hex << _session_id << std::dec << " (" << _ring_slots
                    << " slots) for " << display_name;
    return 0;
  }

  int display_vgd_vram_t::init_cursor_render(const ::video::config_t &config) {
    // Mirrors display_ddup_vram_t::init()'s cursor block (display_vram.cpp):
    // same shaders, same blend states, same b2 rotation constant buffer.
    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &_cursor_sampler);
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: failed to create cursor sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &_cursor_vs);
    if (status) {
      BOOST_LOG(error) << "LuminalVGD capture: failed to create cursor vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] {rotation_modifier};  // aligned to 16-byte
      auto rotation = make_cbuffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create rotation constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // Normalize the cursor to a 300-nit target on scRGB desktops (same
      // rationale as the DDA path).
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &_cursor_ps);
      if (status) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create cursor (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      float white_multiplier_data[16 / sizeof(float)] {300.0f / 80.f};  // aligned to 16-byte
      auto white_multiplier = make_cbuffer(device.get(), white_multiplier_data);
      if (!white_multiplier) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create white multiplier constant buffer";
        return -1;
      }
      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    } else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &_cursor_ps);
      if (status) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create cursor pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    _blend_alpha = make_blend(device.get(), true, false);
    _blend_invert = make_blend(device.get(), true, true);
    _blend_disable = make_blend(device.get(), false, false);
    if (!_blend_disable || !_blend_alpha || !_blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(_blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &_cursor_sampler);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    _cursor_shape_buf.resize(VGD_CURSOR_SHAPE_BUFFER_SIZE);
    return 0;
  }

  void display_vgd_vram_t::sync_cursor() {
    VgdCursorState state {};
    if (vgd_cursor_state(_cursor, &state) != 0) {
      return;
    }

    if (state.shape_generation != 0 && state.shape_generation != _cursor_shape_generation) {
      VgdCursorShape shape {};
      if (vgd_cursor_shape(_cursor, _cursor_shape_buf.data(), (uint32_t) _cursor_shape_buf.size(), &shape)) {
        // The section carries 32bpp rows at width*4 pitch; describe them in
        // DDA terms so the display_vram.cpp converters apply unchanged.
        // Driver-side XOR emulation means shapes are ALPHA in practice;
        // MASKED is mapped defensively.
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};
        shape_info.Type = shape.kind == VGD_CURSOR_KIND_MASKED ?
                            DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR :
                            DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
        shape_info.Width = shape.width;
        shape_info.Height = shape.height;
        shape_info.Pitch = shape.width * 4;
        shape_info.HotSpot = {(LONG) shape.hotspot_x, (LONG) shape.hotspot_y};

        util::buffer_t<std::uint8_t> img_data {(size_t) shape.width * shape.height * 4};
        std::copy_n(_cursor_shape_buf.data(), img_data.size(), std::begin(img_data));

        auto alpha_img = make_cursor_alpha_image(img_data, shape_info);
        auto xor_img = make_cursor_xor_image(img_data, shape_info);
        if (set_cursor_texture(device.get(), _cursor_alpha, std::move(alpha_img), shape_info) &&
            set_cursor_texture(device.get(), _cursor_xor, std::move(xor_img), shape_info)) {
          _cursor_shape_generation = shape.generation;
        }
      }
      // A failed fetch (driver mid-rewrite) retries on the next snapshot.
    }

    _cursor_alpha.set_pos(state.x, state.y, width_before_rotation, height_before_rotation, display_rotation, state.visible != 0);
    _cursor_xor.set_pos(state.x, state.y, width_before_rotation, height_before_rotation, display_rotation, state.visible != 0);
  }

  void display_vgd_vram_t::blend_cursor_into(img_d3d_t &d3d_img) {
    // Mirrors the DDA blend_cursor lambda (display_vram.cpp).
    device_ctx->VSSetShader(_cursor_vs.get(), nullptr, 0);
    device_ctx->PSSetShader(_cursor_ps.get(), nullptr, 0);
    device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);

    if (_cursor_alpha.texture.get()) {
      device_ctx->OMSetBlendState(_blend_alpha.get(), nullptr, 0xFFFFFFFFu);
      device_ctx->PSSetShaderResources(0, 1, &_cursor_alpha.input_res);
      device_ctx->RSSetViewports(1, &_cursor_alpha.cursor_view);
      device_ctx->Draw(3, 0);
    }

    if (_cursor_xor.texture.get()) {
      device_ctx->OMSetBlendState(_blend_invert.get(), nullptr, 0x00FFFFFFu);
      device_ctx->PSSetShaderResources(0, 1, &_cursor_xor.input_res);
      device_ctx->RSSetViewports(1, &_cursor_xor.cursor_view);
      device_ctx->Draw(3, 0);
    }

    device_ctx->OMSetBlendState(_blend_disable.get(), nullptr, 0xFFFFFFFFu);

    ID3D11RenderTargetView *empty_rt = nullptr;
    device_ctx->OMSetRenderTargets(1, &empty_rt, nullptr);
    device_ctx->RSSetViewports(0, nullptr);
    ID3D11ShaderResourceView *empty_srv = nullptr;
    device_ctx->PSSetShaderResources(0, 1, &empty_srv);
  }

  display_vgd_vram_t::slot_texture_t *display_vgd_vram_t::slot_texture(uint32_t generation, uint32_t slot) {
    if (_texture_generation != generation) {
      _slot_textures.clear();
      _texture_generation = generation;
    }
    if (_slot_textures.empty()) {
      _slot_textures.resize(_ring_slots);
    }
    if (slot >= _slot_textures.size()) {
      return nullptr;
    }

    auto &entry = _slot_textures[slot];
    if (entry.texture) {
      return &entry;
    }

    uint16_t name[96] {};
    vgd_slot_texture_name(_session_id, generation, slot, name);

    device1_t device1;
    HRESULT status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: ID3D11Device1 unavailable [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    texture2d_t texture;
    status = device1->OpenSharedResourceByName(
      reinterpret_cast<LPCWSTR>(name),
      DXGI_SHARED_RESOURCE_READ,
      __uuidof(ID3D11Texture2D),
      (void **) &texture
    );
    if (FAILED(status)) {
      BOOST_LOG(warning) << "LuminalVGD capture: OpenSharedResourceByName failed for slot "
                         << slot << " gen " << generation << " [0x"sv
                         << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    keyed_mutex_t mutex;
    status = texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: slot texture has no keyed mutex [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    entry.texture.reset(texture.release());
    entry.mutex.reset(mutex.release());
    return &entry;
  }

  capture_e display_vgd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ring) {
      return capture_e::error;
    }

    // Cursor-capable driver: frames arrive cursor-free; pull the live
    // shape/position so this frame (or a cursor-only redelivery below)
    // blends the current cursor. Pre-cursor drivers compose it into the
    // frames and `_cursor` stays null.
    if (_cursor) {
      sync_cursor();
    }

    VgdRingStatus status {};
    if (vgd_ring_status(_ring, &status) != 0) {
      return capture_e::reinit;
    }
    if (status.state == kRingStateDead) {
      BOOST_LOG(warning) << "LuminalVGD capture: ring is DEAD; reinitializing.";
      return capture_e::reinit;
    }
    if (status.qpc_frequency != 0 && status.heartbeat_qpc != 0) {
      const auto beats_behind = qpc_time_difference(qpc_counter(), static_cast<int64_t>(status.heartbeat_qpc));
      if (beats_behind > kHeartbeatStale) {
        // The heartbeat stops whenever the OS unassigns the swapchain
        // (game fullscreen/mode transitions) — the worker isn't dead, it's
        // between assignments, and frames resume within seconds. Tearing
        // the capture pipeline down for that turns a sub-second hiccup
        // into a multi-second outage (encoder teardown + re-detection +
        // settle), so wait out a grace window first.
        const auto now = std::chrono::steady_clock::now();
        if (!_heartbeat_stale_since) {
          _heartbeat_stale_since = now;
          BOOST_LOG(info) << "LuminalVGD capture: ring heartbeat stale ("
                          << std::chrono::duration_cast<std::chrono::milliseconds>(beats_behind).count()
                          << " ms); waiting out a possible swapchain reassignment.";
        }
        if (now - *_heartbeat_stale_since > kHeartbeatReinitGrace) {
          BOOST_LOG(warning) << "LuminalVGD capture: ring heartbeat stale for "
                             << std::chrono::duration_cast<std::chrono::seconds>(now - *_heartbeat_stale_since).count()
                             << " s; reinitializing.";
          return capture_e::reinit;
        }
        return capture_e::timeout;
      }
      _heartbeat_stale_since.reset();
    }
    if (status.state == kRingStateRebuilding) {
      // The driver is retiring/recreating textures (mode change, swapchain
      // reassignment). Frames resume under a new generation shortly.
      return capture_e::timeout;
    }

    // A frame NEWER than the one we last delivered sitting unclaimed for a
    // long stretch means the reader side is broken (claim CAS, texture
    // path) — blacklist the ring so the reinit falls back to WGC/DDA
    // instead of leaving the stream black. Cumulative publish counters are
    // deliberately NOT used here: an idle desktop (driver publishing
    // nothing new) is indistinguishable from a stall by counters alone.
    if (status.latest_sequence > _last_sequence) {
      const auto now = std::chrono::steady_clock::now();
      if (!_undelivered_since) {
        _undelivered_since = now;
      } else if (now - *_undelivered_since > kDeliveryStall) {
        BOOST_LOG(error) << "LuminalVGD capture: a newer frame (seq " << status.latest_sequence
                         << " vs delivered " << _last_sequence << ") sat unclaimed for "
                         << std::chrono::duration_cast<std::chrono::seconds>(kDeliveryStall).count()
                         << " s; disabling ring capture for this session.";
        g_broken_session.store(_session_id, std::memory_order_release);
        return capture_e::reinit;
      }
    } else {
      _undelivered_since.reset();
    }

    // Claim the freshest published frame, honoring the caller's timeout.
    // There is no wake event — the section is pure shared memory — so poll at
    // 1 ms granularity; the base capture loop paces us to the client rate and
    // usually calls with timeout 0.
    VgdFrame frame {};
    bool claimed = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      if (vgd_ring_claim(_ring, &frame)) {
        if (frame.sequence > _last_sequence) {
          claimed = true;
          break;
        }
        // At or below the last delivered sequence: either the frame we
        // already delivered, or an OLDER still-published leftover that
        // became the "freshest" after we released the newest slot —
        // delivering it would send frames out of order. Release and wait
        // for something genuinely new.
        vgd_ring_release(_ring, frame.index);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!claimed) {
      // Nothing published — but with the cursor on its own plane, cursor
      // motion over an idle desktop produces no frames at all. Redeliver
      // the last frame with a fresh blend when the cursor state moved past
      // what the client last saw.
      if (_cursor && _last_frame && capture_format != DXGI_FORMAT_UNKNOWN) {
        const bool cursor_changed =
          (int32_t) _cursor_alpha.topleft_x != _delivered_cursor_x ||
          (int32_t) _cursor_alpha.topleft_y != _delivered_cursor_y ||
          _cursor_alpha.visible != _delivered_cursor_visible ||
          _cursor_shape_generation != _delivered_cursor_generation;
        if (cursor_changed) {
          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }
          auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
          // Recoverable conditions must be `reinit`, never `error`: `error`
          // kills the capture thread and with it EVERY session, while this
          // path runs continuously whenever the cursor moves over an idle
          // desktop. A mutex timeout / DEVICE_REMOVED here (the display can
          // vanish mid-stream) is exactly what reinit + fallback exist for.
          if (complete_img(d3d_img.get(), false)) {
            return capture_e::reinit;
          }
          const HRESULT dst_acquire = seh_acquire_sync(d3d_img->capture_mutex.get(), 0, 3000);
          if (dst_acquire != S_OK && dst_acquire != static_cast<HRESULT>(WAIT_ABANDONED)) {
            BOOST_LOG(warning) << "LuminalVGD capture: pooled texture mutex acquire failed [0x"sv << util::hex(dst_acquire).to_string_view() << "]; reinitializing."sv;
            return capture_e::reinit;
          }
          device_ctx->CopyResource(d3d_img->capture_texture.get(), _last_frame.get());
          if (cursor_visible && (_cursor_alpha.visible || _cursor_xor.visible)) {
            blend_cursor_into(*d3d_img);
          }
          seh_release_sync(d3d_img->capture_mutex.get(), 0);

          _delivered_cursor_x = (int32_t) _cursor_alpha.topleft_x;
          _delivered_cursor_y = (int32_t) _cursor_alpha.topleft_y;
          _delivered_cursor_visible = _cursor_alpha.visible;
          _delivered_cursor_generation = _cursor_shape_generation;

          const auto now = std::chrono::steady_clock::now();
          d3d_img->blank = false;
          d3d_img->format = capture_format;
          d3d_img->pixel_pitch = get_pixel_pitch();
          d3d_img->row_pitch = d3d_img->pixel_pitch * d3d_img->width;
          d3d_img->data = (std::uint8_t *) d3d_img->capture_texture.get();
          img->frame_timestamp = now;
          img->host_processing_timestamp = now;
          img_out = img;
          return capture_e::ok;
        }
      }
      return capture_e::timeout;
    }

    auto claim_guard = util::fail_guard([&]() {
      vgd_ring_release(_ring, frame.index);
    });

    auto *slot = slot_texture(frame.generation, frame.index);
    if (!slot) {
      // Generation likely moved on between claim and open; retry next call —
      // but a persistent failure means the shared textures are unreachable.
      if (++_open_failures >= kMaxOpenFailures) {
        BOOST_LOG(error) << "LuminalVGD capture: slot textures unreachable after "
                         << _open_failures << " attempts; disabling ring capture for this session.";
        g_broken_session.store(_session_id, std::memory_order_release);
        return capture_e::reinit;
      }
      return capture_e::timeout;
    }
    _open_failures = 0;

    const HRESULT acquire = seh_acquire_sync(slot->mutex.get(), kVgdMutexKeyHost, kVgdMutexTimeoutMs);
    if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      return capture_e::timeout;
    }
    if (acquire != S_OK && acquire != static_cast<HRESULT>(WAIT_ABANDONED)) {
      BOOST_LOG(warning) << "LuminalVGD capture: slot mutex acquire failed [0x"sv << util::hex(acquire).to_string_view() << ']';
      return capture_e::reinit;
    }
    auto mutex_guard = util::fail_guard([&]() {
      seh_release_sync(slot->mutex.get(), kVgdMutexKeyHost);
    });

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp;
    if (frame.present_qpc != 0) {
      frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), static_cast<int64_t>(frame.present_qpc));
    }

    D3D11_TEXTURE2D_DESC desc;
    slot->texture->GetDesc(&desc);

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }
    if (desc.Width != static_cast<UINT>(width_before_rotation) || desc.Height != static_cast<UINT>(height_before_rotation)) {
      BOOST_LOG(info) << "Capture size changed ["sv << width_before_rotation << 'x' << height_before_rotation
                      << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format)
                      << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }
    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

    // Lazily create the pooled GPU texture + encoder handle (idempotent).
    // Recoverable failures are `reinit`, not `error`: `error` tears down
    // the capture thread and every session, while a mutex timeout or
    // DEVICE_REMOVED (the virtual display can vanish mid-stream) is what
    // reinit + WGC/DDA fallback exist for.
    if (complete_img(d3d_img.get(), false)) {
      return capture_e::reinit;
    }

    // Pooled textures follow the capture<->encoder key-0 convention.
    const HRESULT dst_acquire = seh_acquire_sync(d3d_img->capture_mutex.get(), 0, 3000);
    if (dst_acquire != S_OK && dst_acquire != static_cast<HRESULT>(WAIT_ABANDONED)) {
      BOOST_LOG(warning) << "LuminalVGD capture: pooled texture mutex acquire failed [0x"sv << util::hex(dst_acquire).to_string_view() << "]; reinitializing."sv;
      return capture_e::reinit;
    }

    device_ctx->CopyResource(d3d_img->capture_texture.get(), slot->texture.get());

    if (_cursor) {
      // Cursor-free copy of this frame (the driver owns the cursor plane,
      // so slot pixels never contain it) for idle cursor-only redelivery.
      // Reads the slot texture, so this stays inside the slot-mutex hold.
      if (!_last_frame) {
        D3D11_TEXTURE2D_DESC copy_desc = desc;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = 0;
        copy_desc.CPUAccessFlags = 0;
        copy_desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&copy_desc, nullptr, &_last_frame))) {
          BOOST_LOG(warning) << "LuminalVGD capture: last-frame copy unavailable; "
                                "cursor motion over an idle desktop will not refresh.";
        }
      }
      if (_last_frame) {
        device_ctx->CopyResource(_last_frame.get(), slot->texture.get());
      }
      if (cursor_visible && (_cursor_alpha.visible || _cursor_xor.visible)) {
        blend_cursor_into(*d3d_img);
      }
      _delivered_cursor_x = (int32_t) _cursor_alpha.topleft_x;
      _delivered_cursor_y = (int32_t) _cursor_alpha.topleft_y;
      _delivered_cursor_visible = _cursor_alpha.visible;
      _delivered_cursor_generation = _cursor_shape_generation;
    }

    seh_release_sync(d3d_img->capture_mutex.get(), 0);

    // Copy done: hand the slot back before the encoder ever sees the image.
    mutex_guard.disable();
    seh_release_sync(slot->mutex.get(), kVgdMutexKeyHost);
    claim_guard.disable();
    vgd_ring_release(_ring, frame.index);
    _last_sequence = frame.sequence;
    _undelivered_since.reset();

    d3d_img->blank = false;
    d3d_img->format = capture_format;
    d3d_img->pixel_pitch = get_pixel_pitch();
    d3d_img->row_pitch = d3d_img->pixel_pitch * d3d_img->width;
    d3d_img->data = (std::uint8_t *) d3d_img->capture_texture.get();
    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    img_out = img;

    return capture_e::ok;
  }

  capture_e display_vgd_vram_t::release_snapshot() {
    // Claims and mutexes are released within snapshot(); nothing is held
    // across the push to the encoder.
    return capture_e::ok;
  }

  int display_vgd_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

}  // namespace platf::dxgi
