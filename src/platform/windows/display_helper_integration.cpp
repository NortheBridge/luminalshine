/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

  #include <winsock2.h>

  // standard
  #include <algorithm>
  #include <atomic>
  #include <boost/algorithm/string/predicate.hpp>
  #include <chrono>
  #include <cmath>
  #include <cstdint>
  #include <filesystem>
  #include <limits>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  // libdisplaydevice
  #include <display_device/json.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_recovery.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // sunshine
  #include "display_helper_integration.h"
  #include "src/globals.h"
  #include "src/gpu_recovery_policy.h"
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_coordinator.h"
  #include "src/platform/windows/display_helper_request_helpers.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/ipc/display_settings_client.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/display.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/platform/windows/virtual_display_vgd.h"
  #include "src/process.h"
  #include "src/tdr_state.h"

  #include <display_device/noop_audio_context.h>
  #include <display_device/noop_settings_persistence.h>
  #include <display_device/windows/persistent_state.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/types.h>
  #include <tlhelp32.h>

namespace {
  std::atomic<display_helper_integration::ApplyFailure> g_last_apply_failure {
    display_helper_integration::ApplyFailure::none
  };
  // Serialize helper start/inspect to avoid races that could spawn duplicate helpers
  std::mutex &helper_mutex() {
    static std::mutex m;
    return m;
  }

  // Persistent process handler to keep helper alive while Sunshine runs
  ProcessHandler &helper_proc() {
    static ProcessHandler h(/*use_job=*/false);
    return h;
  }

  struct PendingSessionSnapshot {
    int width = 0;
    int height = 0;
    int fps = 0;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
    std::optional<int> framegen_refresh_rate;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  struct PendingApplyState {
    display_helper_integration::DisplayApplyRequest request;
    PendingSessionSnapshot session_snapshot;
    uint32_t session_id {0};
    bool has_session {false};
    int attempts {0};
    std::optional<std::chrono::steady_clock::time_point> ready_since;
    std::chrono::steady_clock::time_point next_attempt {};
  };

  std::mutex &pending_apply_mutex() {
    static std::mutex m;
    return m;
  }

  std::optional<PendingApplyState> &pending_apply_state() {
    static std::optional<PendingApplyState> state;
    return state;
  }

  std::atomic<bool> &cold_start_resolution_deferral_armed() {
    static std::atomic<bool> armed {true};
    return armed;
  }

  bool user_session_ready();

  bool request_includes_resolution(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      return false;
    }
    return request.configuration->m_resolution.has_value();
  }

  PendingApplyState make_pending_apply_state(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state;
    state.request = request;
    state.has_session = request.session != nullptr;
    state.request.session = nullptr;

    if (request.session) {
      state.session_id = request.session->id;
      state.session_snapshot.width = request.session->width;
      state.session_snapshot.height = request.session->height;
      state.session_snapshot.fps = request.session->fps;
      state.session_snapshot.enable_hdr = request.session->enable_hdr;
      state.session_snapshot.enable_sops = request.session->enable_sops;
      state.session_snapshot.virtual_display = request.session->virtual_display;
      state.session_snapshot.virtual_display_device_id = request.session->virtual_display_device_id;
      state.session_snapshot.virtual_display_ready_since = request.session->virtual_display_ready_since;
      state.session_snapshot.framegen_refresh_rate = request.session->framegen_refresh_rate;
      state.session_snapshot.gen1_framegen_fix = request.session->gen1_framegen_fix;
      state.session_snapshot.gen2_framegen_fix = request.session->gen2_framegen_fix;
    }

    return state;
  }

