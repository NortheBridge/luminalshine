/**
 * @file src/platform/windows/tdr_lifeboat.cpp
 * @brief Implementation of the TDR topology lifeboat (see tdr_lifeboat.h).
 *
 * Flow: tdr::mark_event → on_tdr_event (constant-time gate, caller's
 * thread) → task_pool → start_attempt (logs intent, arms the 5 s watchdog)
 * → detached worker thread → attempt_activate_physical_display (the
 * blocking enumerate + helper APPLY round-trip).
 *
 * The worker runs on its own thread rather than the task pool because the
 * pool is single-threaded and shared with input-event timers, while helper
 * IPC against a dying display stack can block indefinitely. The watchdog
 * task declares the attempt failed after ~5 s; a worker parked on dead IPC
 * is abandoned (detached, rate-limited to one per cooldown window).
 */
#include <winsock2.h>

#include "tdr_lifeboat.h"

// standard includes
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// lib includes
#include <display_device/types.h>

// local includes
#include "src/display_helper_builder.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/display_helper_integration.h"
#include "src/platform/windows/virtual_display.h"

namespace tdr_lifeboat {
  namespace {
    /// How long the lifeboat waits for the enumerate + APPLY round-trip
    /// before declaring the attempt failed. The stack may already be too
    /// far gone for the helper to answer — that is fine, the escalation
    /// paths (tdr::stack_down latch, session teardown) own terminal
    /// handling; the lifeboat just stops waiting.
    constexpr std::chrono::seconds ATTEMPT_TIMEOUT {5};

    std::mutex g_gate_mutex;
    attempt_gate g_gate;  ///< Guarded by g_gate_mutex.

    /// Shared between the detached worker thread and the watchdog task so
    /// a hung helper call can be abandoned without joining the thread.
    struct attempt_state_t {
      std::atomic<bool> done {false};
      std::atomic<bool> timed_out {false};
    };

    /// The blocking part of the attempt. Runs on a dedicated detached
    /// thread — never on mark_event's caller and never on the (single)
    /// task-pool thread. Must not throw.
    void attempt_activate_physical_display(const std::shared_ptr<attempt_state_t> &state) noexcept {
      const auto finish = [&state](bool success, const std::string &message) {
        state->done.store(true, std::memory_order_release);
        const char *late = state->timed_out.load(std::memory_order_acquire) ? " (late, past the watchdog deadline)" : "";
        if (success) {
          BOOST_LOG(warning) << "TDR lifeboat" << late << ": " << message;
        } else {
          BOOST_LOG(error) << "TDR lifeboat" << late << ": " << message;
        }
      };

      try {
        if (VDISPLAY::has_active_physical_display()) {
          finish(true, "a physical (non-virtual) display is already active; the OS has a hardware-backed output to recover onto — nothing to do.");
          return;
        }

        const auto devices = display_helper_integration::enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
        if (!devices || devices->empty()) {
          finish(false, "could not enumerate display devices; no physical display can be picked for activation.");
          return;
        }

        // has_active_physical_display() returned false, so every physical
        // device in the list is inactive — take the first one.
        std::string device_id;
        std::string device_label;
        for (const auto &device : *devices) {
          if (device.m_device_id.empty()) {
            continue;
          }
          if (VDISPLAY::is_virtual_display_enumerated_device(device)) {
            continue;
          }
          device_id = device.m_device_id;
          device_label = device.m_friendly_name.empty() ? device.m_device_id : device.m_friendly_name;
          break;
        }
        if (device_id.empty()) {
          finish(false, "no physical display device is connected; nothing to activate.");
          return;
        }

        BOOST_LOG(info) << "TDR lifeboat: asking the display helper to activate physical display \"" << device_label
                        << "\" (EnsureActive — the topology is extended; the virtual display stays active so capture keeps running on it).";

        display_device::SingleDisplayConfiguration config;
        config.m_device_id = device_id;
        config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;

        display_helper_integration::DisplayApplyRequest request;
        request.action = display_helper_integration::DisplayApplyAction::Apply;
        request.configuration = config;

        // display_helper_integration::apply() is the highest-level topology
        // API in the host: it routes through the interactive helper process,
        // which owns display-topology writes in this architecture (SYSTEM-
        // context SetDisplayConfig is unreliable, and the helper soft-tests
        // with SDC_VALIDATE before committing). With no session attached the
        // call neither hard-restarts a live helper nor falls back to an
        // in-process SetDisplayConfig.
        if (display_helper_integration::apply(request)) {
          finish(true, "physical display \"" + device_label + "\" activated (topology extended) — the OS now has a hardware-backed display for TDR recovery.");
        } else {
          finish(false, "the display helper rejected or failed activation of \"" + device_label + "\"; continuing without a lifeboat display (the escalation paths handle a failed recovery).");
        }
      } catch (...) {
        try {
          finish(false, "attempt raised an exception; giving up.");
        } catch (...) {
          // Nothing sane left to do — never propagate out of the worker.
        }
      }
    }

