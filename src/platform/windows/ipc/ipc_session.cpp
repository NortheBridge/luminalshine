/**
 * @file src/platform/windows/ipc/ipc_session.cpp
 * @brief Implements the IPC session logic for Windows WGC capture integration.
 * Handles control IPC, shared texture setup, and event-driven frame synchronization
 * between the main process and the WGC capture helper process.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <thread>

// local includes
#include "config.h"
#include "ipc_session.h"
#include "misc_utils.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

// platform includes
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

namespace platf::dxgi {
  namespace {
    constexpr auto kRecentDesktopSwitchGrace = std::chrono::seconds(3);
    std::atomic<std::int64_t> g_last_wgc_desktop_switch_us {0};

    std::int64_t now_steady_us() {
      using namespace std::chrono;
      return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void record_recent_wgc_desktop_switch() {
      g_last_wgc_desktop_switch_us.store(now_steady_us(), std::memory_order_relaxed);
    }

    struct frame_metadata_snapshot_t {
      LONG64 frame_id = 0;
      LONG64 frame_qpc = 0;
    };

    bool read_frame_metadata_snapshot(const frame_metadata_t *metadata, frame_metadata_snapshot_t &snapshot) {
      if (!metadata) {
        return false;
      }

      constexpr int max_attempts = 64;
      for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const auto sequence_start = metadata->sequence;
        if ((sequence_start & 1) != 0) {
          std::this_thread::yield();
          continue;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        const auto frame_id = metadata->frame_id;
        const auto frame_qpc = metadata->frame_qpc;
        std::atomic_thread_fence(std::memory_order_acquire);

        const auto sequence_end = metadata->sequence;
        if (sequence_start == sequence_end && (sequence_end & 1) == 0) {
          snapshot.frame_id = frame_id;
          snapshot.frame_qpc = frame_qpc;
          return true;
        }

        std::this_thread::yield();
      }

      return false;
    }
  }  // namespace

  bool recent_wgc_desktop_switch_grace_active() {
    const auto last_switch_us = g_last_wgc_desktop_switch_us.load(std::memory_order_relaxed);
    if (last_switch_us == 0) {
      return false;
    }

    return (now_steady_us() - last_switch_us) <
           std::chrono::duration_cast<std::chrono::microseconds>(kRecentDesktopSwitchGrace).count();
  }

  ipc_session_t::~ipc_session_t() {
    // Best-effort shutdown. Avoid throwing from a destructor.
    try {
      _initialized = false;
      _force_reinit = true;

      // Flush any pending work on the capture device before tearing down shared resources.
      if (_device) {
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        _device->GetImmediateContext(ctx.put());
        if (ctx) {
          ctx->Flush();
        }
      }

      if (_pipe) {
        _pipe->stop();
        _pipe.reset();
      }

      if (_frame_metadata) {
        UnmapViewOfFile(_frame_metadata);
        _frame_metadata = nullptr;
      }

      _shared_texture = nullptr;
      _keyed_mutex = nullptr;
      _frame_ready_event.close();
      _frame_metadata_mapping.close();

      stop_helper_process();
    } catch (...) {
      // Intentionally swallow all exceptions.
    }
  }

  void ipc_session_t::handle_desktop_switch_message(std::span<const uint8_t> msg) {
    if (msg.size() == 1 && msg[0] == SECURE_DESKTOP_MSG) {
      record_recent_wgc_desktop_switch();
      BOOST_LOG(info) << "WGC helper reported a desktop switch; forcing capture reinit and preferring DXGI fallback";
      _should_swap_to_dxgi = true;
    }
  }

  int ipc_session_t::init(const ::video::config_t &config, std::string_view display_name, ID3D11Device *device) {
    _process_helper = std::make_unique<ProcessHandler>();
    _config = config;
    _display_name = display_name;
    _device.copy_from(device);
    return 0;
  }

  void ipc_session_t::initialize_if_needed() {
    // Fast path: already successfully initialized
    if (_initialized) {
      return;
    }

    // Attempt to become the initializing thread
    bool expected = false;
    if (!_initializing.compare_exchange_strong(expected, true)) {
      // Another thread is initializing; wait until it finishes (either success or failure)
      while (_initializing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return;  // After wait, either initialized is true (success) or false (failure); caller can retry later
    }

    // We are the initializing thread now. Ensure we clear the flag on all exit paths.
    auto clear_initializing = util::fail_guard([this]() {
      _initializing = false;
    });

    // Check if properly initialized via init() first
    if (!_process_helper) {
      BOOST_LOG(debug) << "Cannot initialize_if_needed without prior init()";
      _initialized = false;
      return;
    }

    // Reset success flag before attempting
    _initialized = false;

    if (_pipe) {
      _pipe->stop();
      _pipe.reset();
    }
    if (_frame_metadata) {
      UnmapViewOfFile(_frame_metadata);
      _frame_metadata = nullptr;
    }
    _frame_ready_event.close();
    _frame_metadata_mapping.close();
    _shared_texture = nullptr;
    _keyed_mutex = nullptr;
    _last_frame_id = 0;
    _frame_qpc = 0;
    _force_reinit = false;
    _should_swap_to_dxgi = false;

    // Ensure previous helper is fully stopped before restarting. This avoids overlapping D3D11 allocations
    // across rapid re-inits that have been observed to destabilize the NVIDIA driver stack.
    stop_helper_process();

    // Give the driver a brief window to release resources if we just tore down.
    if (_last_helper_stop.time_since_epoch().count() != 0) {
      auto since_stop = std::chrono::steady_clock::now() - _last_helper_stop;
      if (since_stop < std::chrono::milliseconds(200)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200) - since_stop);
      }
    }

    // Flush any pending work on the capture device before creating a new shared texture.
    if (_device) {
      winrt::com_ptr<ID3D11DeviceContext> ctx;
      _device->GetImmediateContext(ctx.put());
      if (ctx) {
        ctx->Flush();
      }
    }

    // Get the directory of the main executable (Unicode-safe)
    std::wstring exePathBuffer(MAX_PATH, L'\0');
    GetModuleFileNameW(nullptr, exePathBuffer.data(), MAX_PATH);
    exePathBuffer.resize(wcslen(exePathBuffer.data()));
    std::filesystem::path mainExeDir = std::filesystem::path(exePathBuffer).parent_path();
    std::string pipe_guid = generate_guid();

    std::filesystem::path exe_path = mainExeDir / L"tools" / L"sunshine_wgc_capture.exe";
    std::wstring arguments = platf::from_utf8(pipe_guid);

    if (!_process_helper->start(exe_path.wstring(), arguments)) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to start sunshine_wgc_capture executable at: " << exe_path.wstring()
                       << " (error code: " << err << ")";
      return;
    }

    auto on_message = [this](std::span<const uint8_t> msg) {
      if (msg.size() == 1) {
        handle_desktop_switch_message(msg);
      }
    };

    auto on_error = [](const std::string &err) {
      BOOST_LOG(error) << "Pipe error: " << err.c_str();
    };

    auto on_broken_pipe = [this]() {
      BOOST_LOG(warning) << "Broken pipe detected, forcing re-init";
      _force_reinit = true;
    };

    auto anon_connector = std::make_unique<AnonymousPipeFactory>();

    auto control_pipe = anon_connector->create_server(pipe_guid);
    if (!control_pipe) {
      BOOST_LOG(error) << "IPC pipe setup failed for WGC session; aborting";
      return;
    }

    control_pipe->wait_for_client_connection(5000);

    if (!control_pipe->is_connected()) {
      BOOST_LOG(error) << "Helper failed to connect to control pipe within timeout";
      _process_helper->terminate();
      return;
    }

    // Send config data to helper process
    config_data_t config_data = {};
    config_data.dynamic_range = _config.dynamicRange;
    config_data.log_level = config::sunshine.min_log_level;

    // Convert display_name (std::string) to wchar_t[32]
    if (!_display_name.empty()) {
      std::wstring wdisplay_name(_display_name.begin(), _display_name.end());
      wcsncpy_s(config_data.display_name, wdisplay_name.c_str(), 31);
      config_data.display_name[31] = L'\0';
    } else {
      config_data.display_name[0] = L'\0';
    }

    // We need to make sure helper uses the same adapter for now.
    // This won't be a problem in future versions when we add support for cross adapter capture.
    // But for now, it is required that we use the exact same one.
    if (_device) {
      try_get_adapter_luid(config_data.adapter_luid);
    } else {
      BOOST_LOG(warning) << "No D3D11 device available, helper will use default adapter";
      memset(&config_data.adapter_luid, 0, sizeof(LUID));
    }

    auto config_span = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&config_data), sizeof(config_data_t));
    if (!control_pipe->send(config_span, 5000)) {
      BOOST_LOG(error) << "Failed to send configuration data to helper process";
      _process_helper->terminate();
      return;
    }

    constexpr auto handle_wait_timeout = std::chrono::seconds(3);
    auto deadline = std::chrono::steady_clock::now() + handle_wait_timeout;
    std::array<uint8_t, sizeof(shared_handle_data_t)> control_buffer {};
    bool handle_received = false;
    bool timed_out_waiting = false;

    while (!handle_received) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out_waiting = true;
        break;
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int wait_ms = std::max(1, static_cast<int>(remaining.count()));

      size_t bytes_read = 0;
      auto result = control_pipe->receive(std::span<uint8_t>(control_buffer.data(), control_buffer.size()), bytes_read, wait_ms);

      if (result == PipeResult::Success) {
        if (bytes_read == sizeof(shared_handle_data_t)) {
          shared_handle_data_t handle_data {};
          memcpy(&handle_data, control_buffer.data(), sizeof(handle_data));
          if (setup_shared_resources_from_shared_handles(handle_data)) {
            handle_received = true;
          } else {
            break;
          }
        } else if (bytes_read == 1) {
          handle_desktop_switch_message(std::span<const uint8_t>(control_buffer.data(), 1));
        } else if (bytes_read > 0) {
          BOOST_LOG(warning) << "Ignoring unexpected control payload (" << bytes_read << " bytes) while waiting for shared handle";
        }
      } else if (result == PipeResult::Timeout) {
        continue;
      } else if (result == PipeResult::BrokenPipe) {
        BOOST_LOG(warning) << "Broken pipe while waiting for handle data from helper process";
        break;
      } else {
        BOOST_LOG(error) << "Control pipe receive failed while waiting for handle data (state=" << static_cast<int>(result) << ')';
        break;
      }
    }

    if (!handle_received) {
      if (timed_out_waiting) {
        BOOST_LOG(error) << "Timed out waiting for handle data from helper process (3s)";
      }
      BOOST_LOG(error) << "Failed to receive handle data from helper process! Helper is likely deadlocked!";
      _process_helper->terminate();
      return;
    }

    auto cleanup_on_failure = util::fail_guard([this]() {
      if (_pipe) {
        _pipe->stop();
        _pipe.reset();
      }
      if (_frame_metadata) {
        UnmapViewOfFile(_frame_metadata);
        _frame_metadata = nullptr;
      }
      _shared_texture = nullptr;
      _keyed_mutex = nullptr;
      _frame_ready_event.close();
      _frame_metadata_mapping.close();
      if (_process_helper) {
        _process_helper->terminate();
      }
    });

    _pipe = std::make_unique<AsyncNamedPipe>(std::move(control_pipe));

    if (!_pipe->start(on_message, on_error, on_broken_pipe)) {
      BOOST_LOG(error) << "Failed to start AsyncNamedPipe for helper communication";
      return;
    }

    cleanup_on_failure.disable();
    _initialized = true;
  }

  capture_e ipc_session_t::wait_for_frame(std::chrono::milliseconds timeout) {
    if (!_frame_ready_event || !_frame_metadata) {
      return capture_e::error;
    }

    const DWORD wait_ms = timeout.count() <= 0 ?
                            0 :
                            static_cast<DWORD>(std::min<int64_t>(timeout.count(), INFINITE - 1));
    const DWORD wait_result = WaitForSingleObject(_frame_ready_event.get(), wait_ms);

    if (wait_result == WAIT_TIMEOUT) {
      return capture_e::timeout;
    }

    if (wait_result != WAIT_OBJECT_0) {
      BOOST_LOG(warning) << "Frame-ready event wait failed (" << GetLastError() << "); forcing re-init";
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    return capture_e::ok;
  }

  bool ipc_session_t::try_get_adapter_luid(LUID &luid_out) {
    luid_out = {};

    if (!_device) {
      BOOST_LOG(warning) << "_device was null; default adapter will be used";
      return false;
    }

    winrt::com_ptr<IDXGIDevice> dxgi_device = _device.try_as<IDXGIDevice>();
    if (!dxgi_device) {
      BOOST_LOG(warning) << "try_as<IDXGIDevice>() failed; default adapter will be used";
      return false;
    }

    winrt::com_ptr<IDXGIAdapter> adapter;
    HRESULT hr = dxgi_device->GetAdapter(adapter.put());
    if (FAILED(hr) || !adapter) {
      BOOST_LOG(warning) << "GetAdapter() failed; default adapter will be used";
      return false;
    }

    DXGI_ADAPTER_DESC desc {};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "GetDesc() failed; default adapter will be used";
      return false;
    }

    luid_out = desc.AdapterLuid;
    set_last_wgc_adapter_luid(luid_out);
    return true;
  }

  capture_e ipc_session_t::acquire(std::chrono::milliseconds timeout, winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out) {
    auto wait_status = wait_for_frame(timeout);
    if (wait_status != capture_e::ok) {
      return wait_status;
    }

    // Additional validation: ensure required resources are available
    if (!_shared_texture || !_keyed_mutex) {
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    HRESULT hr = _keyed_mutex->AcquireSync(0, 3000);

    if (hr == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Helper process abandoned the keyed mutex, implying it may have crashed or was forcefully terminated.";
      _should_swap_to_dxgi = false;  // Don't swap to DXGI, just reinit
      _force_reinit = true;
      _initialized = false;

      // If WAIT_ABANDONED implies ownership, release immediately to avoid leaving the mutex held.
      (void) _keyed_mutex->ReleaseSync(0);
      return capture_e::reinit;
    }

    if (hr == WAIT_TIMEOUT) {
      BOOST_LOG(error) << "Timed out waiting for keyed mutex; forcing re-init";
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    if (hr != S_OK) {
      BOOST_LOG(error) << "Failed to acquire keyed mutex [0x"sv << util::hex(hr).to_string_view() << "]; forcing re-init";
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    // The helper publishes metadata while holding this same keyed mutex, so the
    // snapshot below belongs to the shared texture we just acquired.
    frame_metadata_snapshot_t snapshot;
    if (!read_frame_metadata_snapshot(_frame_metadata, snapshot)) {
      (void) _keyed_mutex->ReleaseSync(0);
      return capture_e::timeout;
    }

    if (snapshot.frame_id <= _last_frame_id) {
      (void) _keyed_mutex->ReleaseSync(0);
      return capture_e::timeout;
    }

    _last_frame_id = snapshot.frame_id;
    _frame_qpc = static_cast<uint64_t>(snapshot.frame_qpc);

    // Set output parameters
    gpu_tex_out = _shared_texture;
    frame_qpc_out = _frame_qpc;

    return capture_e::ok;
  }

  void ipc_session_t::release() {
    if (_keyed_mutex) {
      const HRESULT hr = _keyed_mutex->ReleaseSync(0);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to release keyed mutex [0x"sv << util::hex(hr).to_string_view() << ']';
      }
    }
  }

  bool ipc_session_t::setup_shared_resources_from_shared_handles(const shared_handle_data_t &handle_data) {
    if (!_device) {
      BOOST_LOG(error) << "No D3D11 device available for WGC shared-resource setup";
      return false;
    }

    if (!handle_data.texture_handle || handle_data.texture_handle == INVALID_HANDLE_VALUE ||
        !handle_data.frame_event_handle || handle_data.frame_event_handle == INVALID_HANDLE_VALUE ||
        !handle_data.frame_metadata_handle || handle_data.frame_metadata_handle == INVALID_HANDLE_VALUE) {
      BOOST_LOG(error) << "Invalid WGC shared handle data provided";
      return false;
    }

    // Get the helper process handle to duplicate from
    HANDLE helper_process_handle = _process_helper->get_process_handle();
    if (!helper_process_handle) {
      BOOST_LOG(error) << "Failed to get helper process handle for duplication";
      return false;
    }

    // Duplicate handles from the helper process into this process. We copy from
    // the helper because it runs at a lower integrity level.
    auto duplicate_helper_handle = [&](HANDLE source, const char *name) -> winrt::handle {
      HANDLE duplicated = nullptr;
      if (!DuplicateHandle(
            helper_process_handle,
            source,
            GetCurrentProcess(),
            &duplicated,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
          )) {
        BOOST_LOG(error) << "Failed to duplicate WGC " << name << " handle from helper process: " << GetLastError();
        return {};
      }

      return winrt::handle {duplicated};
    };

    auto duplicated_texture_handle = duplicate_helper_handle(handle_data.texture_handle, "texture");
    auto duplicated_event_handle = duplicate_helper_handle(handle_data.frame_event_handle, "frame event");
    auto duplicated_metadata_handle = duplicate_helper_handle(handle_data.frame_metadata_handle, "frame metadata");
    if (!duplicated_texture_handle || !duplicated_event_handle || !duplicated_metadata_handle) {
      return false;
    }

    auto device1 = _device.try_as<ID3D11Device1>();
    if (!device1) {
      BOOST_LOG(error) << "Failed to get ID3D11Device1 interface for duplicated handle";
      return false;
    }

    winrt::com_ptr<IUnknown> unknown;
    HRESULT hr = device1->OpenSharedResource1(duplicated_texture_handle.get(), __uuidof(IUnknown), winrt::put_abi(unknown));
    if (FAILED(hr) || !unknown) {
      BOOST_LOG(error) << "Failed to open shared texture from duplicated handle: 0x" << std::hex << hr << " (decimal: " << std::dec << (int32_t) hr << ")";
      return false;
    }

    winrt::com_ptr<ID3D11Texture2D> texture;
    hr = unknown->QueryInterface(__uuidof(ID3D11Texture2D), texture.put_void());
    if (FAILED(hr) || !texture) {
      BOOST_LOG(error) << "Failed to query ID3D11Texture2D from shared resource: 0x" << std::hex << hr << " (decimal: " << std::dec << (int32_t) hr << ")";
      return false;
    }

    // Verify texture properties
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (desc.Width != handle_data.width || desc.Height != handle_data.height) {
      BOOST_LOG(warning) << "Shared texture size mismatch (expected " << handle_data.width << "x" << handle_data.height
                         << ", got " << desc.Width << "x" << desc.Height << ")";
    }

    auto *metadata = static_cast<frame_metadata_t *>(MapViewOfFile(
      duplicated_metadata_handle.get(),
      FILE_MAP_READ,
      0,
      0,
      sizeof(frame_metadata_t)
    ));
    if (!metadata) {
      BOOST_LOG(error) << "Failed to map WGC frame metadata view: " << GetLastError();
      return false;
    }

    auto metadata_guard = util::fail_guard([&]() {
      UnmapViewOfFile(metadata);
    });

    _shared_texture = texture;
    _width = handle_data.width;
    _height = handle_data.height;

    // Get keyed mutex interface for synchronization
    _keyed_mutex = _shared_texture.try_as<IDXGIKeyedMutex>();
    if (!_keyed_mutex) {
      BOOST_LOG(error) << "Failed to get keyed mutex interface from shared texture";
      _shared_texture = nullptr;
      return false;
    }

    _frame_ready_event = std::move(duplicated_event_handle);
    _frame_metadata_mapping = std::move(duplicated_metadata_handle);
    _frame_metadata = metadata;
    frame_metadata_snapshot_t snapshot;
    _last_frame_id = read_frame_metadata_snapshot(_frame_metadata, snapshot) ? snapshot.frame_id : 0;
    metadata_guard.disable();
    return true;
  }

  void ipc_session_t::stop_helper_process() {
    if (!_process_helper) {
      return;
    }

    DWORD exit_code = 0;
    _process_helper->terminate();  // best effort
    _process_helper->wait(exit_code);
    _last_helper_stop = std::chrono::steady_clock::now();
  }

}  // namespace platf::dxgi
