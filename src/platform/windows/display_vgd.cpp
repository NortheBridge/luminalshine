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
    /// frames flow; treat several missed beats as a dead worker.
    constexpr auto kHeartbeatStale = std::chrono::milliseconds(2000);

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

  }  // namespace

  display_vgd_vram_t::~display_vgd_vram_t() {
    _slot_textures.clear();
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

    BOOST_LOG(info) << "LuminalVGD capture: consuming frame ring of session 0x"
                    << std::hex << _session_id << std::dec << " (" << _ring_slots
                    << " slots) for " << display_name;
    return 0;
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
    // The IddCx path composes the cursor into the frame (no hardware-cursor
    // DDIs yet), so cursor_visible needs no handling here.
    (void) cursor_visible;

    if (!_ring) {
      return capture_e::error;
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
        BOOST_LOG(warning) << "LuminalVGD capture: ring heartbeat stale ("
                           << std::chrono::duration_cast<std::chrono::milliseconds>(beats_behind).count()
                           << " ms); reinitializing.";
        return capture_e::reinit;
      }
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
        if (frame.sequence != _last_sequence) {
          claimed = true;
          break;
        }
        // Freshest published frame is the one we already delivered.
        vgd_ring_release(_ring, frame.index);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!claimed) {
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
    if (complete_img(d3d_img.get(), false)) {
      return capture_e::error;
    }

    // Pooled textures follow the capture<->encoder key-0 convention.
    const HRESULT dst_acquire = seh_acquire_sync(d3d_img->capture_mutex.get(), 0, 3000);
    if (dst_acquire != S_OK && dst_acquire != static_cast<HRESULT>(WAIT_ABANDONED)) {
      BOOST_LOG(error) << "LuminalVGD capture: pooled texture mutex acquire failed [0x"sv << util::hex(dst_acquire).to_string_view() << ']';
      return capture_e::error;
    }

    device_ctx->CopyResource(d3d_img->capture_texture.get(), slot->texture.get());

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