    /// Task-pool entry: logs intent, spawns the bounded worker. Must not
    /// block — the shared task pool runs input timers on its single thread.
    void start_attempt(tdr::source_t source) {
      try {
        BOOST_LOG(warning) << "TDR lifeboat: GPU/TDR failure detected (" << tdr::source_label(source)
                           << "); best-effort activating a physical display alongside the virtual display so Windows'"
                           << " TDR recovery has a hardware-backed output to recompose onto. In every observed"
                           << " machine-wide WDDM wedge, a virtual display was the exclusive active display"
                           << " (POSTMORTEM-2026-07-27).";

        auto state = std::make_shared<attempt_state_t>();

        std::thread worker([state]() {
          attempt_activate_physical_display(state);
        });
        worker.detach();

        task_pool.pushDelayed(
          [state]() {
            if (!state->done.load(std::memory_order_acquire)) {
              state->timed_out.store(true, std::memory_order_release);
              BOOST_LOG(error) << "TDR lifeboat: no result within " << ATTEMPT_TIMEOUT.count()
                               << " s; giving up on this attempt. The display stack may already be dead —"
                               << " that is expected sometimes, and the escalation paths handle it.";
            }
          },
          ATTEMPT_TIMEOUT
        );
      } catch (...) {
        try {
          BOOST_LOG(error) << "TDR lifeboat: failed to start the lifeboat attempt.";
        } catch (...) {
        }
      }
    }

    /// tdr:: event hook. Runs on mark_event's calling thread — often a
    /// time-critical path such as the NvEnc encode loop — so it stays
    /// constant time: one uncontended mutex for the gate and one task-pool
    /// push when the gate opens. Never throws, never blocks.
    void on_tdr_event(tdr::source_t source) noexcept {
      try {
        {
          std::lock_guard<std::mutex> lk(g_gate_mutex);
          if (!g_gate.should_attempt(source, std::chrono::steady_clock::now())) {
            return;
          }
        }
        // The gate is armed before the attempt runs, so any tdr:: event a
        // lifeboat attempt itself provokes (e.g. enumeration failing on a
        // dead stack) cannot re-enter and spawn another attempt.
        task_pool.push(start_attempt, source);
      } catch (...) {
        // Never propagate into mark_event's caller.
      }
    }
  }  // namespace

  void init() {
    tdr::set_event_hook(&on_tdr_event);
    // Self-heal probe for the stack-down latch (see tdr::stack_down): a
    // full QueryDisplayConfig succeeding with ≥1 active path proves the
    // stack came back, without needing a D3D11 attempt — which the
    // latch's own gates prevent from ever running while latched.
    tdr::set_stack_recheck([]() {
      LONG status = ERROR_SUCCESS;
      UINT32 paths = 0;
      return platf::dxgi::display_config_api_healthy(&status, &paths);
    });
    BOOST_LOG(info) << "TDR topology lifeboat armed: on a detected GPU hang, a physical display is activated"
                    << " alongside the virtual display (best-effort, at most once per "
                    << std::chrono::duration_cast<std::chrono::minutes>(ATTEMPT_COOLDOWN).count()
                    << " min).";
  }

}  // namespace tdr_lifeboat
