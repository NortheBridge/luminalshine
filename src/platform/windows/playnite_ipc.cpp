/**
 * @file src/platform/windows/playnite_ipc.cpp
 * @brief Playnite plugin IPC client using Windows named pipes with anonymous handshake.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_ipc.h"

#ifndef SUNSHINE_PLAYNITE_LAUNCHER
  #include "src/platform/windows/misc.h"

#endif
#include "src/utility.h"

#include <chrono>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

#ifdef SUNSHINE_PLAYNITE_LAUNCHER
// The launcher never runs as SYSTEM; provide a local stub to avoid linking heavy deps.
namespace platf {
  inline bool is_running_as_system() {
    return false;
  }
}  // namespace platf
#endif

namespace platf::playnite {
  namespace {
    inline constexpr char kControlPipeName[] = "Sunshine.PlayniteExtension";
  }

  IpcClient::IpcClient():
      control_name_(kControlPipeName) {}

  IpcClient::IpcClient(const std::string &control_name):
      control_name_(control_name.empty() ? kControlPipeName : control_name) {}

  IpcClient::~IpcClient() {
    stop();
  }

  void IpcClient::start() {
    if (running_.exchange(true)) {
      return;
    }
    worker_ = std::thread([this]() {
      this->run();
    });
  }

  void IpcClient::stop() {
    running_.store(false);
    // Signal the pipe to stop. Move the unique_ptr out under the lock, then stop/destroy it
    // outside the lock so a (potentially re-entrant) blocking stop() can't deadlock and so the
    // worker thread can't reset it concurrently.
    {
      std::unique_ptr<platf::dxgi::AsyncNamedPipe> local;
      {
        std::lock_guard lg {pipe_mutex_};
        local = std::move(pipe_);
      }
      if (local) {
        local->stop();
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    {
      std::lock_guard lg {pipe_mutex_};
      pipe_.reset();
    }
    active_.store(false);
  }

  void IpcClient::run() {
    while (running_.load()) {
      broken_.store(false);

      if (!is_playnite_running()) {
        if (!no_playnite_logged_) {
          BOOST_LOG(debug) << "Playnite IPC: Playnite not running; deferring client connection";
          no_playnite_logged_ = true;
        }
        // IPC client is only started when there's a session or API activity,
        // so use a reasonable retry interval (not aggressive polling)
        std::this_thread::sleep_for(2s);
        continue;
      }

      if (no_playnite_logged_) {
        BOOST_LOG(debug) << "Playnite IPC: Playnite detected; attempting client connection";
        no_playnite_logged_ = false;
      }

      auto data_pipe = connect_to_plugin();
      if (!data_pipe) {
        std::this_thread::sleep_for(500ms);
        continue;
      }

      auto on_msg = [this](std::span<const uint8_t> bytes) {
        accumulate_and_dispatch_lines(bytes);
      };
      auto on_err = [](const std::string &err) {
        BOOST_LOG(error) << "Playnite IPC: client pipe error: " << err;
      };
      auto on_broken = [this]() {
        BOOST_LOG(warning) << "Playnite IPC: client pipe broken";
        broken_.store(true);
      };

      {
        std::lock_guard lg {pipe_mutex_};
        pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
      }
      // start() may be called outside the lock; pipe_ is only reset by stop() which first
      // moves it out, and we are the only thread that creates it, so this access is safe.
      bool started = false;
      {
        std::lock_guard lg {pipe_mutex_};
        started = pipe_ && pipe_->start(on_msg, on_err, on_broken);
      }
      if (!started) {
        BOOST_LOG(error) << "Playnite IPC: failed to start async pipe";
        {
          std::lock_guard lg {pipe_mutex_};
          pipe_.reset();
        }
        std::this_thread::sleep_for(500ms);
        continue;
      }

      active_.store(true);
      if (connected_handler_) {
        try {
          connected_handler_();
        } catch (...) {}
      }
      serve_connected_loop();
      {
        // Move the pipe out under the lock, then stop/destroy it outside the lock to avoid
        // holding the mutex across a blocking stop() and to avoid racing stop()/send_json_line().
        std::unique_ptr<platf::dxgi::AsyncNamedPipe> local;
        {
          std::lock_guard lg {pipe_mutex_};
          local = std::move(pipe_);
        }
        if (local) {
          local->stop();
        }
      }
      active_.store(false);
      if (disconnected_handler_) {
        try {
          disconnected_handler_();
        } catch (...) {}
      }

      if (!running_.load()) {
        break;
      }
      std::this_thread::sleep_for(300ms);
    }
  }

  void IpcClient::accumulate_and_dispatch_lines(std::span<const uint8_t> bytes) {
    if (!bytes.empty()) {
      recv_buffer_.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    size_t start = 0;
    while (true) {
      auto pos = recv_buffer_.find('\n', start);
      if (pos == std::string::npos) {
        break;
      }
      std::string line = recv_buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      start = pos + 1;
      auto non_ws = line.find_first_not_of(" \t\r\n");
      if (non_ws == std::string::npos) {
        continue;
      }
      if (handler_) {
        handler_(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(line.data()), line.size()));
      }
    }
    if (start > 0) {
      recv_buffer_.erase(0, start);
    }
  }

  std::unique_ptr<platf::dxgi::INamedPipe> IpcClient::connect_to_plugin() {
    platf::dxgi::AnonymousPipeFactory factory;
    const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
    BOOST_LOG(debug) << "Playnite IPC: connecting to control pipe '" << ctl << "'";
    auto pipe = factory.create_client(ctl);
    if (!pipe) {
      BOOST_LOG(debug) << "Playnite IPC: control connection attempt failed";
      return nullptr;
    }
    BOOST_LOG(debug) << "Playnite IPC: data pipe acquired";
    return pipe;
  }

  void IpcClient::serve_connected_loop() {
    BOOST_LOG(debug) << "Playnite IPC: client connected";
    while (running_.load() && !broken_.load()) {
      bool connected = false;
      {
        std::lock_guard lg {pipe_mutex_};
        connected = pipe_ && pipe_->is_connected();
      }
      if (!connected) {
        break;
      }
      std::this_thread::sleep_for(200ms);
    }
    BOOST_LOG(debug) << "Playnite IPC: client disconnected";
  }

  bool IpcClient::is_playnite_running() {
    try {
      auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!v1.empty()) {
        return true;
      }
      auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      return !v2.empty();
    } catch (...) {
      return false;
    }
  }

  bool IpcClient::send_json_line(const std::string &json) {
    std::string payload = json;
    payload.push_back('\n');
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    // Hold the lock across the connectivity check and send so the worker thread can't reset
    // pipe_ between the check and the dereference.
    std::lock_guard lg {pipe_mutex_};
    if (!pipe_ || !pipe_->is_connected()) {
      BOOST_LOG(warning) << "Playnite IPC: send_json_line called but client not connected";
      return false;
    }
    BOOST_LOG(debug) << "Playnite IPC: sending command (" << payload.size() << " bytes)";
    pipe_->send(bytes);
    return true;
  }

}  // namespace platf::playnite
