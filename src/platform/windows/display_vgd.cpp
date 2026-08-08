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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

// platform includes
#include <winsock2.h>
#include <d3d12.h>

// local includes
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/tdr_state.h"
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
    // Encoder mailbox textures are host-owned. A free image returned by the
    // pool should therefore be immediately acquirable; waiting seconds here
    // would recouple the display producer to a wedged encoder. Drop/reinit on
    // a short bound instead and leave every VGD ring claim released.
    constexpr uint32_t kEncoderMailboxMutexTimeoutMs = 100;

    /// Circuit breaker: a session whose ring consistently fails to deliver
    /// (texture opens failing, or frames publishing that we can never claim)
    /// is remembered here so the factory's next attempt refuses and the
    /// stream falls back to WGC/DDA instead of staying black.
    ///
    /// Deliberately permanent for the life of the process: a ring that cannot
    /// deliver does not heal, and re-arming it on a timer would reintroduce a
    /// capture teardown every time the timer expired. What keeps that safe is
    /// that only genuine READER breakage reaches it — a RECOVERABLE GPU outage
    /// (ring rebuilding, our device removed) resolves to reinit, never here.
    ///
    /// The one non-reader-breakage entry is an exhausted AGGREGATE recovery
    /// budget: a ring that has been REBUILDING past the point where even the
    /// driver's own budget should have given up is not coming back, and a plain
    /// reinit there would just build a new reader onto the same stuck ring
    /// forever. Latching hands the session to WGC/DDA, which is a terminal
    /// state rather than a cycle — and still a streaming session.
    std::atomic<uint64_t> g_broken_session {0};

    /// A pre-capture admission timeout is not proof of reader corruption.
    /// Build 22 can be REBUILDING while Windows transiently refuses
    /// D3D11CreateDevice during topology activation, then publish normally
    /// after the driver's bounded retry. Give that distinct condition one
    /// final, stable-ring qualification when the capture factory starts.
    std::atomic<uint64_t> g_pre_admission_session {0};
    std::atomic<uint64_t> g_pre_admission_attempted_session {0};

    /// Consecutive slot-texture open failures before giving up on the ring.
    constexpr int kMaxOpenFailures = 20;

    /// How often to report an in-progress ring wait. The ladder logs on
    /// transitions only (a recovering ring is sampled for minutes), which
    /// leaves nothing in the log between "started waiting" and whatever ends
    /// the wait — the two field outcomes that most need telling apart.
    constexpr auto kRingWaitProgressLogInterval = std::chrono::seconds(15);

    /// The driver build that introduced the TDR duck-in-place this whole
    /// ladder is built around: the monitor stays ARRIVED, the ring goes
    /// REBUILDING, and a device-independent heartbeat keeps ticking for the
    /// whole outage. Every older build departs the monitor instead and its
    /// heartbeat simply stops — indistinguishable from a live one for the
    /// 2 s staleness threshold, so those builds must never reach the
    /// recovering branch at all (see trust_rebuilding_heartbeat).
    constexpr uint32_t kFirstDuckInPlaceDriverBuild = 16;

    /// Aggregate recovery clocks, keyed by ring session and kept ACROSS reader
    /// instances.
    ///
    /// The liveness ladder lives on the reader, and every capture reinit
    /// destroys and rebuilds the reader — so a per-object recovery budget
    /// restarts on each reinit and the wait that actually matters (rebuild,
    /// spend the budget, reinit, spend it again, forever) is unbounded. These
    /// survive the reader so one outage gets one budget.
    std::mutex g_recovery_clock_mutex;
    std::map<uint64_t, vgd_recovery_clock_t> g_recovery_clocks;

    /// How often a running clock is refreshed in the store above.
    ///
    /// The stored `last_seen` is what the next reader checks before adopting
    /// (vgd_ring_liveness_config_t::recovery_clock_handoff_grace), so it has to
    /// keep up with a live outage — but this runs once per sampled frame, so it
    /// must not take a process-wide lock at that rate. Once a second is two
    /// orders of magnitude inside the 30 s grace and ~1/100th of the sampling
    /// rate.
    constexpr auto kRecoveryClockRepublishInterval = std::chrono::seconds(1);

    /// Cap on the table above. Bounded by construction rather than by age: a
    /// time-based sweep could drop precisely the expired clock a rebuilt reader
    /// is about to adopt, handing it a fresh budget and restoring the very
    /// unboundedness this exists to remove.
    constexpr size_t kMaxTrackedRecoveryClocks = 8;

    /// Persist (or clear) the recovery clock of `session_id`. An empty `clock`
    /// means the ladder reports no outage in progress, which is what lets a
    /// later, unrelated outage start a full budget of its own — and what stops
    /// a spent clock from lying in wait here to be adopted by a healthy ring.
    void store_recovery_clock(uint64_t session_id, std::optional<vgd_recovery_clock_t> clock) {
      std::lock_guard lk(g_recovery_clock_mutex);
      if (!clock) {
        g_recovery_clocks.erase(session_id);
        return;
      }
      g_recovery_clocks[session_id] = *clock;
      while (g_recovery_clocks.size() > kMaxTrackedRecoveryClocks) {
        // Evict the oldest clock that is not the one just stored.
        auto oldest = g_recovery_clocks.end();
        for (auto it = g_recovery_clocks.begin(); it != g_recovery_clocks.end(); ++it) {
          if (it->first == session_id) {
            continue;
          }
          if (oldest == g_recovery_clocks.end() || it->second.started < oldest->second.started) {
            oldest = it;
          }
        }
        if (oldest == g_recovery_clocks.end()) {
          break;
        }
        g_recovery_clocks.erase(oldest);
      }
    }

    std::optional<vgd_recovery_clock_t> load_recovery_clock(uint64_t session_id) {
      std::lock_guard lk(g_recovery_clock_mutex);
      auto it = g_recovery_clocks.find(session_id);
      if (it == g_recovery_clocks.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    /// One line per liveness-ladder transition (never per snapshot — a
    /// recovering ring is sampled ~100x/s for minutes).
    void log_ring_decision(const vgd_ring_decision_t &decision, const vgd_ring_sample_t &sample) {
      const auto heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sample.heartbeat_age).count();
      switch (decision.reason) {
        case vgd_ring_reason_e::ring_dead:
          BOOST_LOG(warning) << "LuminalVGD capture: ring is DEAD; reinitializing.";
          break;
        case vgd_ring_reason_e::recovering:
          BOOST_LOG(info) << "LuminalVGD capture: ring is REBUILDING with a live heartbeat ("
                          << heartbeat_ms << " ms behind) — the driver is riding out a GPU "
                                             "outage with the monitor still arrived. Holding the "
                                             "capture instead of tearing it down.";
          break;
        case vgd_ring_reason_e::rebuilding:
          BOOST_LOG(info) << "LuminalVGD capture: ring is REBUILDING (no usable heartbeat); waiting.";
          break;
        case vgd_ring_reason_e::recovery_budget_expired:
          BOOST_LOG(warning) << "LuminalVGD capture: ring has been REBUILDING past the host's "
                                "recovery budget; reinitializing.";
          break;
        case vgd_ring_reason_e::heartbeat_stale:
          BOOST_LOG(info) << "LuminalVGD capture: ring heartbeat stale (" << heartbeat_ms
                          << " ms); waiting out a possible swapchain reassignment.";
          break;
        case vgd_ring_reason_e::heartbeat_lost:
          BOOST_LOG(warning) << "LuminalVGD capture: ring heartbeat stale past the grace window ("
                             << heartbeat_ms << " ms behind); reinitializing.";
          break;
        case vgd_ring_reason_e::gpu_reset:
          BOOST_LOG(warning) << "LuminalVGD capture: reinitializing after a GPU-reset event.";
          break;
        case vgd_ring_reason_e::delivery_stall:
          BOOST_LOG(error) << "LuminalVGD capture: a newer frame (seq " << sample.latest_sequence
                           << " vs delivered " << sample.delivered_sequence
                           << ") sat unclaimed; disabling ring capture for this session.";
          break;
        case vgd_ring_reason_e::healthy:
          break;
      }
    }

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

  void mark_vgd_ring_broken(uint64_t session_id) {
    if (session_id != 0) {
      const auto prior_session = g_pre_admission_session.exchange(session_id, std::memory_order_acq_rel);
      if (prior_session != session_id) {
        g_pre_admission_attempted_session.store(0, std::memory_order_release);
      }
      BOOST_LOG(warning) << "LuminalVGD capture: session 0x" << std::hex << session_id
                         << std::dec << " failed first-frame admission; one bounded requalification remains.";
    }
  }

  display_vgd_vram_t::~display_vgd_vram_t() {
    _slot_textures.clear();
    if (_consumer_fence_event) {
      CloseHandle(_consumer_fence_event);
      _consumer_fence_event = nullptr;
    }
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
    if (g_pre_admission_session.load(std::memory_order_acquire) == target->session_id) {
      uint64_t expected = 0;
      if (!g_pre_admission_attempted_session.compare_exchange_strong(
            expected,
            target->session_id,
            std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        BOOST_LOG(warning) << "LuminalVGD capture: pre-admission requalification already spent for session 0x"
                           << std::hex << target->session_id << std::dec << "; using fallback capture.";
        return -1;
      }

      const auto token = VDISPLAY::vgd::begin_planned_modeset();
      if (!token || token->session_id != target->session_id ||
          !VDISPLAY::vgd::wait_for_planned_modeset(*token, std::chrono::seconds(3))) {
        BOOST_LOG(warning) << "LuminalVGD capture: session 0x" << std::hex << target->session_id
                           << std::dec << " did not publish a stable frame during final qualification; using fallback capture.";
        return -1;
      } else {
        BOOST_LOG(info) << "LuminalVGD capture: session 0x" << std::hex << target->session_id
                        << std::dec << " published a stable frame during final qualification; direct capture restored.";
      }

      uint64_t admitted_session = target->session_id;
      g_pre_admission_session.compare_exchange_strong(
        admitted_session,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire
      );
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
    if (VDISPLAY::vgd::has_worker_ring_target()) {
      // Holding the ring open is what arms surface publication on supported
      // IddCx stacks, so the first status reads after open routinely show the
      // provisional REBUILDING/sequence-zero contract (or another transient of
      // the post-modeset rebuild, e.g. ACTIVE before the first surface). Wait
      // a bounded interval for the committed ACTIVE contract instead of
      // abandoning Direct-to-Encode on the first observation; isolated WGC
      // remains the fallback if the ring never commits.
      VgdRingStatus opened_status {};
      std::int32_t ring_status = -1;
      auto binding = vgd_worker_ring_binding_e::reject;
      bool heartbeat_fresh = false;
      // One second: long enough for a ring that is actively committing its
      // post-modeset generation, short enough that a producer that has not
      // started (observed: provisional for 3+ s after the exclusive APPLY)
      // pivots to WGC before eating the connection budget. A later modeset
      // re-enters init and can still re-bind Direct-to-Encode.
      const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      for (;;) {
        ring_status = vgd_ring_status(ring, &opened_status);
        if (ring_status != 0 || opened_status.state == vgd_ring_state::dead) {
          binding = vgd_worker_ring_binding_e::reject;
          break;  // The ring is gone or dead; waiting cannot revive it.
        }
        if (target->generation != 0 && opened_status.generation != 0 &&
            opened_status.generation != target->generation) {
          // A Windows modeset retired the previously bound generation while
          // the authenticated session identity stayed fixed. Re-defer and let
          // this admission bind the successor generation instead of
          // permanently degrading the session to WGC.
          BOOST_LOG(info) << "LuminalVGD capture: session 0x" << std::hex << target->session_id
                          << std::dec << " generation " << target->generation
                          << " was retired by a modeset; re-deferring to bind generation "
                          << opened_status.generation << '.';
          target->generation = 0;
        }
        binding = classify_worker_ring_binding(
          ring_status,
          opened_status.state,
          opened_status.generation,
          opened_status.latest_sequence,
          opened_status.transport_flags,
          target->generation,
          target->transport_flags
        );
        LARGE_INTEGER now_qpc {};
        const bool qpc_read = QueryPerformanceCounter(&now_qpc) != FALSE;
        heartbeat_fresh = qpc_read && now_qpc.QuadPart >= 0 &&
          opened_status.qpc_frequency != 0 && opened_status.heartbeat_qpc != 0 &&
          static_cast<uint64_t>(now_qpc.QuadPart) >= opened_status.heartbeat_qpc &&
          static_cast<uint64_t>(now_qpc.QuadPart) - opened_status.heartbeat_qpc <=
            opened_status.qpc_frequency * 2ULL;
        if (binding == vgd_worker_ring_binding_e::ready && heartbeat_fresh) break;
        if (binding == vgd_worker_ring_binding_e::provisional) {
          // A producer that has never published stays provisional for many
          // seconds on the current driver; every second spent here charges
          // the client's first-video budget. Pivot to WGC immediately — the
          // mid-session re-defer path upgrades to Direct-to-Encode as soon
          // as the ring commits a generation.
          break;
        }
        if (std::chrono::steady_clock::now() >= admission_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (binding != vgd_worker_ring_binding_e::ready || !heartbeat_fresh) {
        BOOST_LOG(warning) << "Video worker: transferred LuminalVGD ring did not become capture-ready within its bounded admission; switching to isolated WGC"
                         << " (status=" << ring_status
                         << ", expected_generation=" << target->generation
                         << ", observed_generation=" << opened_status.generation
                         << ", state=" << opened_status.state
                         << ", sequence=" << opened_status.latest_sequence
                         << ", expected_transport=0x" << std::hex << target->transport_flags
                         << ", observed_transport=0x" << opened_status.transport_flags
                         << std::dec << ", heartbeat_fresh=" << heartbeat_fresh << ").";
        return -1;
      }
      if (target->generation == 0) {
        target->generation = opened_status.generation;
        VDISPLAY::vgd::set_worker_ring_target(*target, display_name);
        BOOST_LOG(info) << "Video worker: bound direct LuminalVGD session 0x" << std::hex
                        << target->session_id << std::dec << " to post-modeset generation "
                        << target->generation << " (state=" << opened_status.state << ").";
      }
    }

    // Do the comparatively expensive DXGI/display initialization only after
    // a transferred ring has proven it can deliver. A provisional ring used
    // to spend roughly two seconds here before selecting WGC, consuming the
    // Xbox/webOS connection budget for no benefit.
    if (display_base_t::init(config, display_name, true /* skip_dd_test: ring capture doesn't use Desktop Duplication */)) {
      return -1;
    }

    // The display initialization above can overlap a Windows modeset. Recheck
    // the now-fixed generation before advertising direct capture to NVENC.
    if (VDISPLAY::vgd::has_worker_ring_target()) {
      VgdRingStatus confirmed {};
      const auto status = vgd_ring_status(ring, &confirmed);
      const auto binding = classify_worker_ring_binding(
        status,
        confirmed.state,
        confirmed.generation,
        confirmed.latest_sequence,
        confirmed.transport_flags,
        target->generation,
        target->transport_flags
      );
      if (binding != vgd_worker_ring_binding_e::ready) {
        BOOST_LOG(warning) << "Video worker: LuminalVGD ring changed during display initialization; switching to isolated WGC"
                           << " (status=" << status << ", generation=" << confirmed.generation
                           << ", state=" << confirmed.state << ", sequence=" << confirmed.latest_sequence
                           << ", transport=0x" << std::hex << confirmed.transport_flags << std::dec << ").";
        return -1;
      }
    }
    _session_id = target->session_id;
    _ring_slots = target->ring_slots;
    _fence_transport = (target->transport_flags & VGD_CREATE_D3D12_FENCE_TRANSPORT) != 0;
    _fence_required = (target->transport_flags & VGD_CREATE_FENCE_TRANSPORT_REQUIRED) != 0;
    _transport_generation = target->generation;
    _display_name = display_name;
    _tdr_marks_at_open = tdr::event_count();
    _first_frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    _first_frame_timeout_reported = false;
    capture_format = DXGI_FORMAT_UNKNOWN;  // latched from the first claimed frame

    BOOST_LOG(info) << "LuminalVGD capture: "
                    << (_fence_transport ? "shared timeline-fence transport active"
                                         : "legacy keyed-mutex transport active") << '.';

    // Only vouch for the "REBUILDING + live heartbeat = recovering" reading on
    // a driver that actually behaves that way. Older builds depart the monitor
    // on a TDR and their heartbeat stops, which for the 2 s staleness threshold
    // reads exactly like a beating one — so trusting it there would buy them up
    // to 2 s of deferral they never had before. An unknown build (no handshake)
    // counts as old: this is the conservative direction, and it keeps every
    // pre-build-16 driver bit-identical to pre-change behaviour.
    const auto driver_build = VDISPLAY::vgd::driver_build();
    vgd_ring_liveness_config_t liveness_config;
    liveness_config.trust_rebuilding_heartbeat =
      driver_build && *driver_build >= kFirstDuckInPlaceDriverBuild;
    // Build 20 explicitly deletes a failed IDDCX_SWAPCHAIN and asks IddCx for
    // a replacement. A healthy reassignment finishes in seconds; if it has
    // not returned after ten seconds, keeping a frozen direct-ring reader for
    // the historical eleven-minute TDR budget only strands the client. Latch
    // this session away from direct capture so the normal display factory
    // reinit selects WGC/DDA against the same still-arrived VGD monitor.
    if (driver_build && *driver_build >= 20) {
      liveness_config.recovery_budget = std::chrono::seconds(10);
    }
    _liveness = vgd_ring_liveness_t {liveness_config};
    if (!liveness_config.trust_rebuilding_heartbeat) {
      BOOST_LOG(info) << "LuminalVGD capture: driver build "
                      << (driver_build ? std::to_string(*driver_build) : std::string("unknown"))
                      << " predates the TDR duck-in-place contract (build "
                      << kFirstDuckInPlaceDriverBuild
                      << "); a REBUILDING ring will be treated as it always was.";
    }

    // Adopt any recovery clock still running for this session: the reader this
    // one replaces may have been rebuilt BY the outage, and the budget has to
    // cover the outage rather than restart with each reader. The ladder itself
    // decides whether the stored clock is still CURRENT — a leftover from an
    // outage nobody has been watching is not a handoff, and charging this ring
    // for it would expire a budget it never spent (see adopt_recovery_start).
    _published_recovery_start = load_recovery_clock(_session_id);
    _liveness.adopt_recovery_start(_published_recovery_start, std::chrono::steady_clock::now());

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
    if (VDISPLAY::vgd::has_worker_ring_target()) {
      BOOST_LOG(info) << "Video worker: VGD_RING_OPENED session=0x" << std::hex << _session_id
                      << std::dec << " slots=" << _ring_slots << " display=" << display_name;
    }
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
    // Clear BEFORE any exit path, so the flag always describes THIS call.
    // Clearing it further down left the out-of-range return below reporting a
    // previous call's device-removed verdict, and the caller reads it only when
    // this returns null: a stale `true` turns every subsequent failure into
    // "reinit against a fresh device" forever, so the open-failure counter never
    // reaches its limit and the bounded fall back to WGC/DDA never happens.
    _open_failed_device_removed = false;

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
    if (_fence_transport) {
      vgd_slot_texture_d3d12_name(_session_id, generation, slot, name);
    } else {
      vgd_slot_texture_name(_session_id, generation, slot, name);
    }

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
      // Distinguish "the ring's textures are unreachable" (a broken reader —
      // worth blacklisting the session) from "OUR OWN D3D device was removed"
      // (a GPU reset — the ring is fine and a reinit against a fresh device
      // fixes it). Without this, a device removed by the same TDR the driver
      // is riding out fails every open forever and permanently disables ring
      // capture for a session that was only ever recovering.
      const HRESULT removed_reason = device->GetDeviceRemovedReason();
      _open_failed_device_removed = FAILED(removed_reason) ||
                                    status == DXGI_ERROR_DEVICE_REMOVED ||
                                    status == DXGI_ERROR_DEVICE_RESET;
      BOOST_LOG(warning) << "LuminalVGD capture: OpenSharedResourceByName failed for slot "
                         << slot << " gen " << generation << " [0x"sv
                         << util::hex(status).to_string_view() << "], device removed reason [0x"sv
                         << util::hex(removed_reason).to_string_view() << ']';
      return nullptr;
    }

    keyed_mutex_t mutex;
    if (!_fence_transport) {
      status = texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "LuminalVGD capture: slot texture has no keyed mutex [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }
    }

    entry.texture.reset(texture.release());
    entry.mutex.reset(mutex.release());
    return &entry;
  }

  bool display_vgd_vram_t::prepare_fence_transport(uint32_t generation) {
    if (!_fence_transport) {
      return true;
    }
    if (_producer_fence && _fence_generation == generation) {
      return true;
    }

    _producer_fence.reset();
    _fence_generation = 0;

    device5_t device5;
    HRESULT status = device->QueryInterface(__uuidof(ID3D11Device5), (void **) &device5);
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: ID3D11Device5 unavailable for fence transport: "
                       << util::log_hex(status);
      return false;
    }
    if (!_device_ctx4) {
      status = device_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **) &_device_ctx4);
      if (FAILED(status)) {
        BOOST_LOG(error) << "LuminalVGD capture: ID3D11DeviceContext4 unavailable for fence transport: "
                         << util::log_hex(status);
        return false;
      }
    }
    if (!_consumer_fence) {
      status = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **) &_consumer_fence);
      if (FAILED(status)) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create consumer completion fence: "
                         << util::log_hex(status);
        return false;
      }
      _consumer_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!_consumer_fence_event) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to create consumer fence event (Win32 "
                         << GetLastError() << ").";
        _consumer_fence.reset();
        return false;
      }
    }

    uint16_t fence_name[96] {};
    vgd_ring_fence_name(_session_id, generation, fence_name);

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12Device1> d3d12_device1;
    status = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(status)) status = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(status)) status = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device));
    if (SUCCEEDED(status)) status = d3d12_device.As(&d3d12_device1);

    HANDLE shared_handle = nullptr;
    if (SUCCEEDED(status)) {
      status = d3d12_device1->OpenSharedHandleByName(
        reinterpret_cast<LPCWSTR>(fence_name), GENERIC_ALL, &shared_handle);
    }
    if (SUCCEEDED(status)) {
      status = device5->OpenSharedFence(shared_handle, __uuidof(ID3D11Fence), (void **) &_producer_fence);
    }
    if (shared_handle) {
      CloseHandle(shared_handle);
    }
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: failed to open generation " << generation
                       << " producer fence: " << util::log_hex(status);
      _producer_fence.reset();
      return false;
    }

    _fence_generation = generation;
    BOOST_LOG(info) << "LuminalVGD capture: opened producer timeline fence for generation "
                    << generation << '.';
    return true;
  }

  bool display_vgd_vram_t::refresh_transport() {
    VgdRingStatus status {};
    if (vgd_ring_status(_ring, &status) != 0) {
      return false;
    }
    const bool fence = (status.transport_flags & VGD_CREATE_D3D12_FENCE_TRANSPORT) != 0;
    const bool required = (status.transport_flags & VGD_CREATE_FENCE_TRANSPORT_REQUIRED) != 0;
    if ((_fence_required && (!fence || !required)) ||
        (status.generation == _transport_generation && fence != _fence_transport)) {
      BOOST_LOG(error) << "LuminalVGD capture: synchronization contract changed inside ring generation "
                       << status.generation << "; rejecting the generation.";
      return false;
    }
    if (fence == _fence_transport) {
      _transport_generation = status.generation;
      return true;
    }

    BOOST_LOG(info) << "LuminalVGD capture: driver selected "
                    << (fence ? "shared timeline-fence" : "legacy keyed-mutex")
                    << " transport for ring generation " << status.generation << '.';
    _fence_transport = fence;
    _fence_required = required;
    _transport_generation = status.generation;
    _slot_textures.clear();
    _texture_generation = 0;
    _producer_fence.reset();
    _fence_generation = 0;
    return true;
  }

  bool display_vgd_vram_t::finish_slot_copy() {
    const uint64_t value = ++_consumer_fence_value;
    HRESULT status = _device_ctx4->Signal(_consumer_fence.get(), value);
    if (SUCCEEDED(status)) {
      status = _consumer_fence->SetEventOnCompletion(value, _consumer_fence_event);
    }
    if (FAILED(status)) {
      BOOST_LOG(error) << "LuminalVGD capture: failed to arm consumer-copy fence: "
                       << util::log_hex(status);
      return false;
    }
    const DWORD wait = WaitForSingleObject(_consumer_fence_event, 500);
    if (wait != WAIT_OBJECT_0) {
      const HRESULT removed = device->GetDeviceRemovedReason();
      BOOST_LOG(error) << "LuminalVGD capture: GPU did not finish reading the claimed slot within 500 ms"
                          " (wait=" << wait << ", device=" << util::log_hex(removed) << ").";
      return false;
    }
    return true;
  }

  void display_vgd_vram_t::release_shared_allocations() {
    _slot_textures.clear();
    _last_frame.reset();
  }

  void display_vgd_vram_t::wait_out_ring(std::chrono::milliseconds timeout, const vgd_ring_sample_t &waiting_on) {
    const auto started = std::chrono::steady_clock::now();
    while (true) {
      const auto slice = vgd_wait_slice(timeout, std::chrono::steady_clock::now() - started);
      if (slice <= std::chrono::nanoseconds::zero()) {
        return;
      }
      std::this_thread::sleep_for(slice);

      // A pure shared-memory header read — no device work, which is the whole
      // point of the wait verdict. Watching the published sequence as well as
      // the state matters for the heartbeat-stale wait, where the ring stays
      // ACTIVE throughout: a driver that resumes publishing mid-wait (the
      // swapchain-reassignment case, which is over in seconds) is then picked
      // up within a slice instead of at the end of the caller's timeout.
      VgdRingStatus status {};
      if (vgd_ring_status(_ring, &status) != 0 ||
          status.state != waiting_on.state ||
          status.latest_sequence != waiting_on.latest_sequence) {
        return;
      }
    }
  }

  void display_vgd_vram_t::publish_recovery_clock() {
    // Only on a change: this runs per captured frame, and the common case (no
    // outage, nothing to publish) should not touch a process-wide lock at all.
    const auto clock = _liveness.recovery_start();
    if (!clock) {
      if (_published_recovery_start) {
        // The outage ended. Clearing the store is what stops a spent clock from
        // being adopted by a later reader, so it is not an optimisation.
        _published_recovery_start.reset();
        store_recovery_clock(_session_id, std::nullopt);
      }
      return;
    }
    if (_published_recovery_start &&
        _published_recovery_start->started == clock->started &&
        clock->last_seen - _published_recovery_start->last_seen < kRecoveryClockRepublishInterval) {
      // Same outage, and the stored confirmation is still recent enough for the
      // next reader to trust it.
      return;
    }
    _published_recovery_start = clock;
    store_recovery_clock(_session_id, clock);
  }

  bool display_vgd_vram_t::read_ring_sample(vgd_ring_sample_t &sample, bool tdr_edge) {
    VgdRingStatus status {};
    if (vgd_ring_status(_ring, &status) != 0) {
      return false;
    }

    sample.state = status.state;
    sample.generation = status.generation;
    sample.latest_sequence = status.latest_sequence;
    sample.delivered_sequence = _last_sequence;
    sample.tdr_edge = tdr_edge;
    sample.heartbeat_valid = status.qpc_frequency != 0 && status.heartbeat_qpc != 0;
    if (sample.heartbeat_valid) {
      sample.heartbeat_age = qpc_time_difference(qpc_counter(), static_cast<int64_t>(status.heartbeat_qpc));
    }
    return true;
  }

  bool display_vgd_vram_t::is_recovering() {
    if (!_ring) {
      return false;
    }
    vgd_ring_sample_t sample {};
    if (!read_ring_sample(sample, false)) {
      return false;
    }
    // Recovery is a transition away from a previously publishing source. A
    // brand-new REBUILDING ring with no sequence has never supplied a frame;
    // treating it as a 15-minute GPU outage bypasses startup fallback and
    // strands every client behind a blank placeholder.
    if (_last_sequence == 0 && sample.latest_sequence == 0) {
      return false;
    }
    const bool recovering = _liveness.recovering(sample, std::chrono::steady_clock::now());
    // Keep the aggregate clock in step: this entry point can be the ONLY one a
    // deferring capture loop ever reaches, so it has to persist the clock too
    // or a reader rebuilt out of that path would adopt nothing.
    publish_recovery_clock();
    return recovering;
  }

  capture_e display_vgd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ring) {
      return capture_e::error;
    }

    // TDR blast-radius shrink: once any GPU-reset-class event is recorded
    // (the encoder stall fires within ~100 ms of a hang), drop every shared
    // GPU allocation this reader holds — the cached cross-process slot
    // textures and the last-frame copy. Fewer live shared allocations for
    // VidMm to terminate while the driver stack recovers (a 0x10E_37
    // bugcheck in dxgmms2's pending-termination path took the whole machine
    // down in the field). The DROP is unconditional and immediate; the
    // reinit that used to accompany it is now a verdict of the liveness
    // ladder, which defers it while the ring says it is recovering.
    const bool tdr_edge = tdr::event_count() != _tdr_marks_at_open;
    if (tdr_edge && !_tdr_allocations_dropped) {
      BOOST_LOG(warning) << "LuminalVGD capture: GPU-reset event recorded; releasing shared ring textures.";
      release_shared_allocations();
      _tdr_allocations_dropped = true;
    }

    vgd_ring_sample_t sample {};
    if (!read_ring_sample(sample, tdr_edge)) {
      release_shared_allocations();
      return capture_e::reinit;
    }

    const auto now = std::chrono::steady_clock::now();
    if (_last_sequence == 0 && sample.latest_sequence == 0 &&
        _first_frame_deadline != std::chrono::steady_clock::time_point {} &&
        now >= _first_frame_deadline) {
      if (!_first_frame_timeout_reported) {
        _first_frame_timeout_reported = true;
        BOOST_LOG(error) << "LuminalVGD capture: ring was healthy before capture but published no first frame within 5 seconds; disabling direct ring capture for this session and falling back.";
      }
      g_broken_session.store(_session_id, std::memory_order_release);
      release_shared_allocations();
      return capture_e::reinit;
    }
    const auto decision = _liveness.evaluate(sample, now);

    // Publish the verdict for the ENCODER threads before acting on it. Holding
    // the capture loop through an outage is only half the job: the encoder runs
    // across that same window and ends the client's session on its first failed
    // encode unless it is told the display is coming back. One store, never a
    // clear-then-set (see platf::display_t::capture_recovering()).
    set_capture_recovering(decision.reason == vgd_ring_reason_e::recovering);
    // Persist the ladder's recovery clock so the budget survives the capture
    // reinits an outage causes, and covers the outage instead of restarting.
    publish_recovery_clock();

    switch (decision.verdict) {
      case vgd_ring_verdict_e::wait:
        if (decision.first_report) {
          log_ring_decision(decision, sample);
          _wait_logged_at = now;
        } else if (now - _wait_logged_at >= kRingWaitProgressLogInterval) {
          // A recovery runs for minutes. Without this the log goes silent for
          // all of it and "host wedged" is indistinguishable from "host
          // correctly waiting" after the fact. Rate-limited, and bounded
          // overall by the recovery budget.
          _wait_logged_at = now;
          BOOST_LOG(info) << "LuminalVGD capture: still waiting on the ring after "
                          << std::chrono::duration_cast<std::chrono::seconds>(_liveness.rebuilding_for(now)).count()
                          << " s; holding the capture.";
        }
        // Nothing to deliver and the GPU may be mid-reset: do not touch the
        // device (no cursor sync, no claim). Honour the caller's timeout rather
        // than returning instantly — the base capture loop treats `timeout` as
        // "nothing this round" and calls straight back, so an instant return
        // free-runs this thread at ~100 Hz for the whole multi-minute recovery,
        // burning a core in a SYSTEM service to re-read a header the driver
        // touches four times a second.
        wait_out_ring(timeout, sample);
        return capture_e::timeout;

      case vgd_ring_verdict_e::reinit:
        log_ring_decision(decision, sample);
        if (decision.reason == vgd_ring_reason_e::recovery_budget_expired) {
          // The budget is aggregate, so this is not "this reader waited long
          // enough" but "the host has waited out the whole outage and the ring
          // never came back" — past the point the driver's own budget should
          // have given up first. A plain reinit would build a fresh reader onto
          // the same stuck ring and wait again; latch instead so the reinit
          // lands on WGC/DDA and the session gets a definite answer.
          BOOST_LOG(error) << "LuminalVGD capture: ring never finished rebuilding within the host's "
                              "recovery budget; disabling ring capture for this session and falling "
                              "back to WGC/DDA.";
          g_broken_session.store(_session_id, std::memory_order_release);
          // Drop the clock everywhere. The ladder's copy first, so a later
          // publish cannot resurrect the spent one into the session-keyed store.
          _liveness.reset_recovery_clock();
          publish_recovery_clock();
        }
        release_shared_allocations();
        return capture_e::reinit;

      case vgd_ring_verdict_e::broken:
        log_ring_decision(decision, sample);
        g_broken_session.store(_session_id, std::memory_order_release);
        release_shared_allocations();
        return capture_e::reinit;

      case vgd_ring_verdict_e::consume:
        break;
    }

    // Cursor-capable driver: frames arrive cursor-free; pull the live
    // shape/position so this frame (or a cursor-only redelivery below)
    // blends the current cursor. Pre-cursor drivers compose it into the
    // frames and `_cursor` stays null.
    if (_cursor) {
      sync_cursor();
    }

    // True when the live cursor state has moved past what the client
    // last saw — shared by the wait loop below and the cursor-only
    // redelivery branch.
    const auto cursor_pending = [&]() {
      return (int32_t) _cursor_alpha.topleft_x != _delivered_cursor_x ||
             (int32_t) _cursor_alpha.topleft_y != _delivered_cursor_y ||
             _cursor_alpha.visible != _delivered_cursor_visible ||
             _cursor_shape_generation != _delivered_cursor_generation;
    };

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
      if (_cursor) {
        // Wake on cursor motion, not just frames: the driver publishes
        // nothing for cursor-only changes, so a wait that only watches
        // the ring is cursor-blind for its whole timeout — at motion
        // onset over an idle desktop that was a 200 ms + retry-cycle
        // rubber-band before the first cursor update reached the
        // encoder. DDA's AcquireNextFrame returns early on mouse
        // updates; breaking out to the redelivery path below restores
        // that latency model for the ring backend. The state read is a
        // volatile header peek — negligible at this loop's 1 kHz.
        sync_cursor();
        if (cursor_pending()) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!claimed) {
      // Nothing published — but with the cursor on its own plane, cursor
      // motion over an idle desktop produces no frames at all. Redeliver
      // the last frame with a fresh blend when the cursor state moved past
      // what the client last saw.
      if (_cursor && _last_frame && capture_format != DXGI_FORMAT_UNKNOWN) {
        if (cursor_pending()) {
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
          const HRESULT dst_acquire = seh_acquire_sync(d3d_img->capture_mutex.get(), 0, kEncoderMailboxMutexTimeoutMs);
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

    if (!refresh_transport()) {
      BOOST_LOG(warning) << "LuminalVGD capture: unable to read the selected ring transport; reinitializing.";
      return capture_e::reinit;
    }

    auto *slot = slot_texture(frame.generation, frame.index);
    if (!slot) {
      if (_open_failed_device_removed) {
        // Our own device is gone, not the ring. Rebuild against a fresh one;
        // blacklisting the session here would retire ring capture for the
        // rest of the process over a recoverable GPU event.
        BOOST_LOG(warning) << "LuminalVGD capture: local D3D device removed while opening a slot "
                              "texture; reinitializing against a fresh device.";
        _open_failures = 0;
        release_shared_allocations();
        return capture_e::reinit;
      }
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

    if (_fence_transport) {
      if (frame.ready_fence_value == 0 || !prepare_fence_transport(frame.generation)) {
        BOOST_LOG(error) << "LuminalVGD capture: fence transport frame lacks a usable producer fence value; reinitializing.";
        return capture_e::reinit;
      }
      const HRESULT wait_status = _device_ctx4->Wait(_producer_fence.get(), frame.ready_fence_value);
      if (FAILED(wait_status)) {
        BOOST_LOG(error) << "LuminalVGD capture: failed to queue producer-fence wait: "
                         << util::log_hex(wait_status);
        return capture_e::reinit;
      }
    }

    if (!_fence_transport) {
      const HRESULT acquire = seh_acquire_sync(slot->mutex.get(), kVgdMutexKeyHost, kVgdMutexTimeoutMs);
      if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) {
        return capture_e::timeout;
      }
      if (acquire != S_OK && acquire != static_cast<HRESULT>(WAIT_ABANDONED)) {
        BOOST_LOG(warning) << "LuminalVGD capture: slot mutex acquire failed [0x"sv << util::hex(acquire).to_string_view() << ']';
        return capture_e::reinit;
      }
    }
    auto mutex_guard = util::fail_guard([&]() {
      if (slot->mutex) seh_release_sync(slot->mutex.get(), kVgdMutexKeyHost);
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
    const HRESULT dst_acquire = seh_acquire_sync(d3d_img->capture_mutex.get(), 0, kEncoderMailboxMutexTimeoutMs);
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
      // Re-pull the live cursor state: the entry-time sample can be a
      // full wait window stale when the claim landed late in the
      // caller's timeout. Reads only the cursor section — safe under
      // the slot-mutex hold.
      sync_cursor();
      if (cursor_visible && (_cursor_alpha.visible || _cursor_xor.visible)) {
        blend_cursor_into(*d3d_img);
      }
      _delivered_cursor_x = (int32_t) _cursor_alpha.topleft_x;
      _delivered_cursor_y = (int32_t) _cursor_alpha.topleft_y;
      _delivered_cursor_visible = _cursor_alpha.visible;
      _delivered_cursor_generation = _cursor_shape_generation;
    }

    seh_release_sync(d3d_img->capture_mutex.get(), 0);

    if (_fence_transport && !finish_slot_copy()) {
      return capture_e::reinit;
    }

    // Copy done: hand the slot back before the encoder ever sees the image.
    mutex_guard.disable();
    if (slot->mutex) seh_release_sync(slot->mutex.get(), kVgdMutexKeyHost);
    claim_guard.disable();
    vgd_ring_release(_ring, frame.index);
    _last_sequence = frame.sequence;
    _liveness.note_delivered();

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