  void queue_deferred_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state = make_pending_apply_state(request);
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    pending_apply_state() = std::move(state);
    BOOST_LOG(info) << "Display helper: deferring resolution apply for session " << pending_apply_state()->session_id << ".";
  }

  void maybe_queue_deferred_resolution_apply_on_api_unavailable(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.session) {
      return;
    }
    if (!request_includes_resolution(request)) {
      return;
    }
    queue_deferred_resolution_apply(request);
    BOOST_LOG(info) << "Display helper: API unavailable; queued deferred resolution apply for session "
                    << pending_apply_state()->session_id << ".";
  }

  bool should_defer_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.session) {
      return false;
    }
    if (!request_includes_resolution(request)) {
      return false;
    }
    if (!platf::is_running_as_system()) {
      return false;
    }
    if (user_session_ready()) {
      return false;
    }
    return true;
  }

  void maybe_queue_deferred_resolution_apply(
    const display_helper_integration::DisplayApplyRequest &request,
    bool allow_resolution_deferral
  ) {
    if (!allow_resolution_deferral) {
      return;
    }
    if (!should_defer_resolution_apply(request)) {
      return;
    }
    bool expected = true;
    if (!cold_start_resolution_deferral_armed().compare_exchange_strong(expected, false)) {
      return;
    }
    queue_deferred_resolution_apply(request);
  }

  bool user_session_ready() {
    HANDLE user_token = platf::dxgi::retrieve_users_token(false);
    if (!user_token) {
      return false;
    }
    CloseHandle(user_token);
    return true;
  }

  constexpr std::chrono::seconds kTopologyWaitTimeout {6};
  constexpr std::chrono::milliseconds kHelperIpcReadyTimeout {5000};
  constexpr std::chrono::milliseconds kHelperIpcReadyPoll {100};

  // Stream-start requirement: stop very recent helper restore activity quickly.
  // Once the helper has had time to begin an actual restore, do not kill/overwrite
  // that in-flight restore from a later stream-start probe; the helper will either
  // finish restoring or an explicit APPLY will supersede it.
  constexpr std::chrono::milliseconds kDisarmRestoreBudget {150};
  constexpr std::chrono::milliseconds kDisarmRetryThrottle {150};
  constexpr std::chrono::milliseconds kDisarmRestoreGrace {5000};
  constexpr std::chrono::milliseconds kDeferredApplyInitialDelay {2000};
  constexpr std::chrono::milliseconds kDeferredApplyRetryBase {500};
  constexpr std::chrono::milliseconds kDeferredApplyRetryMax {10000};
  constexpr int kMaxDeferredApplyAttempts = 6;

  bool shutdown_requested();
  bool ensure_helper_started(bool force_restart = false, bool force_enable = false);
  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout);

  bool helper_process_running() {
    std::lock_guard<std::mutex> lg(helper_mutex());
    if (HANDLE h = helper_proc().get_process_handle()) {
      return WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    }
    return false;
  }

  bool restore_expected_with_live_helper();

  std::chrono::milliseconds deferred_apply_retry_delay(int attempts) {
    if (attempts <= 0) {
      return kDeferredApplyRetryBase;
    }
    const int shift = std::min(attempts - 1, 5);
    auto delay = kDeferredApplyRetryBase * (1 << shift);
    if (delay > kDeferredApplyRetryMax) {
      delay = kDeferredApplyRetryMax;
    }
    return delay;
  }

  struct InProcessDisplayContext {
    std::shared_ptr<display_device::SettingsManagerInterface> settings_mgr;
    std::shared_ptr<display_device::WinDisplayDeviceInterface> display;
  };

  std::optional<InProcessDisplayContext> make_settings_manager() {
    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
      auto impersonated_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
      auto audio = std::make_shared<display_device::NoopAudioContext>();
      auto persistence = std::make_unique<display_device::PersistentState>(
        std::make_shared<display_device::NoopSettingsPersistence>()
      );
      auto settings_mgr = std::make_shared<display_device::SettingsManager>(
        impersonated_dd,
        audio,
        std::move(persistence),
        display_device::WinWorkarounds {}
      );
      return InProcessDisplayContext {
        .settings_mgr = std::move(settings_mgr),
        .display = std::move(impersonated_dd),
      };
    } catch (const std::exception &ex) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager: " << ex.what();
    } catch (...) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager due to unknown error.";
    }
    return std::nullopt;
  }

  bool device_id_equals_ci(const std::string &lhs, const std::string &rhs) {
    if (lhs.empty() || rhs.empty()) {
      return false;
    }
    return boost::iequals(lhs, rhs);
  }

  bool device_is_active(const std::string &device_id) {
    if (device_id.empty()) {
      return false;
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }

    for (const auto &device : *devices) {
      if (device.m_device_id.empty() || !device.m_info) {
        continue;
      }
      if (device_id_equals_ci(device.m_device_id, device_id)) {
        return true;
      }
    }
    return false;
  }

  double floating_point_value(const display_device::FloatingPoint &value) {
    if (const auto *number = std::get_if<double>(&value)) {
      return *number;
    }
    const auto &rational = std::get<display_device::Rational>(value);
    return rational.m_denominator == 0 ? 0.0 :
           static_cast<double>(rational.m_numerator) / rational.m_denominator;
  }

  std::string refresh_rate_description(const display_device::FloatingPoint &value) {
    if (const auto *number = std::get_if<double>(&value)) {
      return std::to_string(*number) + " (floating)";
    }
    const auto &rational = std::get<display_device::Rational>(value);
    return std::to_string(rational.m_numerator) + '/' + std::to_string(rational.m_denominator);
  }

  bool requested_mode_is_active(const display_device::SingleDisplayConfiguration &configuration) {
    if (configuration.m_device_id.empty()) {
      return false;
    }
    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
      display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }
    for (const auto &device : *devices) {
      if (!device.m_info || !device_id_equals_ci(device.m_device_id, configuration.m_device_id)) {
        continue;
      }
      if (configuration.m_resolution &&
          device.m_info->m_resolution != *configuration.m_resolution) {
        return false;
      }
      if (configuration.m_refresh_rate) {
        const double requested = floating_point_value(*configuration.m_refresh_rate);
        const double active = floating_point_value(device.m_info->m_refresh_rate);
        // Windows commonly reports 239.760 for a nominal 240 Hz mode.
        if (std::abs(requested - active) > 0.5) {
          return false;
        }
      }
      if (configuration.m_hdr_state && device.m_info->m_hdr_state &&
          device.m_info->m_hdr_state != configuration.m_hdr_state) {
        return false;
      }
      return true;
    }
    return false;
  }

  bool requested_core_mode_is_active(const display_device::SingleDisplayConfiguration &configuration) {
    if (configuration.m_device_id.empty()) {
      return false;
    }
    const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
      display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }
    for (const auto &device : *devices) {
      if (!device.m_info || !device_id_equals_ci(device.m_device_id, configuration.m_device_id)) {
        continue;
      }
      if (configuration.m_resolution && device.m_info->m_resolution != *configuration.m_resolution) {
        return false;
      }
      if (configuration.m_refresh_rate) {
        const double requested = floating_point_value(*configuration.m_refresh_rate);
        const double active = floating_point_value(device.m_info->m_refresh_rate);
        if (std::abs(requested - active) > 0.5) {
          return false;
        }
      }
      return true;
    }
    return false;
  }

  bool wait_for_requested_core_mode(
    const display_device::SingleDisplayConfiguration &configuration,
    std::chrono::steady_clock::duration timeout
  ) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (requested_core_mode_is_active(configuration)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline && !shutdown_requested());
    return false;
  }

  bool apply_best_effort_hdr_now(
    const std::string &device_id,
    std::optional<display_device::HdrState> desired_hdr
  ) {
    if (device_id.empty() || !desired_hdr) {
      return true;
    }
    if (!device_is_active(device_id)) {
      BOOST_LOG(warning) << "Display helper: pre-capture HDR skipped because the target is not active.";
      return false;
    }
    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto win_dd = std::make_shared<display_device::WinDisplayDevice>(api);
      auto impersonated = std::make_shared<display_device::ImpersonatingDisplayDevice>(win_dd);
      const display_device::HdrStateMap states {{device_id, desired_hdr}};
      const bool applied = impersonated->setHdrStates(states);
      if (applied) {
        BOOST_LOG(info) << "Display helper: pre-capture HDR applied for device_id=" << device_id << ".";
      } else {
        BOOST_LOG(warning) << "Display helper: pre-capture HDR did not stick for device_id=" << device_id << ".";
      }
      return applied;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Display helper: pre-capture HDR failed: " << e.what();
    } catch (...) {
      BOOST_LOG(warning) << "Display helper: pre-capture HDR failed with an unknown exception.";
    }
    return false;
  }

  void log_observed_mode(
    const display_device::SingleDisplayConfiguration &configuration,
    const char *stage
  ) {
    const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
      display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      BOOST_LOG(warning) << "Display helper: " << stage << " could not enumerate the observed mode.";
      return;
    }
    for (const auto &device : *devices) {
      if (!device.m_info || !device_id_equals_ci(device.m_device_id, configuration.m_device_id)) {
        continue;
      }
      BOOST_LOG(info) << "Display helper: " << stage << " observed device_id=" << device.m_device_id
                      << " mode=" << device.m_info->m_resolution.m_width << 'x'
                      << device.m_info->m_resolution.m_height << '@'
                      << floating_point_value(device.m_info->m_refresh_rate) << "Hz"
                      << " active_refresh=" << refresh_rate_description(device.m_info->m_refresh_rate)
                      << " requested_refresh="
                      << (configuration.m_refresh_rate ? refresh_rate_description(*configuration.m_refresh_rate) : "topology-only")
                      << (requested_core_mode_is_active(configuration) ? " (core mode matched)" : " (core mode mismatch)")
                      << ", HDR="
                      << (device.m_info->m_hdr_state ?
                            (*device.m_info->m_hdr_state == display_device::HdrState::Enabled ? "enabled" : "disabled") :
                            "unknown")
                      << (configuration.m_hdr_state && device.m_info->m_hdr_state &&
                          configuration.m_hdr_state != device.m_info->m_hdr_state ?
                            " (deferred; not admission-critical)" : "");
      return;
    }
    BOOST_LOG(warning) << "Display helper: " << stage << " device_id=" << configuration.m_device_id
                       << " was not active/enumerated.";
  }

  bool wait_for_requested_mode(
    const display_device::SingleDisplayConfiguration &configuration,
    std::chrono::steady_clock::duration timeout
  ) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (requested_mode_is_active(configuration)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline && !shutdown_requested());
    return false;
  }

  std::string build_snapshot_exclude_payload() {
    try {
      nlohmann::json j = config::video.dd.snapshot_exclude_devices;
      return j.dump();
    } catch (...) {
      return std::string {};
    }
  }

  bool wait_for_device_activation(const std::string &device_id, std::chrono::steady_clock::duration timeout) {
    if (device_id.empty()) {
      return false;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (device_is_active(device_id)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool wait_for_virtual_display_activation(std::chrono::steady_clock::duration timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      bool any_active = std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      );
      if (any_active) {
        return true;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool verify_helper_topology(
    const rtsp_stream::launch_session_t &session,
    const std::string &device_id
  ) {
    if (!device_id.empty()) {
      const bool has_activation_hint = session.virtual_display &&
                                       session.virtual_display_ready_since.has_value() &&
                                       !session.virtual_display_device_id.empty() &&
                                       device_id_equals_ci(device_id, session.virtual_display_device_id);
      if (has_activation_hint && device_is_active(device_id)) {
        BOOST_LOG(debug) << "Display helper: device_id " << device_id
                         << " already active; skipping activation wait.";
        return true;
      }

      if (!wait_for_device_activation(device_id, kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: device_id " << device_id << " did not become active after APPLY.";
        return false;
      }
      return true;
    }

    if (session.virtual_display) {
      const bool hint_ready = session.virtual_display_ready_since.has_value();
      if (hint_ready) {
        BOOST_LOG(debug) << "Display helper: virtual display ready hint satisfied. Skipping activation wait.";
        return true;
      }
      if (!wait_for_virtual_display_activation(kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: virtual display topology did not become active after APPLY.";
        return false;
      }
    }

    return true;
  }

  bool apply_topology_definition(
    const display_helper_integration::DisplayTopologyDefinition &topology,
    const char *label
  ) {
    if (topology.topology.empty() && topology.monitor_positions.empty()) {
      return true;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      BOOST_LOG(warning) << "Display helper: unable to initialize display context for topology apply (" << label << ").";
      return false;
    }

    bool topology_ok = true;
    if (!topology.topology.empty()) {
      try {
        auto current_topology = ctx->display->getCurrentTopology();
        const bool already_matches = ctx->display->isTopologyTheSame(current_topology, topology.topology);
        if (!already_matches) {
          BOOST_LOG(info) << "Display helper: applying requested topology (" << label << ").";
          topology_ok = ctx->display->setTopology(topology.topology);
          if (!topology_ok) {
            BOOST_LOG(warning) << "Display helper: requested topology apply failed (" << label << ").";
          }
        } else {
          BOOST_LOG(debug) << "Display helper: requested topology already active (" << label << ").";
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << "): " << ex.what();
        topology_ok = false;
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << ") with an unknown error.";
        topology_ok = false;
      }
    }

    for (const auto &[device_id, point] : topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper: setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ") after " << label << ".";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return topology_ok;
  }

  display_device::SettingsManagerInterface::ApplyResult apply_in_process(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper (in-process): no configuration provided for APPLY request.";
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    const auto result = ctx->settings_mgr->applySettings(*request.configuration);
    const bool ok = (result == display_device::SettingsManagerInterface::ApplyResult::Ok);
    BOOST_LOG(info) << "Display helper (in-process): APPLY result=" << (ok ? "Ok" : "Failed");
    if (!ok) {
      return result;
    }

    // Apply optional topology/placement tweaks when provided.
    if (!request.topology.topology.empty()) {
      BOOST_LOG(debug) << "Display helper (in-process): applying topology override.";
      (void) ctx->display->setTopology(request.topology.topology);
    }
    for (const auto &[device_id, point] : request.topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper (in-process): setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ").";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return display_device::SettingsManagerInterface::ApplyResult::Ok;
  }

  constexpr DWORD kHelperForceKillWaitMs = 2000;

  bool wait_for_helper_ipc_ready_locked() {
    const auto deadline = std::chrono::steady_clock::now() + kHelperIpcReadyTimeout;
    int attempts = 0;

    platf::display_helper_client::reset_connection();
    while (std::chrono::steady_clock::now() < deadline) {
      if (shutdown_requested()) {
        return false;
      }
      if (platf::display_helper_client::send_ping()) {
        if (attempts > 0) {
          BOOST_LOG(debug) << "Display helper IPC became reachable after " << attempts << " retries.";
        }
        return true;
      }
      ++attempts;
      std::this_thread::sleep_for(kHelperIpcReadyPoll);
      platf::display_helper_client::reset_connection();
    }

    BOOST_LOG(warning) << "Display helper IPC did not respond within " << kHelperIpcReadyTimeout.count()
                       << " ms of helper start.";
    return false;
  }

  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout) {
    using enum display_helper_integration::VirtualDisplayArrangement;
    switch (layout) {
      case Extended:
        return "extended";
      case ExtendedPrimary:
        return "extended_primary";
      case ExtendedIsolated:
        return "extended_isolated";
      case ExtendedPrimaryIsolated:
        return "extended_primary_isolated";
      case Exclusive:
      default:
        return "exclusive";
    }
  }

  void kill_all_helper_processes() {
    helper_proc().terminate();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "Display helper: failed to snapshot processes for cleanup (winerr=" << err << ").";
      return;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> targets;

    if (Process32FirstW(snapshot, &entry)) {
      do {
        // Match both the new luminalshine_display_helper.exe and the legacy
        // sunshine_display_helper.exe so cleanup still works on hosts where
        // an old helper from a pre-26.05.1 install survived the upgrade
        // (e.g. service was offline when the MSI ran). The MSI's KillProcs
        // custom action also enumerates both names — this is the runtime
        // counterpart of that list.
        if ((_wcsicmp(entry.szExeFile, L"luminalshine_display_helper.exe") == 0 ||
             _wcsicmp(entry.szExeFile, L"sunshine_display_helper.exe") == 0) &&
            entry.th32ProcessID != GetCurrentProcessId()) {
          targets.push_back(entry.th32ProcessID);
        }
      } while (Process32NextW(snapshot, &entry));
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_NO_MORE_FILES) {
        BOOST_LOG(warning) << "Display helper: process enumeration failed during cleanup (winerr=" << err << ").";
      }
    }

    CloseHandle(snapshot);

    for (DWORD pid : targets) {
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
      if (!h) {
        DWORD err = GetLastError();
        BOOST_LOG(warning) << "Display helper: unable to open external instance (pid=" << pid
                           << ", winerr=" << err << ") for termination.";
        continue;
      }

      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        BOOST_LOG(warning) << "Display helper: terminating external instance (pid=" << pid << ").";
        if (!TerminateProcess(h, 1)) {
          DWORD err = GetLastError();
          BOOST_LOG(error) << "Display helper: TerminateProcess failed for pid=" << pid << " (winerr=" << err << ").";
        } else {
          DWORD wait_res = WaitForSingleObject(h, kHelperForceKillWaitMs);
          if (wait_res != WAIT_OBJECT_0) {
            BOOST_LOG(warning) << "Display helper: external instance pid=" << pid
                               << " did not exit within " << kHelperForceKillWaitMs << " ms.";
          }
        }
      }

      CloseHandle(h);
    }
  }

  struct session_dd_fields_t {
    int width = -1;
    int height = -1;
    int fps = -1;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<int> framegen_refresh_rate;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  static std::mutex g_session_mutex;
  static std::optional<session_dd_fields_t> g_active_session_dd;

  // Tracks whether we've recently requested a helper REVERT and therefore expect a restore loop to be active.
  // Used to avoid spamming DISARM frames and to enable a kill-switch if IPC is wedged.
  static std::atomic<bool> g_restore_expected {false};
  static std::atomic<std::uint64_t> g_restore_generation {0};
  static std::atomic<std::uint64_t> g_disarm_generation_sent {0};
  static std::atomic<std::int64_t> g_last_revert_us {0};
  static std::atomic<std::int64_t> g_last_revert_completed_us {0};
  static std::atomic<std::int64_t> g_last_disarm_attempt_us {0};
  static std::atomic<std::int64_t> g_last_disarm_success_us {0};

  // Tracks when the most recent successful APPLY completed, so the capture thread
  // can add a stabilization delay before attempting to reinit after topology changes.
  static std::atomic<std::int64_t> g_last_apply_completed_us {0};

  static std::int64_t now_steady_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
  }

  bool restore_expected_with_live_helper() {
    if (!g_restore_expected.load(std::memory_order_relaxed)) {
      return false;
    }
    if (helper_process_running()) {
      return true;
    }
    g_restore_expected.store(false, std::memory_order_relaxed);
    return false;
  }

  // Active session display parameters snapshot for re-apply on reconnect.
  // We do NOT cache serialized JSON, only the subset of session fields that
  // affect display configuration. On reconnect, we rebuild the full
  // SingleDisplayConfiguration from current Sunshine config + these fields.

  bool dd_feature_enabled() {
    using config_option_e = config::video_t::dd_t::config_option_e;
    if (config::video.dd.configuration_option != config_option_e::disabled) {
      return true;
    }

    const bool virtual_display_selected =
      (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::per_client ||
       config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::shared);
    if (virtual_display_selected) {
      return true;
    }

    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd && g_active_session_dd->virtual_display;
  }

  bool shutdown_requested() {
    if (!mail::man) {
      return false;
    }
    try {
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      return shutdown_event && shutdown_event->peek();
    } catch (...) {
      return false;
    }
  }

  bool disarm_helper_restore_if_running() {
    if (shutdown_requested()) {
      return false;
    }

    const bool helper_running = helper_process_running();
    if (!helper_running) {
      g_restore_expected.store(false, std::memory_order_relaxed);
      return false;
    }

    const bool restore_expected = g_restore_expected.load(std::memory_order_relaxed);
    const auto now_us = now_steady_us();
    const auto last_revert_us = g_last_revert_us.load(std::memory_order_relaxed);
    if (restore_expected && last_revert_us > 0) {
      const auto disarm_grace_us = kDisarmRestoreGrace.count() * 1000LL;
      if ((now_us - last_revert_us) >= disarm_grace_us) {
        BOOST_LOG(info) << "Display helper: restore has been pending for more than "
                        << kDisarmRestoreGrace.count()
                        << "ms; not sending DISARM so the unconfirmed restore can complete.";
        return false;
      }
    }

    const auto last_attempt_us = g_last_disarm_attempt_us.load(std::memory_order_relaxed);

    // Don't spam DISARM frames (they share the helper's job/message queues with APPLY/REVERT).
    if ((now_us - last_attempt_us) < (kDisarmRetryThrottle.count() * 1000)) {
      const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
      return (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000);
    }

    // If we believe a restore loop is active, ensure we only issue one DISARM per restore generation unless it fails
    // and the throttle allows a retry.
    const auto restore_generation = g_restore_generation.load(std::memory_order_relaxed);
    if (restore_expected) {
      const auto disarmed_generation = g_disarm_generation_sent.load(std::memory_order_relaxed);
      if (disarmed_generation >= restore_generation) {
        const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
        return (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000);
      }
    }

    using namespace std::chrono;
    const auto start = steady_clock::now();
    const auto deadline = start + kDisarmRestoreBudget;
    auto remaining_ms = [&]() -> int {
      const auto now = steady_clock::now();
      if (now >= deadline) {
        return 0;
      }
      return static_cast<int>(duration_cast<milliseconds>(deadline - now).count());
    };

    // Bound total blocking to kDisarmRestoreBudget by splitting the budget across connect+send.
    auto try_send_fast = [&](int max_total_ms) -> bool {
      const int per_op_ms = std::max(10, max_total_ms / 2);
      return platf::display_helper_client::send_disarm_restore_fast(per_op_ms);
    };

    g_last_disarm_attempt_us.store(now_us, std::memory_order_relaxed);
    bool ok = try_send_fast(static_cast<int>(kDisarmRestoreBudget.count()));
    if (!ok) {
      const int rem = remaining_ms();
      if (rem > 20) {
        platf::display_helper_client::reset_connection();
        ok = try_send_fast(rem);
      }
    }

    if (ok) {
      g_last_disarm_success_us.store(now_us, std::memory_order_relaxed);
      g_disarm_generation_sent.store(restore_generation, std::memory_order_relaxed);
      g_restore_expected.store(false, std::memory_order_relaxed);
      BOOST_LOG(info) << "Display helper: DISARM dispatched (fast).";
      return true;
    }

    // Fail-safe: if we recently initiated a helper restore, and DISARM couldn't be delivered quickly,
    // terminate the helper so restore activity stops immediately (prevents virtual display crash loops).
    const bool revert_recent = (now_us - last_revert_us) < (30LL * 1000LL * 1000LL);
    if (revert_recent) {
      BOOST_LOG(warning) << "Display helper: DISARM could not be delivered within "
                         << kDisarmRestoreBudget.count() << "ms; terminating helper to stop restore activity.";
      {
        std::lock_guard<std::mutex> lg(helper_mutex());
        helper_proc().terminate();
      }
      platf::display_helper_client::reset_connection();
      g_restore_expected.store(false, std::memory_order_relaxed);
    }

    return false;
  }

  bool ensure_helper_started(bool force_restart, bool force_enable) {
    if (!force_enable && !dd_feature_enabled()) {
      return false;
    }
    const bool shutting_down = shutdown_requested();
    std::lock_guard<std::mutex> lg(helper_mutex());
    // Already started? Verify liveness to avoid stale or wedged state
    if (HANDLE h = helper_proc().get_process_handle(); h != nullptr) {
      BOOST_LOG(debug) << "Display helper: checking existing process handle...";
      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        DWORD pid = GetProcessId(h);
        BOOST_LOG(debug) << "Display helper already running (pid=" << pid << ")";
        if (!force_restart) {
          // Check IPC liveness with a lightweight ping; if responsive, reuse existing helper
          bool ping_ok = false;
          for (int i = 0; i < 2 && !ping_ok; ++i) {
            ping_ok = platf::display_helper_client::send_ping();
            if (!ping_ok) {
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
          }
          if (ping_ok) {
            return true;
          }
          platf::display_helper_client::reset_connection();
          BOOST_LOG(warning) << "Display helper process ping failed; keeping existing instance and deferring restart.";
          return false;
        }

        BOOST_LOG(warning) << "Display helper: hard restart requested; terminating existing instance (pid=" << pid
                           << ") with no grace period.";
        platf::display_helper_client::reset_connection();
        helper_proc().terminate();

        DWORD wait_result = WaitForSingleObject(h, kHelperForceKillWaitMs);
        if (wait_result == WAIT_OBJECT_0) {
          DWORD exit_code = 0;
          GetExitCodeProcess(h, &exit_code);
          BOOST_LOG(info) << "Display helper exited after forced termination (code=" << exit_code << ").";
        } else if (wait_result == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "Display helper: process did not exit within " << kHelperForceKillWaitMs
                             << " ms after termination request; continuing with cleanup.";
        } else {
          DWORD wait_err = GetLastError();
          BOOST_LOG(warning) << "Display helper: wait after termination failed (winerr=" << wait_err
                             << "); continuing with cleanup.";
        }

        // Small delay to reduce the chance of named pipe / mutex conflicts during rapid restart.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } else {
        // Process exited; fall through to restart
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        BOOST_LOG(debug) << "Display helper process detected as exited (code=" << exit_code << "); preparing restart.";
      }
    }
    if (shutting_down) {
      return false;
    }

    kill_all_helper_processes();

    // Compute path to luminalshine_display_helper.exe inside the tools subdirectory next to luminalshine.exe
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
      BOOST_LOG(error) << "Failed to resolve LuminalShine module path; cannot launch display helper.";
      return false;
    }
    std::filesystem::path exe_path(module_path);
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path helper = dir / L"tools" / L"luminalshine_display_helper.exe";

    if (!std::filesystem::exists(helper)) {
      BOOST_LOG(warning) << "Display helper not found at: " << platf::to_utf8(helper.wstring())
                         << ". Ensure the tools subdirectory is present and contains luminalshine_display_helper.exe.";
      return false;
    }

    const bool allow_system_fallback = platf::is_running_as_system() && !user_session_ready();
    BOOST_LOG(debug) << "Starting display helper: " << platf::to_utf8(helper.wstring());
    bool started = helper_proc().start(helper.wstring(), L"", allow_system_fallback);
    if (!started && force_restart) {
      // If we were asked to hard-restart, tolerate a brief overlap window where the old
      // instance is still tearing down and retry quickly.
      for (int attempt = 0; attempt < 5 && !started; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        started = helper_proc().start(helper.wstring(), L"", allow_system_fallback);
      }
    }
    if (!started) {
      BOOST_LOG(error) << "Failed to start display helper: " << platf::to_utf8(helper.wstring());
      return false;
    }

    HANDLE h = helper_proc().get_process_handle();
    if (!h) {
      BOOST_LOG(error) << "Display helper started but no process handle available";
      return false;
    }

    DWORD pid = GetProcessId(h);
    BOOST_LOG(info) << "Display helper successfully started (pid=" << pid << ")";

    // Give the helper process time to initialize and create its named pipe server
    // Check if it exits early (e.g., singleton mutex conflict from incomplete cleanup)
    for (int check = 0; check < 6; ++check) {
      DWORD wait = WaitForSingleObject(h, 50);
      if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 3) {
          BOOST_LOG(warning) << "Display helper exited immediately with code 3 (singleton conflict). "
                             << "Retrying after extended cleanup delay...";
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));

          const bool retry_started = helper_proc().start(helper.wstring(), L"", allow_system_fallback);
          if (!retry_started) {
            BOOST_LOG(error) << "Display helper retry start failed";
            return false;
          }
          h = helper_proc().get_process_handle();
          if (h) {
            pid = GetProcessId(h);
            BOOST_LOG(info) << "Display helper retry succeeded (pid=" << pid << ")";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
          }
          break;
        } else {
          BOOST_LOG(error) << "Display helper exited unexpectedly with code " << exit_code;
          return false;
        }
      }
    }

    // Final initialization delay for pipe server creation
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return wait_for_helper_ipc_ready_locked();
  }

  // Watchdog state for helper liveness during active streams
  static std::atomic<bool> g_watchdog_running {false};
  static std::jthread g_watchdog_thread;
  static std::chrono::steady_clock::time_point g_last_vd_reenable {};

  constexpr auto kVirtualDisplayReenableCooldown = std::chrono::seconds(3);

  bool recently_reenabled_virtual_display() {
    if (g_last_vd_reenable.time_since_epoch().count() == 0) {
      return false;
    }
    return (std::chrono::steady_clock::now() - g_last_vd_reenable) < kVirtualDisplayReenableCooldown;
  }

  [[maybe_unused]] void explicit_virtual_display_reset_and_apply(
    display_helper_integration::DisplayApplyBuilder &builder,
    const rtsp_stream::launch_session_t &session,
    std::function<bool(const display_helper_integration::DisplayApplyRequest &)> apply_fn
  ) {
    // Only act if virtual display is in play.
    if (!session.virtual_display && !builder.build().session_overrides.virtual_display_override.value_or(false)) {
      return;
    }

    // Debounce to avoid hammering the driver.
    if (recently_reenabled_virtual_display()) {
      return;
    }

    // First send a "blank" request to detach virtual display.
    display_helper_integration::DisplayApplyBuilder disable_builder;
    disable_builder.set_session(session);
    auto &overrides = disable_builder.mutable_session_overrides();
    overrides.virtual_display_override = false;
    disable_builder.set_action(display_helper_integration::DisplayApplyAction::Apply);
    auto disable_req = disable_builder.build();

    BOOST_LOG(info) << "Display helper: explicit virtual display disable before re-enable.";
    (void) apply_fn(disable_req);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Re-enable with the original builder intent.
    BOOST_LOG(info) << "Display helper: explicit virtual display re-enable after disappearance.";
    auto enable_req = builder.build();
    if (apply_fn(enable_req)) {
      g_last_vd_reenable = std::chrono::steady_clock::now();
    }
  }

  static void set_active_session(
    const rtsp_stream::launch_session_t &session,
    std::optional<std::string> device_id_override = std::nullopt,
    std::optional<int> fps_override = std::nullopt,
    std::optional<int> width_override = std::nullopt,
    std::optional<int> height_override = std::nullopt,
    std::optional<bool> virtual_display_override = std::nullopt,
    std::optional<int> framegen_refresh_override = std::nullopt
  ) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    const int effective_fps = fps_override ? *fps_override :
      (session.virtual_display ? session.fps :
       (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps));
    g_active_session_dd = session_dd_fields_t {
      .width = width_override ? *width_override : session.width,
      .height = height_override ? *height_override : session.height,
      .fps = effective_fps,
      .enable_hdr = session.enable_hdr,
      .enable_sops = session.enable_sops,
      .virtual_display = virtual_display_override ? *virtual_display_override : session.virtual_display,
      .virtual_display_device_id = device_id_override ? *device_id_override : session.virtual_display_device_id,
      .framegen_refresh_rate = framegen_refresh_override ? framegen_refresh_override : session.framegen_refresh_rate,
      .gen1_framegen_fix = session.gen1_framegen_fix,
      .gen2_framegen_fix = session.gen2_framegen_fix,
    };
  }

  [[maybe_unused]] static std::optional<session_dd_fields_t> get_active_session_copy() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd;
  }

  static void clear_active_session() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    g_active_session_dd.reset();
  }

  std::optional<std::string> build_helper_apply_payload(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper: no configuration provided for APPLY payload.";
      return std::nullopt;
    }

    bool ok = true;
    std::string json = display_device::toJson(*request.configuration, 0u, &ok);
    if (!ok) {
      BOOST_LOG(error) << "Display helper: failed to serialize configuration for helper APPLY payload.";
      return std::nullopt;
    }

    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded()) {
      BOOST_LOG(error) << "Display helper: failed to parse serialized configuration JSON for helper APPLY payload.";
      return std::nullopt;
    }

    if (request.attach_hdr_toggle_flag) {
      j["wa_hdr_toggle"] = true;
    }

    if (request.transitional_apply) {
      j["sunshine_transitional_apply"] = true;
    }

    if (request.dark_recovery_anchor) {
      j["sunshine_dark_recovery_anchor"] = true;
    }

    if (request.virtual_display_arrangement) {
      j["sunshine_virtual_layout"] = virtual_layout_to_string(*request.virtual_display_arrangement);
    }

    if (!request.topology.topology.empty()) {
      nlohmann::json topo = nlohmann::json::array();
      for (const auto &grp : request.topology.topology) {
        nlohmann::json group = nlohmann::json::array();
        for (const auto &id : grp) {
          group.push_back(id);
        }
        topo.push_back(std::move(group));
      }
      j["sunshine_topology"] = std::move(topo);
    }

    if (!request.topology.monitor_positions.empty()) {
      nlohmann::json positions = nlohmann::json::object();
      for (const auto &[device_id, point] : request.topology.monitor_positions) {
        positions[device_id] = {{"x", point.m_x}, {"y", point.m_y}};
      }
      j["sunshine_monitor_positions"] = std::move(positions);
    }

    if (!request.topology.device_refresh_rate_overrides.empty()) {
      nlohmann::json overrides = nlohmann::json::object();
      for (const auto &[device_id, rate] : request.topology.device_refresh_rate_overrides) {
        overrides[device_id] = {{"num", rate.first}, {"den", rate.second}};
      }
      j["sunshine_device_refresh_rate_overrides"] = std::move(overrides);
    }

    // Pass golden-first restore preference to helper
    if (config::video.dd.always_restore_from_golden) {
      j["sunshine_always_restore_from_golden"] = true;
    }

    return j.dump();
  }

  std::string build_revert_payload(bool prefer_golden_if_current_missing) {
    nlohmann::json j = nlohmann::json::object();
    if (prefer_golden_if_current_missing) {
      j["sunshine_prefer_golden_if_current_missing"] = true;
    } else {
      // Clean session shutdowns must restore the baseline captured directly
      // before this VGD session. Golden is a crash/missing-snapshot fallback,
      // not an excuse to replace a newer two-monitor layout.
      j["sunshine_clean_session_revert"] = true;
    }
    return j.dump();
  }

  static void watchdog_proc(std::stop_token st) {
    using namespace std::chrono_literals;
    constexpr auto kActiveInterval = 5s;
    constexpr auto kSuspendedInterval = 20s;
    bool helper_ready = false;

    while (!st.stop_requested()) {
      if (!dd_feature_enabled()) {
        if (helper_ready) {
          platf::display_helper_client::reset_connection();
          helper_ready = false;
        }
        for (auto slept = 0ms; slept < kActiveInterval && !st.stop_requested(); slept += 100ms) {
          std::this_thread::sleep_for(100ms);
        }
        continue;
      }

      if (!helper_ready) {
        helper_ready = ensure_helper_started();
        if (!helper_ready) {
          for (auto slept = 0ms; slept < kActiveInterval && !st.stop_requested(); slept += 100ms) {
            std::this_thread::sleep_for(100ms);
          }
          continue;
        }
        (void) platf::display_helper_client::send_ping();
      }

      const bool suspended = (rtsp_stream::session_count() == 0) && (proc::proc.running() > 0);
      const auto interval = suspended ? kSuspendedInterval : kActiveInterval;
      for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
        std::this_thread::sleep_for(100ms);
      }
      if (st.stop_requested()) {
        break;
      }

      if (!platf::display_helper_client::send_ping()) {
        // Avoid logging ping failures to reduce log spam; proceed to reconnect
        platf::display_helper_client::reset_connection();
        helper_ready = ensure_helper_started();
        if (!helper_ready) {
          continue;
        }
        // Do not re-apply automatically on reconnect; just confirm IPC is reachable.
        helper_ready = platf::display_helper_client::send_ping();
      }
    }
  }

}  // namespace

