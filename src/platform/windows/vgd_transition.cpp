/**
 * @file src/platform/windows/vgd_transition.cpp
 * @brief Forced WGC->VGD transition watcher (see vgd_transition.h).
 *
 * Structure mirrors the codebase's established watcher patterns: the
 * periodic tick runs on the single-threaded task_pool and stays
 * constant-time (one backend query + one cheap open/close probe); any
 * blocking work — the devnode ensure can hold SwDeviceCreate plus a
 * device-started wait for ~35 s — runs on a joinable worker thread with
 * a bounded shutdown_join(), exactly like vgd_devnode's rebind worker.
 */
// platform includes
#include <winsock2.h>
#include <windows.h>

// standard includes
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// local includes
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/windows/vgd_devnode.h"
#include "src/platform/windows/vgd_transition.h"
#include "src/platform/windows/virtual_display_backend.h"
#include "src/platform/windows/virtual_display_vgd.h"
#include "src/process.h"
#include "src/tdr_state.h"

using namespace std::chrono_literals;

namespace platf::vgd_transition {

  namespace {
    /// Watcher cadence. The tick is one atomic-ish backend query plus (only
    /// while un-transitioned) one open/close probe — cheap enough for the
    /// shared pool at this rate.
    constexpr auto kTickInterval = 60s;

    /// Devnode ensure attempts are expensive (SwDeviceCreate + started
    /// wait, worker occupied up to ~35 s) and only ever succeed after an
    /// installer stages the package — retry sparsely. The web UI button
    /// bypasses this limit (explicit user intent).
    constexpr auto kEnsureRetryEvery = 5min;

    std::atomic<bool> g_stop {false};
    std::atomic<bool> g_worker_in_flight {false};
    std::thread g_worker;
    /// Guards g_worker the OBJECT (join/assign/detach); the worker body is
    /// coordinated via the atomics.
    std::mutex g_worker_mutex;

    std::chrono::steady_clock::time_point g_last_ensure_attempt {};
    std::mutex g_ensure_stamp_mutex;

    bool ensure_attempt_due() {
      std::lock_guard lk(g_ensure_stamp_mutex);
      const auto now = std::chrono::steady_clock::now();
      return g_last_ensure_attempt == std::chrono::steady_clock::time_point {} ||
             now - g_last_ensure_attempt >= kEnsureRetryEvery;
    }

    /// Stamped only AFTER a worker actually spawned with the ensure phase
    /// enabled — a lost spawn race must not push the next automatic
    /// attempt out another full cadence.
    void stamp_ensure_attempt() {
      std::lock_guard lk(g_ensure_stamp_mutex);
      g_last_ensure_attempt = std::chrono::steady_clock::now();
    }

    /// The transition attempt. Spawn sites guarantee the backend was not
    /// LuminalVGD just before the spawn, so reaching LUMINALVGD here IS
    /// the WGC->VGD transition (the sentence itself is logged by backend
    /// selection at the moment of promotion, covering every heal path —
    /// this worker, the web UI kick, and the lazy per-query re-probe).
    void transition_worker(bool allow_ensure) {
      struct flight_guard_t {
        ~flight_guard_t() {
          g_worker_in_flight.store(false, std::memory_order_release);
        }
      } flight_guard;

      if (g_stop.load(std::memory_order_acquire)) {
        return;
      }

      // Phase 1: make a devnode exist, if the startup ensure couldn't.
      if (allow_ensure && !VDISPLAY::vgd::driver_appears_installed() &&
          !platf::vgd_devnode::device_expected()) {
        platf::vgd_devnode::ensure_retry();
      }
      if (g_stop.load(std::memory_order_acquire)) {
        return;
      }
      if (!VDISPLAY::vgd::driver_appears_installed()) {
        return;  // control device still not answering — the next tick re-checks
      }

      // Phase 2: heal the backend latch (logs the transition sentence on
      // promotion).
      if (VDISPLAY::reselect_if_none() != VDISPLAY::BackendType::LUMINALVGD) {
        return;
      }
      if (g_stop.load(std::memory_order_acquire)) {
        return;
      }

      // Phase 3: heal the driver-status latch — nvhttp/webrtc/confighttp
      // gate on proc::vDisplayDriverStatus, not the backend enum.
      if (proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        proc::initVDisplayDriver();
      }
      if (proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        BOOST_LOG(info) << "VGD transition: driver status initialized; virtual displays are "
                           "available without a service restart.";
      } else {
        BOOST_LOG(warning) << "VGD transition: backend selected but driver status init reported "
                           << static_cast<int>(proc::vDisplayDriverStatus.load())
                           << "; session launches will retry it.";
      }
    }

    /// Spawn the worker if none is running. Returns true when spawned.
    bool spawn_worker(bool allow_ensure) {
      std::lock_guard lk(g_worker_mutex);
      if (g_stop.load(std::memory_order_acquire)) {
        return false;
      }
      if (g_worker_in_flight.exchange(true, std::memory_order_acq_rel)) {
        return false;  // already running
      }
      if (g_worker.joinable()) {
        g_worker.join();  // reap the previous (finished) attempt
      }
      g_worker = std::thread(&transition_worker, allow_ensure);
      return true;
    }

