/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

  #include <cstdint>
  #include <string>

namespace platf::display_helper_client {
  // Shared IPC pipe name for the display-settings helper. Both ends of the
  // pipe — the server in tools/display_settings_helper.cpp and the client
  // in src/platform/windows/ipc/display_settings_client.cpp — read this
  // constant rather than embedding a literal so the name can never drift
  // between processes (a real bug we used to have when both sides spelled
  // the literal independently).
  //
  // The bare name is appended to the kernel `\.\pipe\` namespace by the
  // pipe factory; the full pipe path becomes:
  //   \\.\pipe\luminalshine_display_helper
  //
  // A legacy `sunshine_display_helper` pipe lives on hosts that haven't
  // yet had a luminalshine-renamed helper start; the MSI's KillProcs
  // custom action terminates the old helper during upgrade so the legacy
  // pipe goes away before any new client tries to connect. We
  // deliberately do NOT probe the legacy name from new clients: the only
  // peer that should answer on it is a stale helper from a half-finished
  // upgrade, and talking to that is worse than failing fast.
  inline constexpr const char *display_helper_pipe_name = "luminalshine_display_helper";

  // Send APPLY with JSON payload (SingleDisplayConfiguration)
  bool send_apply_json(const std::string &json);

  // Send REVERT with optional JSON payload.
  bool send_revert(const std::string &json_payload = {});

  // Export current OS display settings as a golden restore snapshot
  bool send_export_golden(const std::string &json_payload = {});

  // Best-effort cancel of any pending restore/watchdog activity on the helper
  bool send_disarm_restore();

  // Fast, best-effort DISARM that will not block longer than timeout_ms for connect/send.
  // Intended for stream start paths where we must stop helper activity immediately.
  bool send_disarm_restore_fast(int timeout_ms);

  // Save the current OS display state to session_current (rotate current->previous) without applying config.
  bool send_snapshot_current(const std::string &json_payload = {});

  // Reset helper-side persistence/state (best-effort)
  bool send_reset();

  /**
   * @brief Synthesise Ctrl+Win+Shift+B in the user's interactive desktop.
   *
   * This is Windows' built-in WDDM reset shortcut. Routed through the
   * display helper because SYSTEM-context callers cannot SendInput to
   * an interactive session — the helper, which runs in the user
   * session, can. Used as the highest level of the SudoVDA recovery
   * ladder when handle-recycle + PnP disable/enable have already been
   * tried and the entire WDDM context is suspected stuck.
   *
   * Fire-and-forget; the helper has no synchronous reply for this
   * message type. The keystroke takes effect within ~1-2 seconds.
   */
  bool send_wddm_reset();

  // Request helper process to terminate gracefully.
  bool send_stop();

  // Lightweight liveness probe; returns true if a Ping frame was sent.
  // This does not wait for a reply; it only validates a healthy send path.
  bool send_ping();

  // Reset the cached connection so the next send will reconnect.
  void reset_connection();
}  // namespace platf::display_helper_client

#endif