namespace display_helper_integration {
  namespace {
    bool apply_internal(const DisplayApplyRequest &request, bool allow_resolution_deferral) {
      g_last_apply_failure.store(ApplyFailure::none, std::memory_order_release);
      if (request.action == DisplayApplyAction::Skip) {
        BOOST_LOG(info) << "Display helper: configuration parse failed; not dispatching.";
        return false;
      }

      if (request.action == DisplayApplyAction::Revert) {
        const bool helper_ready = ensure_helper_started(false, true);
        if (!helper_ready) {
          BOOST_LOG(warning) << "Display helper: REVERT skipped (helper not reachable).";
          clear_active_session();
          return false;
        }
        BOOST_LOG(info) << "Display helper: sending REVERT request (builder).";
        const bool ok = platf::display_helper_client::send_revert();
        BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
        clear_active_session();
        return ok;
      }

      if (request.action != DisplayApplyAction::Apply) {
        return false;
      }

      // Prefer the helper for APPLY, even when running as SYSTEM without an interactive user session.
      // In-process display APIs frequently return ERROR_ACCESS_DENIED in that context.
      const bool system_no_user_session = platf::is_running_as_system() && !user_session_ready();
      if (system_no_user_session) {
        BOOST_LOG(debug) << "Display helper: SYSTEM context without user session; preferring helper dispatch.";
      }

      // Keep one helper alive across SNAPSHOT_CURRENT -> DISARM -> APPLY. Killing
      // it here discards the process that owns the snapshot and creates a fresh
      // pipe immediately before the longest SetDisplayConfig transaction.
      // Exception: if we recently asked the helper to restore and did not cancel it inside
      // the short disarm grace window, keep that helper alive and let APPLY supersede the
      // restore through IPC. Killing it here can strand the host in a partially restored
      // physical-display mode when a monitor input is still switched away.
      // An unresponsive helper is still restarted below; reuse applies only
      // when the existing process answers its liveness probe.
      const bool restore_expected = restore_expected_with_live_helper();
      const bool hard_restart = false;
      if (request.session && restore_expected) {
        BOOST_LOG(info) << "Display helper: reusing existing helper because an unconfirmed restore is pending; APPLY will supersede it.";
      }

      bool helper_ready = ensure_helper_started(hard_restart, true);
      if (!helper_ready) {
        BOOST_LOG(warning) << "Display helper: existing helper is not responsive; replacing it before APPLY.";
        helper_ready = ensure_helper_started(true, true);
      }

      if (helper_ready) {
        DisplayApplyRequest final_request = request;
        bool direct_ring_admitted = true;
        auto verification_configuration = request.configuration;
        if (request.session && request.session->virtual_display && verification_configuration &&
            request.session->fps > 0) {
          const auto previous_refresh = verification_configuration->m_refresh_rate;
          verification_configuration->m_refresh_rate = display_device::Rational {
            static_cast<unsigned int>(request.session->fps), 1u
          };
          final_request.configuration = verification_configuration;
          final_request.session_overrides.fps_override = request.session->fps;
          final_request.session_overrides.framegen_refresh_override.reset();
          BOOST_LOG(info) << "Display helper: canonical direct-VGD refresh requested="
                          << (previous_refresh ? refresh_rate_description(*previous_refresh) : "unset")
                          << " live=" << request.session->fps << "/1 source=client_fps"
                          << " framegen_metadata="
                          << (request.session->framegen_refresh_rate ?
                                std::to_string(*request.session->framegen_refresh_rate) : "unset")
                          << " legacy_override_ignored=true";
        }
        if (request.session && request.session->virtual_display && verification_configuration &&
            !verification_configuration->m_device_id.empty()) {
          // Newly created IddCx monitors enumerate inactive. Asking Windows to
          // activate that target, select its mode, and tear down every physical
          // path in one SetDisplayConfig transaction can block indefinitely.
          // First establish a simple extended/EnsureActive path; only after it
          // is observable do we request the exclusive client topology.
          display_device::SingleDisplayConfiguration activation_config;
          activation_config.m_device_id = verification_configuration->m_device_id;
          activation_config.m_device_prep =
            display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
          DisplayApplyRequest activation_request;
          activation_request.action = DisplayApplyAction::Apply;
          activation_request.configuration = activation_config;
          activation_request.transitional_apply = true;
          const auto activation_payload = build_helper_apply_payload(activation_request);
          if (!activation_payload) {
            g_last_apply_failure.store(ApplyFailure::virtual_display_activation, std::memory_order_release);
            return false;
          }

          BOOST_LOG(info) << "Display helper: stage 1/3 activating LuminalVGD alongside the physical topology.";
          const auto activation_outcome = platf::display_helper_client::send_apply_json(*activation_payload);
          bool activated = wait_for_device_activation(
            activation_config.m_device_id,
            std::chrono::seconds(10)
          );
          if (!activated) {
            log_observed_mode(activation_config, "stage 1/3");
            BOOST_LOG(error) << "Display helper: stage 1/3 failed; Windows did not activate the LuminalVGD target.";
            g_last_apply_failure.store(ApplyFailure::virtual_display_activation, std::memory_order_release);
            return false;
          }
          log_observed_mode(*verification_configuration, "stage 1/3");
          if (activation_outcome == platf::display_helper_client::ApplyOutcome::indeterminate) {
            BOOST_LOG(warning) << "Display helper: activation became observable after an indeterminate helper completion; restarting the helper before exclusive APPLY.";
            if (!ensure_helper_started(true, true)) {
              g_last_apply_failure.store(ApplyFailure::helper_unavailable, std::memory_order_release);
              return false;
            }
          } else if (activation_outcome == platf::display_helper_client::ApplyOutcome::rejected) {
            BOOST_LOG(error) << "Display helper: stage 1/3 activation was rejected despite transient device visibility.";
            g_last_apply_failure.store(ApplyFailure::virtual_display_activation, std::memory_order_release);
            return false;
          }
          // The driver normally activates directly at its EDID-preferred client
          // mode. Do not re-apply an already-correct mode (the former middle
          // stage waited on HDR and created the very race this sequence avoids).
          // If Windows selected a different core mode, correct resolution/rate
          // once while extended, explicitly excluding HDR from the gate.
          if (!wait_for_requested_core_mode(*verification_configuration, std::chrono::seconds(2))) {
            auto mode_config = *verification_configuration;
            mode_config.m_hdr_state.reset();
            mode_config.m_device_prep =
              display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
            DisplayApplyRequest mode_request;
            mode_request.action = DisplayApplyAction::Apply;
            mode_request.configuration = mode_config;
            mode_request.transitional_apply = true;
            const auto mode_payload = build_helper_apply_payload(mode_request);
            const auto mode_outcome = mode_payload ?
                                        platf::display_helper_client::send_apply_json(*mode_payload) :
                                        platf::display_helper_client::ApplyOutcome::rejected;
            if (mode_outcome == platf::display_helper_client::ApplyOutcome::rejected ||
                !wait_for_requested_core_mode(mode_config, std::chrono::seconds(10))) {
              log_observed_mode(mode_config, "stage 1/3 core-mode correction");
              BOOST_LOG(error) << "Display helper: stage 1/3 failed; requested resolution/refresh was not observed.";
              g_last_apply_failure.store(ApplyFailure::requested_mode, std::memory_order_release);
              return false;
            }
            if (mode_outcome == platf::display_helper_client::ApplyOutcome::indeterminate &&
                !ensure_helper_started(true, true)) {
              g_last_apply_failure.store(ApplyFailure::helper_unavailable, std::memory_order_release);
              return false;
            }
          } else {
            BOOST_LOG(info) << "Display helper: requested resolution/refresh already active; skipping redundant mode APPLY.";
          }

          BOOST_LOG(info) << "Display helper: stage 1/3 complete; entering a 2-second quiet settle before making LuminalVGD primary.";
          std::this_thread::sleep_for(std::chrono::seconds(2));

          display_device::SingleDisplayConfiguration primary_config;
          primary_config.m_device_id = verification_configuration->m_device_id;
          primary_config.m_device_prep =
            display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary;
          DisplayApplyRequest primary_request;
          primary_request.action = DisplayApplyAction::Apply;
          primary_request.configuration = primary_config;
          primary_request.transitional_apply = true;
          const auto primary_payload = build_helper_apply_payload(primary_request);
          BOOST_LOG(info) << "Display helper: stage 2/3 making LuminalVGD primary while retaining the physical topology.";
          const auto primary_outcome = primary_payload ?
                                         platf::display_helper_client::send_apply_json(*primary_payload) :
                                         platf::display_helper_client::ApplyOutcome::rejected;
          if (primary_outcome == platf::display_helper_client::ApplyOutcome::rejected ||
              !wait_for_requested_core_mode(*verification_configuration, std::chrono::seconds(10))) {
            log_observed_mode(*verification_configuration, "stage 2/3");
            BOOST_LOG(error) << "Display helper: stage 2/3 failed; LuminalVGD did not remain capture-ready while becoming primary.";
            g_last_apply_failure.store(ApplyFailure::requested_mode, std::memory_order_release);
            return false;
          }
          BOOST_LOG(info) << "Display helper: stage 2/3 complete; retaining both displays for a 3-second capture-ready settle.";
          std::this_thread::sleep_for(std::chrono::seconds(3));

          const auto pre_exclusive_ring = VDISPLAY::vgd::begin_planned_modeset();
          (void) apply_best_effort_hdr_now(
            verification_configuration->m_device_id,
            verification_configuration->m_hdr_state
          );
          if (!pre_exclusive_ring ||
              !VDISPLAY::vgd::wait_for_planned_modeset(*pre_exclusive_ring, std::chrono::seconds(12))) {
            direct_ring_admitted = false;
            if (pre_exclusive_ring) {
              platf::dxgi::mark_vgd_ring_broken(pre_exclusive_ring->session_id);
            }
            BOOST_LOG(warning) << "Display helper: LuminalVGD did not publish a pre-exclusive frame; "
                                  "retaining VGD-primary extended topology and selecting HDR-capable fallback capture.";
            final_request.configuration = primary_config;
            final_request.virtual_display_arrangement = VirtualDisplayArrangement::ExtendedPrimary;
          } else {
            BOOST_LOG(info) << "Display helper: pre-exclusive LuminalVGD frame published after final HDR state; direct capture admitted.";
            auto exclusive_config = *verification_configuration;
            exclusive_config.m_resolution.reset();
            exclusive_config.m_refresh_rate.reset();
            exclusive_config.m_hdr_state.reset();
            final_request.configuration = std::move(exclusive_config);
          }
        }

        auto payload = build_helper_apply_payload(final_request);
        if (!payload) {
          BOOST_LOG(error) << "Display helper: failed to build APPLY payload for helper dispatch.";
          return false;
        }

        BOOST_LOG(info) << (direct_ring_admitted ?
          "Display helper: stage 3/3 committing topology-only exclusive APPLY via helper." :
          "Display helper: stage 3/3 committing safe VGD-primary fallback topology via helper.");
        const auto ring_before = direct_ring_admitted && request.session && request.session->virtual_display ?
                                   VDISPLAY::vgd::begin_planned_modeset() : std::nullopt;
        const auto outcome = platf::display_helper_client::send_apply_json(*payload);
        bool ok = outcome == platf::display_helper_client::ApplyOutcome::applied;
        if (outcome == platf::display_helper_client::ApplyOutcome::indeterminate &&
            verification_configuration && request.session) {
          BOOST_LOG(info) << "Display helper: APPLY acknowledgement was indeterminate; "
                             "verifying the requested Windows mode before declaring failure.";
          ok = request.session->virtual_display ?
                 wait_for_requested_core_mode(*verification_configuration, std::chrono::seconds(30)) :
                 wait_for_requested_mode(*verification_configuration, kTopologyWaitTimeout);
          if (ok) {
            BOOST_LOG(info) << "Display helper: requested mode is active despite the missing APPLY acknowledgement.";
            // The old pipe may still contain a late acknowledgement. Reconnect
            // before the next command so it cannot be mistaken for that reply.
            platf::display_helper_client::reset_connection();
          }
        }
        if (ok && verification_configuration && request.session &&
            !(request.session->virtual_display ?
                wait_for_requested_core_mode(*verification_configuration, std::chrono::seconds(30)) :
                wait_for_requested_mode(*verification_configuration, kTopologyWaitTimeout))) {
          BOOST_LOG(error) << "Display helper: APPLY was accepted, but mandatory requested-mode verification failed.";
          ok = false;
        }
        if (verification_configuration && request.session) {
          log_observed_mode(*verification_configuration, "stage 3/3");
        }
        if (ok && ring_before) {
          BOOST_LOG(info) << "Display helper: Windows accepted the planned modeset; waiting for "
                             "the LuminalVGD ring to settle ACTIVE before capture starts.";
          if (!VDISPLAY::vgd::wait_for_planned_modeset(*ring_before, std::chrono::seconds(8))) {
            BOOST_LOG(error) << "Display helper: requested mode is active, but the LuminalVGD ring "
                                "did not settle ACTIVE after the planned modeset.";
            ok = false;
            g_last_apply_failure.store(ApplyFailure::capture_ring, std::memory_order_release);
          } else {
            BOOST_LOG(info) << "Display helper: LuminalVGD ring is ACTIVE after the planned modeset.";
          }
        }
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok && request.session) {
          g_restore_expected.store(false, std::memory_order_relaxed);
          g_last_apply_completed_us.store(now_steady_us(), std::memory_order_relaxed);
          set_active_session(
            *request.session,
            final_request.session_overrides.device_id_override,
            final_request.session_overrides.fps_override,
            final_request.session_overrides.width_override,
            final_request.session_overrides.height_override,
            final_request.session_overrides.virtual_display_override,
            final_request.session_overrides.framegen_refresh_override
          );
          if (request.enable_virtual_display_watchdog) {
            platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
          }
        }
        if (!ok && allow_resolution_deferral && request.session && platf::is_lock_screen_active()) {
          BOOST_LOG(info) << "Display helper: APPLY failed during lock screen; queuing deferred apply for retry after unlock.";
          queue_deferred_resolution_apply(request);
        }
        if (!ok && g_last_apply_failure.load(std::memory_order_acquire) == ApplyFailure::none) {
          g_last_apply_failure.store(ApplyFailure::requested_mode, std::memory_order_release);
        }
        return ok;
      }

