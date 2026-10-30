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

  namespace detail {
    // Upper bounds of the display-helper calls run() makes. The timeouts they
    // are built from are file-local (anonymous namespaces in
    // display_helper_integration.cpp and ipc/display_settings_client.cpp), so
    // they are mirrored here; a drift test in test_session_teardown_policy.cpp
    // reads those files and fails when a mirror goes stale.
    //
    // Each bound is the sum of the explicit timeouts and sleeps on the nominal
    // longest path of the call. It is not a hard upper limit. Not counted:
    //  - the pipe handshake inside every connect (pipes.cpp: up to 2 s for the
    //    handshake message, 5 s to send the ACK, a 5 s data-pipe retry), and
    //    the overrun of wait_for_helper_ipc_ready_locked(), which checks its
    //    deadline only at the top of its loop;
    //  - helper_mutex and pipe_mutex contention: the helper watchdog's
    //    ensure_helper_started() can hold helper_mutex for about 8.6 s, and
    //    another run() caller holds it for its whole REVERT wait;
    //  - a pipe send, which waits only for buffer space;
    //  - OS calls with no timeout of their own (process creation,
    //    SetDisplayConfig, display enumeration, driver IOCTLs).
    // The margin in stream::session::teardown::display_cleanup_deadline covers
    // their ordinary latency, and the phase watchdog stays the backstop for a
    // real hang.

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
    // launch the helper. Terms: kill_all_helper_processes(), which waits up to
    // 2 s per stray instance and matches both luminalshine_display_helper.exe
    // and the legacy sunshine_display_helper.exe (the bound assumes one stray);
    // the early-exit poll (6 x 50 ms); the singleton-conflict (exit code 3)
    // retry (1000 ms wait, 300 ms settle); wait_for_helper_ipc_ready_locked().
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
  }  // namespace detail

  /**
   * @brief Longest time run() spends on its explicit waits and helper calls,
   *        along its nominal longest path.
   *
   * enforce_db_restore is run()'s argument of the same name. With it set the
   * path is the full recovery ladder: a REVERT attempt, a helper restart and a
   * second attempt, a database restore wait, the physical fallback with its
   * wait, and the virtual display removal wait. Without it run() only removes
   * the virtual displays and waits for them to go.
   *
   * The session teardown watchdogs derive their display-cleanup deadline from
   * this value so the ladder can finish in-process. Pipe handshake waits and
   * mutex contention are not counted (see detail); the deadline's margin and
   * the watchdog cover them.
   */
  constexpr std::chrono::seconds worst_case_duration(bool enforce_db_restore = true) noexcept {
    if (!enforce_db_restore) {
      return std::chrono::ceil<std::chrono::seconds>(kVirtualDisplayTeardownTimeout);
    }
    return std::chrono::ceil<std::chrono::seconds>(
      detail::kRevertAttemptBound + detail::kHelperRestartBound + detail::kRevertAttemptBound + kPhysicalDisplayRecoveryTimeout + detail::kPhysicalFallbackBound + kPhysicalDisplayRecoveryTimeout + kVirtualDisplayTeardownTimeout
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
