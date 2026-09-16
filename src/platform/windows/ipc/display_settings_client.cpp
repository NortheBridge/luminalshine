/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <utility>
  #include <vector>

  // local
  #include "display_settings_client.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  namespace {
    constexpr int kConnectTimeoutMs = 2000;
    constexpr int kSendTimeoutMs = 5000;
    constexpr int kShutdownIpcTimeoutMs = 500;
    // A display-helper completion is advisory: every caller verifies the
    // requested Windows topology independently.  Blocking the HTTPS launch
    // response for 30 seconds makes fixed-deadline Moonlight clients abandon
    // an otherwise healthy host — but real Windows modesets with driver settle
    // routinely take several seconds, and classifying them as indeterminate
    // destabilizes the launch path.  10 seconds covers a legitimate modeset
    // while remaining inside the clients' launch tolerances.
    constexpr int kApplyResultTimeoutMs = 10000;
    constexpr int kRevertAcceptedTimeoutMs = 2000;

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

    int effective_connect_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kConnectTimeoutMs;
    }

    int effective_send_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kSendTimeoutMs;
    }

    // Duplicate of the helper's process handle (owned here; see bind_helper_process). Only ever
    // used to answer "has the helper exited?" while a connect is polling for its pipe.
    std::mutex &helper_process_mutex() {
      static std::mutex m;
      return m;
    }

    HANDLE &bound_helper_process() {
      static HANDLE h = nullptr;
      return h;
    }

    // Liveness probe handed to the pipe factory. Unknown (nothing bound) reads as "still alive" so
    // the connect falls back to its time bound rather than giving up at once.
    bool helper_process_exited() {
      std::lock_guard<std::mutex> lg(helper_process_mutex());
      const HANDLE h = bound_helper_process();
      if (!h) {
        return false;
      }
      return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    }

  }  // namespace

  /**
   * @brief IPC message types used by the display settings helper protocol.
   */
  enum class MsgType : uint8_t {
    Apply = 1,  ///< Apply display settings from JSON payload.
    Revert = 2,  ///< Revert display settings to the previous state.
    Reset = 3,  ///< Reset helper persistence/state (if supported).
    ExportGolden = 4,  ///< Export current OS settings as golden snapshot
    ApplyResult = 6,  ///< Helper acknowledgement for APPLY (payload: [u8 success][optional message...]).
    Disarm = 7,  ///< Cancel any pending restore/watchdog actions on the helper.
    SnapshotCurrent = 8,  ///< Save current session snapshot (rotate current->previous) without applying config.
    WddmReset = 9,  ///< Synthesise Ctrl+Win+Shift+B in the user's desktop. No payload, no reply.
    RevertAccepted = 10,  ///< Helper acknowledgement that asynchronous REVERT was scheduled.
    ApplyAccepted = 11,  ///< Helper acknowledgement that APPLY was queued for execution.
    Ping = 0xFE,  ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  namespace {
    std::optional<bool> wait_for_apply_result_locked(platf::dxgi::INamedPipe &pipe) {
      using namespace std::chrono;

      const auto deadline = steady_clock::now() + milliseconds(kApplyResultTimeoutMs);
      std::array<uint8_t, 2048> buffer {};
      bool accepted = false;

      while (steady_clock::now() < deadline) {
        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        int timeout_ms = static_cast<int>(std::max<long long>(remaining.count(), 100LL));
        size_t bytes_read = 0;
        auto result = pipe.receive(buffer, bytes_read, timeout_ms);

        if (result == platf::dxgi::PipeResult::Timeout) {
          // The reply can only arrive on the connection that carried the APPLY. The pipe object
          // never reconnects behind our back (see ensure_connected_locked), so a connection that
          // is no longer up means the reply is unreachable; stop instead of waiting out the budget.
          if (!pipe.is_connected()) {
            BOOST_LOG(error) << "Display helper IPC: connection dropped while waiting for APPLY result";
            return std::nullopt;
          }
          continue;
        }
        if (result != platf::dxgi::PipeResult::Success) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for APPLY result (pipe error)";
          return std::nullopt;
        }
        if (bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection closed while waiting for APPLY result";
          return std::nullopt;
        }

        const uint8_t msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::ApplyResult)) {
          bool success = bytes_read >= 2 && buffer[1] != 0;
          if (!success && bytes_read > 2) {
            std::string helper_msg(reinterpret_cast<const char *>(buffer.data() + 2), reinterpret_cast<const char *>(buffer.data() + bytes_read));
            BOOST_LOG(error) << "Display helper reported APPLY failure: " << helper_msg;
          }
          return success;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::ApplyAccepted)) {
          accepted = bytes_read >= 2 && buffer[1] != 0;
          if (!accepted) {
            BOOST_LOG(error) << "Display helper rejected APPLY before execution";
            return false;
          }
          BOOST_LOG(debug) << "Display helper acknowledged APPLY acceptance; awaiting completion.";
          continue;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting APPLY result";
      }

      BOOST_LOG(error) << "Display helper IPC: timed out waiting for APPLY completion"
                       << (accepted ? " after acceptance" : " before acceptance")
                       << " (" << kApplyResultTimeoutMs << " ms)";
      return std::nullopt;
    }
    bool wait_for_revert_accepted_locked(platf::dxgi::INamedPipe &pipe) {
      using namespace std::chrono;
      const auto deadline = steady_clock::now() + milliseconds(kRevertAcceptedTimeoutMs);
      std::array<uint8_t, 256> buffer {};
      while (steady_clock::now() < deadline) {
        size_t bytes_read = 0;
        const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        const auto result = pipe.receive(buffer, bytes_read,
                                         static_cast<int>(std::max<int64_t>(remaining.count(), 100)));
        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (result != platf::dxgi::PipeResult::Success || bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection failed while awaiting REVERT acceptance";
          return false;
        }
        if (buffer[0] == static_cast<uint8_t>(MsgType::RevertAccepted)) {
          return bytes_read >= 2 && buffer[1] != 0;
        }
        if (buffer[0] != static_cast<uint8_t>(MsgType::Ping)) {
          BOOST_LOG(debug) << "Display helper IPC: ignoring message type="
                           << static_cast<int>(buffer[0]) << " while awaiting REVERT acceptance";
        }
      }
      BOOST_LOG(error) << "Display helper IPC: timed out waiting for REVERT acceptance";
      return false;
    }
  }  // namespace

  static bool send_message(
    platf::dxgi::INamedPipe &pipe,
    MsgType type,
    const std::vector<uint8_t> &payload,
    std::optional<int> send_timeout_override_ms = std::nullopt
  ) {
    const bool is_ping = (type == MsgType::Ping);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                      << ", payload_len=" << payload.size();
    }
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    const int timeout_ms = send_timeout_override_ms.value_or(effective_send_timeout());
    const bool ok = pipe.send(out, timeout_ms);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: send result=" << (ok ? "true" : "false");
    }
    return ok;
  }

  // Persistent connection across a stream session. Helper stays alive until
  // successful revert; we reuse the data pipe for APPLY/REVERT.
  //
  // The object is a plain (framed) client pipe with no self-healing: a connection is either the one
  // a request was sent on, or it is gone and the next command opens a new one via
  // ensure_connected_locked. That keeps every request/response exchange bound to a single
  // connection, so a reply can never be awaited on a connection other than the one it was sent on.
  static std::unique_ptr<platf::dxgi::INamedPipe> &pipe_singleton() {
    static std::unique_ptr<platf::dxgi::INamedPipe> s_pipe;
    return s_pipe;
  }

  // Global mutex to serialize all access to the pipe (connect, reset, send)
  // and prevent interleaved writes on a BYTE-mode pipe.
  static std::mutex &pipe_mutex() {
    static std::mutex m;
    return m;
  }

  // Ensure connected while holding the pipe mutex. Returns true on success.
  //
  // Every connection goes through AnonymousPipeFactory, i.e. it consumes the helper's anonymous-pipe
  // handshake. The helper's server always speaks that handshake: the first thing it writes on a new
  // control connection is an 80-byte AnonConnectMsg preamble (a "{GUID}" in UTF-16) naming the data
  // pipe. A client that skipped the handshake (the former raw named-pipe fallback) received that
  // preamble inside its framed byte stream; FramedPipe's resync heuristic then locked onto a bogus
  // frame length derived from the GUID's trailing bytes and swallowed every subsequent helper reply
  // (APPLY accepted, APPLY result, ping echoes) while the host waited out the full completion
  // timeout. AnonymousPipeFactory::create_client already degrades to the plain control pipe when no
  // handshake message arrives, so there is no case the raw fallback served that this does not.
  //
  // The connect wait is bounded by the budget below and by helper-process liveness (see
  // bind_helper_process): a slow helper start is polled until its pipe appears, a crashed one is
  // reported as soon as the process is gone.
  static bool ensure_connected_locked(std::optional<int> connect_timeout_override_ms = std::nullopt) {
    if (shutdown_requested()) {
      return false;
    }
    auto &pipe = pipe_singleton();
    if (pipe && pipe->is_connected()) {
      return true;
    }

    // Never resume a dropped connection in place. The helper serves one client at a time and starts a
    // fresh session (new epoch, new handshake) for every connect, so a reconnect must be a brand-new
    // client object; any reply still owed on the old connection is gone with it.
    pipe.reset();

    const int connect_timeout_ms = std::max(0, connect_timeout_override_ms.value_or(effective_connect_timeout()));
    BOOST_LOG(debug) << "Display helper IPC: connecting to server pipe '"
                     << platf::display_helper_client::display_helper_pipe_name
                     << "' (timeout_ms=" << connect_timeout_ms << ")";

    platf::dxgi::ClientConnectOptions connect_options;
    connect_options.retry.max_wait = std::chrono::milliseconds(connect_timeout_ms);
    connect_options.server_exited = helper_process_exited;

    auto anonymous_factory = std::make_unique<platf::dxgi::AnonymousPipeFactory>();
    anonymous_factory->set_client_connect_options(std::move(connect_options));
    platf::dxgi::FramedPipeFactory factory(std::move(anonymous_factory));

    pipe = factory.create_client(platf::display_helper_client::display_helper_pipe_name);
    if (pipe && pipe->is_connected()) {
      return true;
    }
    pipe.reset();
    BOOST_LOG(warning) << "Display helper IPC: connection failed"
                       << (helper_process_exited() ? " (helper process has exited)" : "");
    return false;
  }

  void reset_connection() {
    std::lock_guard<std::mutex> lg(pipe_mutex());
    auto &pipe = pipe_singleton();
    if (pipe) {
      BOOST_LOG(debug) << "Display helper IPC: resetting cached connection";
      pipe->disconnect();
    }
    pipe.reset();
  }

  void bind_helper_process(void *process_handle) {
    HANDLE duplicate = nullptr;
    if (process_handle) {
      if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(process_handle), GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) {
        BOOST_LOG(warning) << "Display helper IPC: could not duplicate the helper process handle (winerr="
                           << GetLastError() << "); connect waits fall back to their time bound.";
        duplicate = nullptr;
      }
    }
    HANDLE previous = nullptr;
    {
      std::lock_guard<std::mutex> lg(helper_process_mutex());
      previous = std::exchange(bound_helper_process(), duplicate);
    }
    if (previous) {
      CloseHandle(previous);
    }
  }

  bool ensure_connected(int connect_timeout_ms) {
    std::lock_guard<std::mutex> lg(pipe_mutex());
    return ensure_connected_locked(connect_timeout_ms);
  }

  ApplyOutcome send_apply_json(const std::string &json) {
    BOOST_LOG(debug) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no connection";
      return ApplyOutcome::indeterminate;
    }
    std::vector<uint8_t> payload(json.begin(), json.end());
    auto &pipe = pipe_singleton();
    if (!pipe) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no pipe instance";
      return ApplyOutcome::indeterminate;
    }

    if (!send_message(*pipe, MsgType::Apply, payload)) {
      return ApplyOutcome::indeterminate;
    }

    if (auto result = wait_for_apply_result_locked(*pipe)) {
      return *result ? ApplyOutcome::applied : ApplyOutcome::rejected;
    }

    // The helper performs SetDisplayConfig before replying. A missing reply
    // therefore does not prove the apply failed: the modeset may have replaced
    // the IddCx swapchain (and briefly stalled IPC) after Windows committed it.
    // Let the integration layer verify observable OS state before deciding.
    return ApplyOutcome::indeterminate;
  }

  bool send_revert(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: REVERT request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: REVERT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Revert, payload)) {
      return wait_for_revert_accepted_locked(*pipe);
    }
    return false;
  }

  bool send_export_golden(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: EXPORT_GOLDEN request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: EXPORT_GOLDEN aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::ExportGolden, payload)) {
      return true;
    }
    return false;
  }

  bool send_reset() {
    BOOST_LOG(debug) << "Display helper IPC: RESET request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Reset, payload)) {
      return true;
    }
    return false;
  }

  bool send_disarm_restore() {
    BOOST_LOG(info) << "Display helper IPC: DISARM request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: DISARM aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Disarm, payload)) {
      return true;
    }
    return false;
  }

  bool send_disarm_restore_fast(int timeout_ms) {
    BOOST_LOG(debug) << "Display helper IPC: DISARM (fast) request queued (timeout_ms=" << timeout_ms << ")";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked(timeout_ms)) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Disarm, payload, timeout_ms)) {
      return true;
    }
    return false;
  }

  bool send_snapshot_current(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: SNAPSHOT_CURRENT request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::SnapshotCurrent, payload)) {
      return true;
    }
    return false;
  }

  bool send_wddm_reset() {
    BOOST_LOG(info) << "Display helper IPC: WDDM_RESET request queued (synthesise Ctrl+Win+Shift+B)";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: WDDM_RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::WddmReset, payload)) {
      return true;
    }
    return false;
  }

  bool send_stop() {
    BOOST_LOG(info) << "Display helper IPC: STOP request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: STOP aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Stop, payload)) {
      return true;
    }
    return false;
  }

  bool send_ping() {
    // No logging for ping path to reduce log spam
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Ping, payload)) {
      return true;
    }
    return false;
  }
}  // namespace platf::display_helper_client

#endif