      if (system_no_user_session) {
        BOOST_LOG(warning) << "Display helper: helper unavailable in SYSTEM context without user session; skipping in-process APPLY fallback.";
        maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
        return false;
      }

      BOOST_LOG(warning) << "Display helper: helper unavailable; falling back to in-process APPLY.";

      if (!request.session) {
        BOOST_LOG(error) << "Display helper: missing session context for in-process APPLY.";
        return false;
      }

      const auto apply_result = apply_in_process(request);
      if (apply_result != display_device::SettingsManagerInterface::ApplyResult::Ok) {
        if (apply_result == display_device::SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
          maybe_queue_deferred_resolution_apply_on_api_unavailable(request);
        }
        BOOST_LOG(warning) << "Display helper: in-process APPLY failed.";
        return false;
      }

      const auto device_id = request.configuration ? request.configuration->m_device_id : std::string {};
      if (!verify_helper_topology(*request.session, device_id)) {
        BOOST_LOG(error) << "Display helper: topology verification failed after in-process APPLY.";
        return false;
      }
      (void) apply_topology_definition(request.topology, "in-process");

      if (request.session->virtual_display) {
        const auto ring_before = VDISPLAY::vgd::begin_planned_modeset();
        if (!ring_before || !VDISPLAY::vgd::wait_for_planned_modeset(*ring_before, std::chrono::seconds(8))) {
          BOOST_LOG(error) << "Display helper: in-process APPLY did not produce a stable, publishing LuminalVGD ring.";
          return false;
        }
      }

