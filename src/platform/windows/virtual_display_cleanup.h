#pragma once

#ifdef _WIN32

  #include <chrono>
  #include <string_view>

namespace platf::virtual_display_cleanup {
  enum class revert_order_t {
    remove_before_restore,
    restore_before_remove,
  };

  struct cleanup_result_t {
    bool virtual_displays_removed {false};
    bool helper_revert_dispatched {false};
    bool database_restore_applied {false};
    bool physical_display_verified {false};
    bool helper_restarted {false};
    bool physical_fallback_applied {false};
  };

  // Explicit waits in run().
  constexpr std::chrono::seconds kRevertCompletionTimeout {30};
  constexpr std::chrono::seconds kPhysicalDisplayAfterRevertTimeout {2};
  constexpr std::chrono::seconds kPhysicalDisplayRecoveryTimeout {5};
  constexpr std::chrono::seconds kVirtualDisplayTeardownTimeout {5};

  // Upper bounds of the display-helper calls run() makes. This module cannot
  // include display_helper_integration.cpp or ipc/display_settings_client.cpp,
  // so their timeouts are mirrored here and must be updated with their source.
  // Each bound is the sum of every explicit timeout and sleep on the longest
  // path of the call. A pipe send is not counted: it waits only for buffer
  // space, which a one-message request cannot exhaust. OS calls with no timeout
  // of their own (process creation, SetDisplayConfig, display enumeration, driver
  // IOCTLs) are not counted either; the margin in
  // stream::session::teardown::display_cleanup_deadline absorbs their ordinary
  // latency, and the phase watchdog stays the backstop for a real hang.

  // display_helper_integration.cpp: kHelperForceKillWaitMs
  constexpr std::chrono::milliseconds kHelperForceKillWait {2000};
  // display_helper_integration.cpp: kHelperIpcReadyTimeout
  constexpr std::chrono::milliseconds kHelperIpcReadyTimeout {5000};
  // ipc/display_settings_client.cpp: kConnectTimeoutMs
  constexpr std::chrono::milliseconds kHelperIpcConnectTimeout {1000};
  // ipc/display_settings_client.cpp: kRevertAcceptedTimeoutMs
  constexpr std::chrono::milliseconds kHelperRevertAcceptedTimeout {2000};
  // ipc/display_settings_client.cpp: kApplyResultTimeoutMs
  constexpr std::chrono::milliseconds kHelperApplyResultTimeout {10000};

  // display_helper_integration.cpp: ensure_helper_started() when it has to
  // launch the helper. Terms: kill_all_helper_processes() waiting on one stray
  // instance; the early-exit poll (6 x 50 ms); the singleton-conflict (exit
  // code 3) retry (1000 ms wait, 300 ms settle); wait_for_helper_ipc_ready_locked().
  // It is longer than the live-helper ping probe (two connects, two 200 ms sleeps).
  constexpr std::chrono::milliseconds kHelperStartBound = kHelperForceKillWait + std::chrono::milliseconds {300 + 1000 + 300} + kHelperIpcReadyTimeout;

  // restart_helper_for_recovery(): ensure_helper_started(force_restart) on a
  // live helper. Terms: the start; terminating the bound helper; the 100 ms
  // named pipe / mutex settle; 5 start retries of 150 ms while the old instance
  // tears down.
  constexpr std::chrono::milliseconds kHelperRestartBound = kHelperStartBound + kHelperForceKillWait + std::chrono::milliseconds {100 + 5 * 150};

  // revert(): ensure_helper_started(), then send_revert() (connect, accepted reply).
  constexpr std::chrono::milliseconds kHelperRevertDispatchBound = kHelperStartBound + kHelperIpcConnectTimeout + kHelperRevertAcceptedTimeout;

  // ensure_physical_display_active(): apply() starts the helper, restarts it
  // when that did not leave it ready, then send_apply_json() (connect, result).
  constexpr std::chrono::milliseconds kPhysicalFallbackBound = kHelperStartBound + kHelperRestartBound + kHelperIpcConnectTimeout + kHelperApplyResultTimeout;

  // One REVERT attempt in run(): dispatch, wait for the helper to finish, wait
  // for a physical display.
  constexpr std::chrono::milliseconds kRevertAttemptBound = kHelperRevertDispatchBound + kRevertCompletionTimeout + kPhysicalDisplayAfterRevertTimeout;

  /**
   * @brief Longest time run() can spend with enforce_db_restore set: every
   *        bounded wait and helper call along the full recovery ladder.
   *
   * The session teardown watchdog derives its display-cleanup deadline from
   * this value so the ladder can finish in-process. The ladder is a REVERT
   * attempt, a helper restart and a second attempt, a database restore wait,
   * the physical fallback with its wait, and the virtual display removal wait.
   */
  constexpr std::chrono::seconds worst_case_duration() noexcept {
    return std::chrono::ceil<std::chrono::seconds>(
      kRevertAttemptBound + kHelperRestartBound + kRevertAttemptBound + kPhysicalDisplayRecoveryTimeout + kPhysicalFallbackBound + kPhysicalDisplayRecoveryTimeout + kVirtualDisplayTeardownTimeout
    );
  }

  cleanup_result_t run(
    std::string_view reason,
    bool enforce_db_restore = true,
    revert_order_t revert_order = revert_order_t::restore_before_remove,
    bool prefer_golden_if_current_missing = false
  );
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