    void watcher_tick() {
      if (g_stop.load(std::memory_order_acquire)) {
        return;  // teardown: queued ticks still run after task_pool.stop()
      }
      // Stack-down check FIRST: while selection is unlatched,
      // active_backend() itself performs a device open, and any device
      // I/O against a wedged WDDM stack can hang — on the single-threaded
      // shared task_pool that would stall input timers process-wide
      // (review finding).
      if (tdr::stack_down()) {
        task_pool.pushDelayed(&watcher_tick, kTickInterval);
        return;
      }
      if (VDISPLAY::active_backend() == VDISPLAY::BackendType::LUMINALVGD) {
        // Transitioned (by this watcher or by any lazy backend query) —
        // the latch can never revert, so the watcher's job is done. Live
        // WGC-fallback sessions are handled by their own fallback
        // monitors.
        return;
      }
      if (!platf::vgd_devnode::rebind_in_flight() &&
          !g_worker_in_flight.load(std::memory_order_acquire)) {
        const bool reachable = VDISPLAY::vgd::driver_appears_installed();
        bool allow_ensure = false;
        if (!reachable && !platf::vgd_devnode::device_expected()) {
          allow_ensure = ensure_attempt_due();
        }
        if ((reachable || allow_ensure) && spawn_worker(allow_ensure) && allow_ensure) {
          stamp_ensure_attempt();
        }
      }
      task_pool.pushDelayed(&watcher_tick, kTickInterval);
    }
  }  // namespace

  void init() {
    // Deliberately NO backend query here: platf::init() also runs inside
    // the unit-test fixture (PlatformTestSuite), and active_backend()'s
    // first invocation performs the full devnode ensure — a real
    // SwDeviceCreate on a driver dev box (re-verify finding). The first
    // tick does the query on the task pool and self-disarms when the
    // backend is already (or becomes) LuminalVGD.
    BOOST_LOG(info) << "VGD transition watcher armed: if the host is in WGC fallback, it "
                       "transitions to VGD Mode automatically once the LuminalVGD driver "
                       "is available.";
    task_pool.pushDelayed(&watcher_tick, kTickInterval);
  }

  void shutdown_join() {
    g_stop.store(true, std::memory_order_release);
    std::lock_guard lk(g_worker_mutex);
    if (!g_worker.joinable()) {
      return;
    }
    // Bounded: poll the in-flight flag (cleared by the worker's scope
    // guard) for up to 2 s, then detach — same contract as
    // vgd_devnode::shutdown_join(); a worker parked inside SwDeviceCreate
    // or UpdateDriver must not hold shutdown past the 10 s watchdog.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (g_worker_in_flight.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100ms);
    }
    if (!g_worker_in_flight.load(std::memory_order_acquire)) {
      g_worker.join();
      return;
    }
    BOOST_LOG(warning) << "VGD transition: shutting down with a transition attempt still in "
                          "flight; detaching the worker.";
    g_worker.detach();
  }

  const char *kick_status_name(kick_status status) {
    switch (status) {
      case kick_status::started:
        return "started";
      case kick_status::already_vgd:
        return "already_vgd";
      case kick_status::busy:
        return "busy";
      case kick_status::stack_down:
        return "stack_down";
      case kick_status::shutting_down:
        return "shutting_down";
    }
    return "unknown";
  }

  kick_status kick_now() {
    if (g_stop.load(std::memory_order_acquire)) {
      return kick_status::shutting_down;
    }
    // Stack-down check BEFORE any backend query: while unlatched,
    // active_backend() opens the device, and device I/O against a wedged
    // WDDM stack can hang the confighttp request thread.
    if (tdr::stack_down()) {
      return kick_status::stack_down;
    }
    // This query may itself complete the transition (lazy re-probe while
    // unlatched) — that is success, not a conflict.
    if (VDISPLAY::active_backend() == VDISPLAY::BackendType::LUMINALVGD) {
      return kick_status::already_vgd;
    }
    if (platf::vgd_devnode::rebind_in_flight() ||
        g_worker_in_flight.load(std::memory_order_acquire)) {
      return kick_status::busy;
    }
    // Explicit user intent overrides the sparse ensure cadence (the
    // worker's ensure phase runs regardless of the stamp; stamp it so the
    // periodic watcher doesn't immediately double-attempt).
    if (spawn_worker(true)) {
      stamp_ensure_attempt();
      return kick_status::started;
    }
    return kick_status::busy;
  }

  // -- active-capture-backend signal ---------------------------------------

  namespace {
    std::mutex g_capture_note_mutex;
    std::string g_capture_kind;
    std::string g_capture_display;
    std::chrono::steady_clock::time_point g_capture_noted_at {};
    std::uint64_t g_capture_sequence {0};
  }  // namespace

  void note_capture_backend(const char *kind, const std::string &display) {
    std::lock_guard lk(g_capture_note_mutex);
    g_capture_kind = kind ? kind : "";
    g_capture_display = display;
    g_capture_noted_at = std::chrono::steady_clock::now();
    ++g_capture_sequence;
  }

  capture_note last_capture_note() {
    std::lock_guard lk(g_capture_note_mutex);
    capture_note out;
    out.kind = g_capture_kind;
    out.display = g_capture_display;
    out.sequence = g_capture_sequence;
    out.age_ms = 0;
    if (g_capture_sequence != 0) {
      out.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - g_capture_noted_at
      )
                     .count();
    }
    return out;
  }

}  // namespace platf::vgd_transition