      g_last_apply_completed_us.store(now_steady_us(), std::memory_order_relaxed);
      set_active_session(
        *request.session,
        request.session_overrides.device_id_override,
        request.session_overrides.fps_override,
        request.session_overrides.width_override,
        request.session_overrides.height_override,
        request.session_overrides.virtual_display_override,
        request.session_overrides.framegen_refresh_override
      );
      if (request.enable_virtual_display_watchdog) {
        platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
      }
      maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
      return true;
    }
  }  // namespace

  bool apply(const DisplayApplyRequest &request) {
    return apply_internal(request, true);
  }

  bool revert(bool prefer_golden_if_current_missing) {
    clear_pending_apply();
    const auto completed_us = g_last_revert_completed_us.load(std::memory_order_acquire);
    if (!prefer_golden_if_current_missing && completed_us > 0 &&
        now_steady_us() - completed_us < 5'000'000 && !helper_process_running()) {
      BOOST_LOG(info) << "Display helper: suppressing duplicate REVERT because the prior transactional restore just completed.";
      return true;
    }
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot send revert.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending REVERT request"
                    << (prefer_golden_if_current_missing ? " (prefer golden if current missing)." : ".");
    const bool ok = platf::display_helper_client::send_revert(build_revert_payload(prefer_golden_if_current_missing));
    BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
    if (ok) {
      g_restore_expected.store(true, std::memory_order_relaxed);
      g_last_revert_us.store(now_steady_us(), std::memory_order_relaxed);
      g_restore_generation.fetch_add(1, std::memory_order_relaxed);
    }
    clear_active_session();
    return ok;
  }

  bool wait_for_revert_completion(std::chrono::milliseconds timeout) {
    HANDLE process = nullptr;
    {
      std::lock_guard<std::mutex> lg(helper_mutex());
      process = helper_proc().get_process_handle();
      if (!process) {
        BOOST_LOG(error) << "Display helper: cannot wait for REVERT completion without a helper process handle.";
        return false;
      }
      const auto bounded = std::clamp<long long>(timeout.count(), 0, 120000);
      const DWORD waited = WaitForSingleObject(process, static_cast<DWORD>(bounded));
      if (waited != WAIT_OBJECT_0) {
        BOOST_LOG(error) << "Display helper: timed out waiting for strictly verified REVERT completion.";
        return false;
      }
    }
    g_restore_expected.store(false, std::memory_order_release);
    g_last_revert_completed_us.store(now_steady_us(), std::memory_order_release);
    platf::display_helper_client::reset_connection();
    BOOST_LOG(info) << "Display helper: transactional REVERT completed and helper exited after verification.";
    return true;
  }

  ApplyFailure last_apply_failure() {
    return g_last_apply_failure.load(std::memory_order_acquire);
  }

  const char *apply_failure_message(ApplyFailure failure) {
    switch (failure) {
      case ApplyFailure::virtual_display_activation:
        return "Windows did not activate the virtual display.";
      case ApplyFailure::requested_mode:
        return "Windows did not apply the requested virtual-display mode.";
      case ApplyFailure::capture_ring:
        return "The virtual display activated, but its capture ring did not begin publishing frames.";
      case ApplyFailure::helper_unavailable:
        return "The Windows display helper was unavailable.";
      case ApplyFailure::none:
      default:
        return "Display configuration failed.";
    }
  }

  bool restart_helper_for_recovery() {
    platf::display_helper_client::reset_connection();
    return ensure_helper_started(true, true);
  }

  bool ensure_physical_display_active() {
    if (VDISPLAY::has_active_physical_display()) {
      return true;
    }
    const auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }
    for (const auto &device : *devices) {
      if (device.m_device_id.empty() || VDISPLAY::is_virtual_display_enumerated_device(device)) {
        continue;
      }
      display_device::SingleDisplayConfiguration config;
      config.m_device_id = device.m_device_id;
      config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
      DisplayApplyRequest request;
      request.action = DisplayApplyAction::Apply;
      request.configuration = config;
      BOOST_LOG(warning) << "Display helper recovery: activating physical display '"
                         << (device.m_friendly_name.empty() ? device.m_device_id : device.m_friendly_name)
                         << "' alongside the virtual display.";
      return apply(request);
    }
    return false;
  }

  bool request_wddm_reset_recovery() {
    static std::atomic<std::int64_t> last_reset_ms {0};
    constexpr std::int64_t kResetCooldownMs = 15LL * 60LL * 1000LL;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    auto previous = last_reset_ms.load(std::memory_order_acquire);
    if (previous != 0 && now_ms - previous < kResetCooldownMs) {
      BOOST_LOG(debug) << "Display helper: WDDM reset suppressed by the 15-minute safety cooldown.";
      return false;
    }
    if (!last_reset_ms.compare_exchange_strong(previous, now_ms, std::memory_order_acq_rel)) {
      return false;
    }
    if (!ensure_helper_started(false, true)) {
      BOOST_LOG(error) << "Display helper: cannot request WDDM reset because the interactive helper is unavailable.";
      return false;
    }
    BOOST_LOG(warning) << "Display helper: requesting one rate-limited WDDM reset (Ctrl+Win+Shift+B).";
    return platf::display_helper_client::send_wddm_reset();
  }

  bool disarm_pending_restore() {
    return disarm_helper_restore_if_running();
  }

  bool export_golden_restore() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot export golden snapshot.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending EXPORT_GOLDEN request.";
    const bool ok = platf::display_helper_client::send_export_golden(build_snapshot_exclude_payload());
    BOOST_LOG(info) << "Display helper: EXPORT_GOLDEN dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool reset_persistence() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot reset persistence.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending RESET request.";
    const bool ok = platf::display_helper_client::send_reset();
    BOOST_LOG(info) << "Display helper: RESET dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool snapshot_current_display_state() {
    if (restore_expected_with_live_helper()) {
      BOOST_LOG(info) << "Display helper: skipping SNAPSHOT_CURRENT while an unconfirmed restore is pending.";
      return false;
    }

    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot snapshot current display state.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending SNAPSHOT_CURRENT request.";
    const bool ok = platf::display_helper_client::send_snapshot_current(build_snapshot_exclude_payload());
    BOOST_LOG(info) << "Display helper: SNAPSHOT_CURRENT dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool apply_pending_if_ready() {
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        return false;
      }
    }

    if (platf::is_running_as_system() && !user_session_ready()) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    PendingApplyState pending;
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        return false;
      }
      auto &state = *pending_apply_state();
      if (!state.ready_since) {
        state.ready_since = now;
        state.next_attempt = now + kDeferredApplyInitialDelay;
        BOOST_LOG(info) << "Display helper: user session detected; delaying deferred APPLY for "
                        << kDeferredApplyInitialDelay.count() << "ms.";
        return false;
      }
      if (now < state.next_attempt) {
        return false;
      }
      if (state.attempts >= kMaxDeferredApplyAttempts) {
        BOOST_LOG(warning) << "Display helper: deferred APPLY exceeded retry limit; giving up on session "
                           << state.session_id << ".";
        pending_apply_state().reset();
        return false;
      }
      pending = state;
      pending_apply_state().reset();
    }

    std::optional<rtsp_stream::launch_session_t> session;
    if (pending.has_session) {
      rtsp_stream::launch_session_t snapshot {};
      snapshot.width = pending.session_snapshot.width;
      snapshot.height = pending.session_snapshot.height;
      snapshot.fps = pending.session_snapshot.fps;
      snapshot.enable_hdr = pending.session_snapshot.enable_hdr;
      snapshot.enable_sops = pending.session_snapshot.enable_sops;
      snapshot.virtual_display = pending.session_snapshot.virtual_display;
      snapshot.virtual_display_device_id = pending.session_snapshot.virtual_display_device_id;
      snapshot.virtual_display_ready_since = pending.session_snapshot.virtual_display_ready_since;
      snapshot.framegen_refresh_rate = pending.session_snapshot.framegen_refresh_rate;
      snapshot.gen1_framegen_fix = pending.session_snapshot.gen1_framegen_fix;
      snapshot.gen2_framegen_fix = pending.session_snapshot.gen2_framegen_fix;
      session = std::move(snapshot);
      pending.request.session = &*session;
    } else {
      pending.request.session = nullptr;
    }

    BOOST_LOG(info) << "Display helper: applying deferred configuration for session " << pending.session_id << ".";
    const bool ok = apply_internal(pending.request, false);
    if (!ok) {
      pending.attempts += 1;
      pending.request.session = nullptr;
      const auto delay = deferred_apply_retry_delay(pending.attempts);
      pending.next_attempt = std::chrono::steady_clock::now() + delay;
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        pending_apply_state() = pending;
        BOOST_LOG(warning) << "Display helper: deferred APPLY failed; retrying in "
                           << delay.count() << "ms (attempt " << pending.attempts
                           << "/" << kMaxDeferredApplyAttempts << ").";
      } else {
        BOOST_LOG(info) << "Display helper: deferred APPLY failed but a newer pending configuration is queued; dropping retry.";
      }
    }
    return ok;
  }

  bool has_pending_apply() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    return pending_apply_state().has_value();
  }

  void clear_pending_apply() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    pending_apply_state().reset();
  }

  int64_t ms_since_last_apply() {
    const auto last_us = g_last_apply_completed_us.load(std::memory_order_relaxed);
    if (last_us == 0) {
      return std::numeric_limits<int64_t>::max();
    }
    const auto elapsed_us = now_steady_us() - last_us;
    return elapsed_us / 1000;
  }

  namespace {
    constexpr double kEdidRefreshToleranceHz = 0.5;

    struct ParsedEdidRefreshInfo {
      bool present {false};
      std::optional<int> max_vertical_hz;
      double max_timing_hz {0.0};
    };

    void consider_timing(double hz, ParsedEdidRefreshInfo &out) {
      if (!std::isfinite(hz) || hz <= 0.0) {
        return;
      }
      if (hz > out.max_timing_hz) {
        out.max_timing_hz = hz;
      }
    }

    void parse_detailed_descriptor(const uint8_t *descriptor, ParsedEdidRefreshInfo &out) {
      if (!descriptor) {
        return;
      }

      const uint16_t pixel_clock = static_cast<uint16_t>(descriptor[0] | (static_cast<uint16_t>(descriptor[1]) << 8));
      if (pixel_clock == 0) {
        if (descriptor[3] == 0xFD) {
          const int max_vertical = static_cast<int>(descriptor[6]);
          if (max_vertical > 0 && max_vertical < 2000) {
            if (!out.max_vertical_hz || max_vertical > *out.max_vertical_hz) {
              out.max_vertical_hz = max_vertical;
            }
          }
        }
        return;
      }

      const uint16_t h_active = static_cast<uint16_t>(descriptor[2] | (static_cast<uint16_t>(descriptor[4] & 0xF0) << 4));
      const uint16_t h_blanking = static_cast<uint16_t>(descriptor[3] | (static_cast<uint16_t>(descriptor[4] & 0x0F) << 8));
      const uint16_t v_active = static_cast<uint16_t>(descriptor[5] | (static_cast<uint16_t>(descriptor[7] & 0xF0) << 4));
      const uint16_t v_blanking = static_cast<uint16_t>(descriptor[6] | (static_cast<uint16_t>(descriptor[7] & 0x0F) << 8));
      const uint32_t h_total = static_cast<uint32_t>(h_active) + static_cast<uint32_t>(h_blanking);
      const uint32_t v_total = static_cast<uint32_t>(v_active) + static_cast<uint32_t>(v_blanking);
      if (h_total == 0 || v_total == 0) {
        return;
      }

      const double pixel_clock_hz = static_cast<double>(pixel_clock) * 10000.0;
      double refresh_hz = pixel_clock_hz / (static_cast<double>(h_total) * static_cast<double>(v_total));
      if ((descriptor[17] & 0x80) != 0) {
        refresh_hz *= 2.0;
      }

      consider_timing(refresh_hz, out);
    }

    ParsedEdidRefreshInfo parse_edid_refresh(const std::vector<std::byte> &edid) {
      ParsedEdidRefreshInfo info;
      if (edid.empty()) {
        return info;
      }
      info.present = true;
      if (edid.size() < 128) {
        return info;
      }

      const auto *bytes = reinterpret_cast<const uint8_t *>(edid.data());
      const auto parse_block_descriptors = [&](const uint8_t *block, std::size_t start, std::size_t end) {
        if (!block || start >= end) {
          return;
        }
        for (std::size_t offset = start; offset + 17 < end; offset += 18) {
          parse_detailed_descriptor(block + offset, info);
        }
      };

      parse_block_descriptors(bytes, 54, 126);

      const std::size_t block_count = edid.size() / 128;
      const uint8_t extension_count = bytes[126];
      const std::size_t max_extensions = std::min<std::size_t>(extension_count, block_count > 0 ? block_count - 1 : 0);
      for (std::size_t idx = 0; idx < max_extensions; ++idx) {
        const std::size_t block_start = (idx + 1) * 128;
        if (block_start + 128 > edid.size()) {
          break;
        }
        const auto *ext = bytes + block_start;
        if (ext[0] == 0x02) {
          const uint8_t dtd_offset = ext[2];
          if (dtd_offset >= 4 && dtd_offset < 127) {
            const std::size_t start = block_start + dtd_offset;
            const std::size_t end = block_start + 127;
            for (std::size_t offset = start; offset + 17 < end; offset += 18) {
              parse_detailed_descriptor(bytes + offset, info);
            }
          }
        }
      }

      return info;
    }

    std::vector<std::byte> read_edid_for_device_id(const std::string &device_id) {
      if (device_id.empty()) {
        return {};
      }
      try {
        display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto display_data = api->queryDisplayConfig(display_device::QueryType::All);
        if (!display_data) {
          return {};
        }

        auto source_data = display_device::win_utils::collectSourceDataForMatchingPaths(*api, display_data->m_paths);
        auto it = source_data.find(device_id);
        if (it == source_data.end()) {
          for (const auto &entry : source_data) {
            if (boost::iequals(entry.first, device_id)) {
              it = source_data.find(entry.first);
              break;
            }
          }
        }

        if (it == source_data.end() || it->second.m_source_id_to_path_index.empty()) {
          return {};
        }

        const UINT32 source_id = it->second.m_active_source.value_or(it->second.m_source_id_to_path_index.begin()->first);
        const auto path_it = it->second.m_source_id_to_path_index.find(source_id);
        if (path_it == it->second.m_source_id_to_path_index.end()) {
          return {};
        }

        const std::size_t path_index = path_it->second;
        if (path_index >= display_data->m_paths.size()) {
          return {};
        }

        const auto &path = display_data->m_paths[path_index];
        return api->getEdid(path);
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << ": " << ex.what();
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << " due to unknown error.";
      }

      return {};
    }

    std::optional<display_device::EnumeratedDevice> find_device_for_hint(const std::string &hint) {
      if (hint.empty()) {
        return std::nullopt;
      }

      auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return std::nullopt;
      }

      for (const auto &device : *devices) {
        if (device_id_equals_ci(device.m_device_id, hint) || device_id_equals_ci(device.m_display_name, hint) ||
            device_id_equals_ci(device.m_friendly_name, hint)) {
          return device;
        }
      }

      return std::nullopt;
    }
  }  // namespace

  std::optional<FramegenEdidSupportResult> framegen_edid_refresh_support(
    const std::string &device_hint,
    const std::vector<int> &targets_hz
  ) {
    const auto resolved_device = find_device_for_hint(device_hint);
    if (!resolved_device) {
      return std::nullopt;
    }

    FramegenEdidSupportResult result;
    result.device_id = resolved_device->m_device_id;
    if (!resolved_device->m_friendly_name.empty()) {
      result.device_label = resolved_device->m_friendly_name;
    } else if (!resolved_device->m_display_name.empty()) {
      result.device_label = resolved_device->m_display_name;
    } else {
      result.device_label = resolved_device->m_device_id;
    }

    const auto edid_bytes = read_edid_for_device_id(result.device_id);
    const auto parsed = parse_edid_refresh(edid_bytes);
    result.edid_present = parsed.present;
    if (parsed.max_vertical_hz) {
      result.max_vertical_hz = parsed.max_vertical_hz;
    }
    if (parsed.max_timing_hz > 0.0) {
      result.max_timing_hz = parsed.max_timing_hz;
    }

    for (int hz : targets_hz) {
      FramegenEdidTargetSupport target {};
      target.hz = hz;
      if (!parsed.present || edid_bytes.empty()) {
        target.supported = std::nullopt;
        target.method = "unknown";
      } else if (parsed.max_vertical_hz && static_cast<double>(*parsed.max_vertical_hz) + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0 && parsed.max_timing_hz + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "timing";
      } else if (parsed.max_vertical_hz) {
        target.supported = false;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0) {
        target.supported = false;
        target.method = "timing";
      } else {
        target.supported = std::nullopt;
        target.method = "unknown";
      }
      result.targets.push_back(std::move(target));
    }

    return result;
  }

  namespace {
    /// Streak counter and its start timestamp are one logical unit: updating them
    /// as two independent atomics lets a success/failure interleave zero the
    /// counter while leaving a live timestamp stranded, after which the "and
    /// thirty seconds" half of the gate below is measured against an anchor that
    /// never advances again. One small mutex keeps them consistent; this is not
    /// a hot path (one enumeration has already happened by the time we get here).
    std::mutex g_enumeration_state_mutex;
    /// Consecutive enumerate_devices() calls that came back with nothing.
    std::uint64_t g_empty_enumerations = 0;
    /// steady_clock ms at which the current failing streak began; 0 == not failing.
    std::int64_t g_enumeration_failing_since_ms = 0;
    /// steady_clock ms of the last confirm probe, so its 2 s sleep runs rarely.
    std::atomic<std::int64_t> g_last_stack_probe_ms {0};

    constexpr std::uint64_t kEmptyEnumerationsBeforeProbe = 20;
    constexpr std::int64_t kMinFailingMsBeforeProbe = 30'000;
    constexpr std::int64_t kMinMsBetweenStackProbes = 60'000;

    /// Degraded ("API answers, zero paths") is reported far sooner than the
    /// terminal verdict, because it is the state recovery can still act in.
    constexpr std::uint64_t kEmptyEnumerationsBeforeDegradedProbe = 5;
    constexpr std::int64_t kMinMsBetweenDegradedProbes = 30'000;
    /// steady_clock ms of the last degraded probe.
    std::atomic<std::int64_t> g_last_degraded_probe_ms {0};
    std::atomic<std::int64_t> g_enumeration_retry_after_ms {0};

    std::int64_t steady_ms_now() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
      )
        .count();
    }

    /**
     * @brief Escalate a sustained enumeration blackout to the terminal stack-down verdict.
     *
     * Until this existed, the only routes to tdr::mark_stack_down ran off
     * D3D11CreateDevice exhaustion (display_base.cpp) and the session-start
     * encoder probe (stream.cpp). Both need something to be actively trying to
     * create a device, so a wedge with no stream running was invisible: during
     * the 2026-07-29 20:22 incident the host spun ~12,500 QueryDisplayConfig
     * give-up cycles over thirty minutes and never once latched. /api/health/tdr
     * reported stack_down=false the entire time, so the Troubleshooting page
     * told the user nothing was wrong while nothing worked.
     *
     * The bar is deliberately high, because latching refuses new sessions:
     * twenty consecutive empty enumerations AND thirty seconds of continuous
     * failure before the confirm probe is even considered, then
     * display_stack_confirmed_down()'s own two ERROR_NOT_SUPPORTED reads two
     * seconds apart. Anything short of the machine-wide wedge signature
     * (ERROR_GEN_FAILURE, ACCESS_DENIED, a healthy API reporting zero paths)
     * does not latch. A machine that recovers clears the latch by itself:
     * tdr::stack_down() re-probes every five seconds through the recheck hook
     * installed by tdr_lifeboat, and note_stack_healthy() un-latches.
     *
     * The probe sleeps two seconds internally and is called from a 150 ms poll
     * loop, so it is additionally rate-limited to once a minute.
     */
    void note_enumeration_result(bool produced_devices) {
      const auto now_ms = steady_ms_now();
      std::uint64_t streak = 0;
      std::int64_t failing_since = 0;
      {
        std::lock_guard lg {g_enumeration_state_mutex};
        if (produced_devices) {
          g_empty_enumerations = 0;
          g_enumeration_failing_since_ms = 0;
          return;
        }
        if (g_empty_enumerations == 0) {
          g_enumeration_failing_since_ms = now_ms;
        }
        streak = ++g_empty_enumerations;
        failing_since = g_enumeration_failing_since_ms;
      }

      if (tdr::stack_down()) {
        return;  // already latched; nothing to add
      }

      const bool active_stream = get_active_session_copy().has_value();
      if (active_stream && streak == 1) {
        LONG qdc_status = ERROR_SUCCESS;
        UINT32 qdc_paths = 0;
        (void) platf::dxgi::display_config_api_healthy(&qdc_status, &qdc_paths);
        if (qdc_status == ERROR_NOT_SUPPORTED) {
          const bool first = gpu_recovery_policy::open_d3d11_circuit();
          BOOST_LOG(error) << "Display API refused the first active-stream enumeration (status "
                           << qdc_status << ", " << qdc_paths
                           << " paths). Cancelling GPU submissions immediately; terminal confirmation "
                              "continues in the background.";
          if (first) {
            BOOST_LOG(warning) << "GPU recovery circuit: native D3D12 NVENC disabled for this host run.";
          }
          tdr::mark_event(
            tdr::source_t::query_display_config,
            static_cast<long>(qdc_status),
            "active-stream QueryDisplayConfig became unavailable; GPU work cancelled before terminal classification"
          );
        }
      }

      // Degraded check first: it fires ~8 s before the terminal one and is the
      // only signal available while recovery can still work. Purely a report --
      // it must not reach mark_stack_down (tells the user to reboot) or
      // mark_event (sets recovery_recent, which briefly refuses sessions),
      // because a clean exclusive teardown legitimately has zero active paths
      // for 2-4.5 s and would otherwise refuse the next session.
      if (streak >= kEmptyEnumerationsBeforeDegradedProbe && streak < kEmptyEnumerationsBeforeProbe) {
        auto last_degraded = g_last_degraded_probe_ms.load(std::memory_order_acquire);
        if ((last_degraded == 0 || (now_ms - last_degraded) >= kMinMsBetweenDegradedProbes) &&
            g_last_degraded_probe_ms.compare_exchange_strong(last_degraded, now_ms, std::memory_order_acq_rel)) {
          LONG deg_status = ERROR_SUCCESS;
          UINT32 deg_paths = 0;
          if (platf::dxgi::display_stack_degraded_zero_paths(&deg_status, &deg_paths)) {
            BOOST_LOG(warning) << "DISPLAY STACK DEGRADED: the display API is answering normally but reports ZERO "
                                  "active paths, after "
                               << streak << " consecutive empty enumerations. Nothing is attached to compose onto. If a virtual "
                                  "display should be active this is the window in which recovery can still succeed -- "
                                  "the API stops answering entirely a few seconds later. This is a report, not a "
                                  "verdict: a clean exclusive-layout teardown also passes through this state.";
          }
        }
      }

      const auto required_streak = active_stream ? std::uint64_t {3} : kEmptyEnumerationsBeforeProbe;
      const auto required_failing_ms = active_stream ? std::int64_t {3'000} : kMinFailingMsBeforeProbe;
      if (streak < required_streak ||
          (now_ms - failing_since) < required_failing_ms) {
        return;
      }

      auto last_probe = g_last_stack_probe_ms.load(std::memory_order_acquire);
      if (last_probe != 0 && (now_ms - last_probe) < kMinMsBetweenStackProbes) {
        return;
      }
      if (!g_last_stack_probe_ms.compare_exchange_strong(last_probe, now_ms, std::memory_order_acq_rel)) {
        return;  // another thread is probing
      }

      LONG qdc_status = ERROR_SUCCESS;
      UINT32 qdc_paths = 0;
      if (!platf::dxgi::display_stack_confirmed_down(&qdc_status, &qdc_paths)) {
        BOOST_LOG(warning) << "Display enumeration has returned no devices for " << streak
                           << " consecutive calls over " << ((now_ms - failing_since) / 1000)
                           << "s, but the display API did not confirm the machine-wide wedge signature"
                              " (status "
                           << qdc_status << ", " << qdc_paths
                           << " paths). Not latching a terminal verdict; will re-check.";
        return;
      }

      std::string detail = "display enumeration returned no devices for ";
      detail += std::to_string(streak);
      detail += " consecutive calls over ";
      detail += std::to_string((now_ms - failing_since) / 1000);
      detail += "s; QueryDisplayConfig unavailable (status ";
      detail += std::to_string(qdc_status);
      detail += ", ";
      detail += std::to_string(qdc_paths);
      detail += " paths, confirmed twice)";

      BOOST_LOG(fatal) << "DISPLAY STACK UNAVAILABLE MACHINE-WIDE: " << detail
                       << ". No display can be configured or captured by any program until this clears."
                          " First try Win+Ctrl+Shift+B to restart the Windows graphics stack; if displays"
                          " do not return, this machine must be REBOOTED.";
      tdr::mark_stack_down(static_cast<long>(qdc_status), std::move(detail));
    }
  }  // namespace

  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  ) {
    const auto now_ms = steady_ms_now();
    if (now_ms < g_enumeration_retry_after_ms.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      auto devices = dd.enumAvailableDevices(detail);
      // enumAvailableDevices returns an ENGAGED but EMPTY list when the
      // underlying QueryDisplayConfig fails, so "did it throw" is not the
      // signal -- "did it produce anything" is.
      note_enumeration_result(!devices.empty());
      if (devices.empty()) {
        LONG qdc_status = ERROR_SUCCESS;
        UINT32 qdc_paths = 0;
        (void) platf::dxgi::display_config_api_healthy(&qdc_status, &qdc_paths);
        if (qdc_status == ERROR_NOT_SUPPORTED) {
          g_enumeration_retry_after_ms.store(now_ms + 1'000, std::memory_order_release);
        }
      } else {
        g_enumeration_retry_after_ms.store(0, std::memory_order_release);
      }
      return devices;
    } catch (...) {
      note_enumeration_result(false);
      return std::nullopt;
    }
  }

  std::optional<std::vector<std::vector<std::string>>> capture_current_topology() {
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.getCurrentTopology();
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail) {
    auto devices = enumerate_devices(detail);
    if (!devices) {
      return "[]";
    }
    if (detail == display_device::DeviceEnumerationDetail::Minimal) {
      devices->erase(
        std::remove_if(
          devices->begin(),
          devices->end(),
          [](const display_device::EnumeratedDevice &device) {
            return !device.m_info.has_value();
          }
        ),
        devices->end()
      );
    }
    return display_device::toJson(*devices);
  }

  void start_watchdog() {
    if (g_watchdog_running.exchange(true, std::memory_order_acq_rel)) {
      return;  // already running
    }
    g_watchdog_thread = std::jthread(watchdog_proc);
  }

  void stop_watchdog() {
    if (!g_watchdog_running.exchange(false, std::memory_order_acq_rel)) {
      return;  // not running
    }
    if (g_watchdog_thread.joinable()) {
      g_watchdog_thread.request_stop();
      g_watchdog_thread.join();
    }
    if (config::video.dd.config_revert_on_disconnect) {
      platf::display_helper_client::reset_connection();
    }
    clear_active_session();
  }
}  // namespace display_helper_integration

#endif
