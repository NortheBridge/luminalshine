#include "video_worker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include "src/globals.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/virtual_display_backend.h"
#include "src/platform/windows/virtual_display_vgd.h"

using namespace std::literals;

namespace platf::video_worker {
  namespace {
    enum class message_e: std::uint32_t {
      start = 1,
      shutdown,
      idr,
      invalidate,
      packet,
      hdr,
      touch,
      chroma_downgrade,
      finished,
      heartbeat,
      encoder_ready,
      capture_ready,
      startup_error,
      capture_reinitializing,
    };

#pragma pack(push, 1)
    struct header_t {
      message_e type;
      std::uint32_t size;
    };
    struct packet_header_t {
      std::int64_t frame_index;
      std::int64_t frame_timestamp_ns;
      std::int64_t host_processing_timestamp_ns;
      std::uint32_t data_size;
      std::uint8_t idr;
      std::uint8_t after_rfi;
      std::uint8_t has_frame_timestamp;
      std::uint8_t has_host_processing_timestamp;
      std::uint8_t capture_placeholder;
      std::uint64_t capture_generation;
    };
    struct generation_t {
      std::uint64_t value;
    };
    struct invalidate_t {
      std::int64_t first;
      std::int64_t last;
    };
    struct start_t {
      std::uint32_t magic;
      std::uint32_t protocol_version;
      video::config_t config;
      video::encoder_probe_snapshot_t encoder;
      std::uint8_t direct_vgd;
      std::uint8_t safe_capture;
      std::uint8_t reserved[6];
      std::uint64_t vgd_session_id;
      std::uint32_t vgd_ring_slots;
      std::uint32_t vgd_transport_flags;
      std::uint32_t vgd_generation;
      char vgd_display_name[64];
    };
#pragma pack(pop)

    std::string g_child_pipe;
    std::uint32_t g_parent_pid {};
    constexpr std::uint32_t kProtocolMagic = 0x4C565750;  // LVWP
    constexpr std::uint32_t kProtocolVersion = 8;
    std::atomic_bool g_capture_reinitializing {false};
    std::atomic<std::uint64_t> g_capture_generation {1};
    std::mutex g_capture_generation_mutex;
    constexpr std::size_t kMaxPacketPayload = 32u * 1024u * 1024u - sizeof(packet_header_t);

    static_assert(std::is_trivially_copyable_v<video::config_t>);
    static_assert(std::is_trivially_copyable_v<video::encoder_probe_snapshot_t>);
    static_assert(std::is_trivially_copyable_v<start_t>);
    static_assert(std::is_trivially_copyable_v<video::hdr_info_raw_t>);
    static_assert(std::is_trivially_copyable_v<input::touch_port_t>);

    std::vector<std::uint8_t> materialize_packet(video::packet_raw_t &packet) {
      if (packet.data_size() > kMaxPacketPayload) {
        return {};
      }
      std::vector<std::uint8_t> bytes(packet.data(), packet.data() + packet.data_size());
      if (!packet.is_idr() || !packet.replacements) {
        return bytes;
      }
      for (const auto &replacement : *packet.replacements) {
        if (replacement.old.empty()) continue;
        std::vector<std::uint8_t> rewritten;
        const auto old_begin = reinterpret_cast<const std::uint8_t *>(replacement.old.data());
        const auto new_begin = reinterpret_cast<const std::uint8_t *>(replacement._new.data());
        for (std::size_t offset = 0; offset < bytes.size();) {
          if (offset + replacement.old.size() <= bytes.size() &&
              std::equal(old_begin, old_begin + replacement.old.size(), bytes.begin() + offset)) {
            if (rewritten.size() > kMaxPacketPayload - replacement._new.size()) return {};
            rewritten.insert(rewritten.end(), new_begin, new_begin + replacement._new.size());
            offset += replacement.old.size();
          } else {
            if (rewritten.size() == kMaxPacketPayload) return {};
            rewritten.push_back(bytes[offset++]);
          }
        }
        bytes = std::move(rewritten);
      }
      return bytes;
    }

    std::int64_t serialize_timestamp(const std::chrono::steady_clock::time_point &timestamp) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count();
    }

    std::chrono::steady_clock::time_point deserialize_timestamp(std::int64_t timestamp_ns) {
      return std::chrono::steady_clock::time_point {std::chrono::nanoseconds {timestamp_ns}};
    }

    bool wait_overlapped(HANDLE pipe, OVERLAPPED &overlapped, DWORD timeout, DWORD &transferred) {
      const DWORD wait = ::WaitForSingleObject(overlapped.hEvent, timeout);
      if (wait != WAIT_OBJECT_0) {
        (void) ::CancelIoEx(pipe, &overlapped);
        // OVERLAPPED and its event are stack-owned. They must remain alive
        // until cancellation is observed as complete.
        (void) ::WaitForSingleObject(overlapped.hEvent, INFINITE);
        // The IO can complete in the race window between the wait expiring and
        // the cancellation taking effect. Those bytes are already consumed from
        // the byte-mode pipe: discarding them here permanently desynchronizes
        // message framing, so honor them exactly like an in-time completion.
        DWORD completed = 0;
        if (::GetOverlappedResult(pipe, &overlapped, &completed, FALSE) && completed != 0) {
          transferred = completed;
          return true;
        }
        return false;
      }
      return ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    }

    bool write_exact(HANDLE pipe, const void *data, std::uint32_t size, DWORD timeout = 500) {
      const auto *cursor = static_cast<const std::uint8_t *>(data);
      while (size) {
        DWORD written = 0;
        OVERLAPPED overlapped {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        const BOOL started = ::WriteFile(pipe, cursor, size, &written, &overlapped);
        const DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
        const bool ok = started || (error == ERROR_IO_PENDING && wait_overlapped(pipe, overlapped, timeout, written));
        ::CloseHandle(overlapped.hEvent);
        if (!ok || written == 0) {
          return false;
        }
        cursor += written;
        size -= written;
      }
      return true;
    }

    bool read_exact(HANDLE pipe, void *data, std::uint32_t size, DWORD timeout = INFINITE) {
      auto *cursor = static_cast<std::uint8_t *>(data);
      const std::uint32_t requested = size;
      while (size) {
        // Once ANY byte of this datum has been consumed from the byte-mode
        // pipe, abandoning the remainder on a short polling timeout would
        // permanently desynchronize message framing. Extend the deadline for
        // the tail exactly like receive() does for payloads.
        const DWORD effective_timeout = size == requested ? timeout : std::max<DWORD>(timeout, 5000);
        DWORD read = 0;
        OVERLAPPED overlapped {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        const BOOL started = ::ReadFile(pipe, cursor, size, &read, &overlapped);
        const DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
        const bool ok = started || (error == ERROR_IO_PENDING && wait_overlapped(pipe, overlapped, effective_timeout, read));
        ::CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) {
          return false;
        }
        cursor += read;
        size -= read;
      }
      return true;
    }

    bool send(HANDLE pipe, message_e type, const void *payload = nullptr, std::uint32_t size = 0, DWORD timeout = 500) {
      const header_t header {type, size};
      return write_exact(pipe, &header, sizeof(header), timeout) && (!size || write_exact(pipe, payload, size, timeout));
    }

    // The child's packet stream can legitimately stall when the parent reader
    // is blocked behind the session's bounded egress queue (a slow client).
    // That is backpressure, not death: give those writes the same tolerance
    // as the parent's 15-second backpressure verdict. A genuinely gone parent
    // breaks the pipe, which fails the write immediately regardless.
    constexpr DWORD kChildPacketSendTimeoutMs = 10000;

    bool receive(HANDLE pipe, header_t &header, std::vector<std::uint8_t> &payload, DWORD timeout = INFINITE) {
      if (!read_exact(pipe, &header, sizeof(header), timeout)) {
        return false;
      }
      const auto valid_size = [&] {
        switch (header.type) {
          case message_e::start: return header.size == sizeof(start_t);
          case message_e::shutdown:
          case message_e::idr:
          case message_e::chroma_downgrade:
          case message_e::finished:
          case message_e::heartbeat:
          case message_e::encoder_ready:
          case message_e::startup_error:
            return header.size == 0;
          case message_e::capture_ready:
          case message_e::capture_reinitializing: return header.size == sizeof(generation_t);
          case message_e::invalidate: return header.size == sizeof(invalidate_t);
          case message_e::hdr: return header.size == sizeof(video::hdr_info_raw_t);
          case message_e::touch: return header.size == sizeof(input::touch_port_t);
          case message_e::packet:
            return header.size >= sizeof(packet_header_t) && header.size <= 32u * 1024u * 1024u;
        }
        return false;
      }();
      if (!valid_size) return false;
      payload.resize(header.size);
      // Once a valid header has arrived, the payload is already being written
      // by the peer. Short polling timeouts must not abandon a half-read
      // message: partial progress cannot be rewound on a byte-mode pipe.
      const DWORD payload_timeout = std::max<DWORD>(timeout, 5000);
      return !header.size || read_exact(pipe, payload.data(), header.size, payload_timeout);
    }

    std::wstring executable_path() {
      std::wstring path(32768, L'\0');
      const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
      if (!size || size >= path.size()) {
        return {};
      }
      path.resize(size);
      return path;
    }

    std::wstring widen_ascii(std::string_view value) {
      return {value.begin(), value.end()};
    }

    std::string random_pipe_name() {
      std::array<std::uint8_t, 16> random {};
      if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return {};
      }
      constexpr char hex[] = "0123456789abcdef";
      std::string name = "LuminalShineVideo-";
      for (const auto byte : random) {
        name.push_back(hex[byte >> 4]);
        name.push_back(hex[byte & 0x0f]);
      }
      return name;
    }

    bool make_pipe_security(SECURITY_ATTRIBUTES &attributes, PSECURITY_DESCRIPTOR &descriptor) {
      HANDLE token = nullptr;
      if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
      auto close_token = util::fail_guard([&] { ::CloseHandle(token); });
      DWORD bytes = 0;
      (void) ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
      if (!bytes || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
      std::vector<std::uint8_t> storage(bytes);
      if (!::GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) return false;
      const auto *token_user = reinterpret_cast<const TOKEN_USER *>(storage.data());
      LPWSTR sid_text = nullptr;
      if (!::ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) return false;
      auto free_sid = util::fail_guard([&] { ::LocalFree(sid_text); });
      const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_text) + L")";
      if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
      }
      attributes = {sizeof(attributes), descriptor, FALSE};
      return true;
    }

    struct child_t {
      HANDLE pipe {INVALID_HANDLE_VALUE};
      HANDLE process {nullptr};
      HANDLE job {nullptr};
      DWORD pid {0};

      ~child_t() {
        if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
        if (process) ::CloseHandle(process);
        if (job) ::CloseHandle(job);
      }
    };

    bool launch(child_t &child, const std::string &pipe_name) {
      const auto exe = executable_path();
      if (exe.empty()) return false;

      std::wstring command = L"\"" + exe + L"\" --internal-video-worker \"" + widen_ascii(pipe_name) +
                             L"\" " + std::to_wstring(::GetCurrentProcessId());
      STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW)};
      PROCESS_INFORMATION process {};
      if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        BOOST_LOG(error) << "Video worker: CreateProcess failed (err=" << ::GetLastError() << ')';
        return false;
      }
      child.process = process.hProcess;
      child.pid = process.dwProcessId;

      child.job = ::CreateJobObjectW(nullptr, nullptr);
      if (child.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            !::AssignProcessToJobObject(child.job, child.process)) {
          BOOST_LOG(warning) << "Video worker: job assignment failed (err=" << ::GetLastError()
                             << "); explicit termination remains armed.";
        }
      }
      if (::ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        BOOST_LOG(error) << "Video worker: ResumeThread failed (err=" << ::GetLastError() << ')';
        ::TerminateProcess(child.process, 0xE002);
        ::CloseHandle(process.hThread);
        return false;
      }
      ::CloseHandle(process.hThread);
      return true;
    }

    bool establish_child(child_t &child) {
      const std::string pipe_name = random_pipe_name();
      if (pipe_name.empty()) return false;
      const auto full_name = L"\\\\.\\pipe\\" + widen_ascii(pipe_name);
      SECURITY_ATTRIBUTES pipe_security {};
      PSECURITY_DESCRIPTOR pipe_descriptor = nullptr;
      if (!make_pipe_security(pipe_security, pipe_descriptor)) return false;
      auto free_pipe_descriptor = util::fail_guard([&] { ::LocalFree(pipe_descriptor); });
      child.pipe = ::CreateNamedPipeW(full_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                      1, 4 * 1024 * 1024, 64 * 1024, 0, &pipe_security);
      if (child.pipe == INVALID_HANDLE_VALUE || !launch(child, pipe_name)) return false;

      OVERLAPPED connect {};
      connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!connect.hEvent) return false;
      auto close_event = util::fail_guard([&] { ::CloseHandle(connect.hEvent); });
      BOOL connected = ::ConnectNamedPipe(child.pipe, &connect);
      const DWORD connect_error = connected ? ERROR_SUCCESS : ::GetLastError();
      bool connect_ok = connected || connect_error == ERROR_PIPE_CONNECTED;
      if (!connect_ok && connect_error == ERROR_IO_PENDING) {
        HANDLE waits[] {connect.hEvent, child.process};
        connect_ok = ::WaitForMultipleObjects(2, waits, FALSE, 10000) == WAIT_OBJECT_0;
        if (connect_ok) {
          DWORD ignored = 0;
          connect_ok = ::GetOverlappedResult(child.pipe, &connect, &ignored, FALSE) != FALSE;
        }
      }
      if (!connect_ok) {
        (void) ::CancelIoEx(child.pipe, &connect);
        (void) ::WaitForSingleObject(connect.hEvent, INFINITE);
        DWORD ignored = 0;
        (void) ::GetOverlappedResult(child.pipe, &connect, &ignored, FALSE);
        ::TerminateProcess(child.process, 0xE003);
        (void) ::WaitForSingleObject(child.process, 1000);
        return false;
      }
      ULONG client_pid = 0;
      if (!::GetNamedPipeClientProcessId(child.pipe, &client_pid) || client_pid != child.pid) {
        ::TerminateProcess(child.process, 0xE004);
        return false;
      }
      return true;
    }

  }

  bool is_child_process() {
    return !g_child_pipe.empty();
  }

  void set_child_pipe(std::string pipe_name, std::uint32_t parent_pid) {
    g_child_pipe = std::move(pipe_name);
    g_parent_pid = parent_pid;
  }

  std::uint64_t current_capture_generation() {
    return g_capture_generation.load(std::memory_order_acquire);
  }

  bool packet_can_signal_capture_ready(video::packet_raw_t &packet) {
    return packet.is_idr();
  }

  bool packet_metadata_can_enter_capture_generation(
    std::uint64_t packet_generation,
    bool capture_placeholder,
    bool is_idr,
    std::uint64_t active_generation,
    bool generation_needs_idr,
    bool first_admission
  ) {
    if (packet_generation == 0 || packet_generation < active_generation) {
      return false;
    }
    const bool entering_generation = generation_needs_idr || packet_generation > active_generation;
    if (!entering_generation) {
      return true;
    }
    if (!is_idr) {
      return false;
    }
    // The synthetic bootstrap IDR may open the very first generation: it is a
    // decodable frame, and admitting it keeps the client's connection budget
    // independent of how long the capture source takes to produce real video.
    // A generation *transition* (capture reinit) still requires a real
    // captured IDR so a retired encoder can never poison the new chain.
    return !capture_placeholder || first_admission;
  }

  namespace {
    // Single-slot prewarmed worker. Ownership is exclusive at every instant:
    // the slot owns the child until exactly one of adopt/cancel/supersede
    // takes the unique_ptr out under the mutex — the unsafe shared-handle
    // hand-off of the earlier prewarm design cannot recur. The child costs
    // ~1.5-2 s to spawn (process + DLLs + shaders + pipe handshake); paying
    // that during the HTTPS launch phase moves it off the client's post-PLAY
    // first-video budget, which strict Xbox/webOS ports enforce hard.
    std::mutex g_prewarm_mutex;
    std::unique_ptr<child_t> g_prewarmed_child;
    std::chrono::steady_clock::time_point g_prewarm_created {};
    constexpr auto kPrewarmTtl = std::chrono::seconds(60);

    void discard_prewarmed_locked(const char *reason) {
      if (!g_prewarmed_child) return;
      BOOST_LOG(info) << "Video worker: discarding prewarmed process (pid="
                      << g_prewarmed_child->pid << ", " << reason << ").";
      ::TerminateProcess(g_prewarmed_child->process, 0xE007);
      (void) ::WaitForSingleObject(g_prewarmed_child->process, 500);
      g_prewarmed_child.reset();
    }

    std::unique_ptr<child_t> take_prewarmed() {
      std::lock_guard lg(g_prewarm_mutex);
      if (!g_prewarmed_child) return nullptr;
      if (std::chrono::steady_clock::now() - g_prewarm_created > kPrewarmTtl) {
        discard_prewarmed_locked("expired");
        return nullptr;
      }
      if (::WaitForSingleObject(g_prewarmed_child->process, 0) != WAIT_TIMEOUT) {
        discard_prewarmed_locked("exited");
        return nullptr;
      }
      return std::move(g_prewarmed_child);
    }
  }  // namespace

  bool prewarm() {
    if (is_child_process()) return false;
    auto owner = std::make_unique<child_t>();
    if (!establish_child(*owner)) {
      BOOST_LOG(warning) << "Video worker: prewarm could not establish an isolated process; the session will spawn one on demand.";
      return false;
    }
    std::lock_guard lg(g_prewarm_mutex);
    discard_prewarmed_locked("superseded by a newer launch");
    g_prewarmed_child = std::move(owner);
    g_prewarm_created = std::chrono::steady_clock::now();
    BOOST_LOG(info) << "Video worker: prewarmed isolated process (pid="
                    << g_prewarmed_child->pid << ") awaiting session adoption.";
    return true;
  }

  void cancel_prewarm() {
    std::lock_guard lg(g_prewarm_mutex);
    discard_prewarmed_locked("launch cancelled");
  }

  int run_child() {
    const auto full_name = L"\\\\.\\pipe\\" + widen_ascii(g_child_pipe);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
      pipe = ::CreateFileW(full_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) break;
      ::WaitNamedPipeW(full_name.c_str(), 250);
    } while (std::chrono::steady_clock::now() < deadline);
    if (pipe == INVALID_HANDLE_VALUE) return 10;
    auto close_pipe = util::fail_guard([&] { ::CloseHandle(pipe); });

    ULONG server_pid = 0;
    if (!::GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid != g_parent_pid) {
      BOOST_LOG(error) << "Video worker: rejected unexpected pipe server pid=" << server_pid
                       << " expected=" << g_parent_pid;
      return 15;
    }

    header_t header {};
    std::vector<std::uint8_t> payload;
    if (!receive(pipe, header, payload) || header.type != message_e::start || payload.size() != sizeof(start_t)) {
      return 11;
    }
    start_t start {};
    std::memcpy(&start, payload.data(), sizeof(start));

    if (start.magic != kProtocolMagic || start.protocol_version != kProtocolVersion) {
      (void) send(pipe, message_e::startup_error);
      return 18;
    }

    if (start.direct_vgd > 1 || start.safe_capture > 1 || (start.direct_vgd && start.safe_capture) ||
        std::any_of(std::begin(start.reserved), std::end(start.reserved),
                                            [](std::uint8_t value) { return value != 0; })) {
      (void) send(pipe, message_e::startup_error);
      return 19;
    }
    if (start.direct_vgd) {
      const auto display_end = std::find(std::begin(start.vgd_display_name), std::end(start.vgd_display_name), '\0');
      if (!start.vgd_session_id ||
          start.vgd_ring_slots < VDISPLAY::vgd::kMinRingSlots ||
          start.vgd_ring_slots > VDISPLAY::vgd::kMaxRingSlots ||
          (start.vgd_transport_flags & ~VDISPLAY::vgd::kAllowedRingTransportFlags) != 0 ||
          display_end == std::end(start.vgd_display_name) ||
          display_end == std::begin(start.vgd_display_name)) {
        (void) send(pipe, message_e::startup_error);
        return 20;
      }
      const std::string display_name(start.vgd_display_name, display_end);
      VDISPLAY::vgd::set_worker_ring_target(VDISPLAY::vgd::RingTargetInfo {
        start.vgd_session_id, start.vgd_ring_slots, start.vgd_transport_flags, start.vgd_generation
      }, display_name);
      BOOST_LOG(info) << "Video worker: imported direct LuminalVGD ring target session 0x"
                      << std::hex << start.vgd_session_id << std::dec
                      << (start.vgd_generation ? " generation " + std::to_string(start.vgd_generation) : " with deferred generation binding")
                      << " (" << start.vgd_ring_slots << " slots) for " << display_name << '.';
    } else {
      VDISPLAY::vgd::set_worker_ring_target(std::nullopt);
    }
    VDISPLAY::vgd::set_worker_safe_capture(start.safe_capture != 0);

    if (!video::import_encoder_probe_snapshot(start.encoder)) {
      BOOST_LOG(error) << "Video worker: rejected invalid or unavailable encoder capability snapshot.";
      (void) send(pipe, message_e::startup_error);
      return 12;
    }
    if (start.config.videoFormat < 0 || start.config.videoFormat > 2 ||
        start.config.dynamicRange < 0 || start.config.dynamicRange > 1 ||
        start.config.chromaSamplingType < 0 || start.config.chromaSamplingType > 1) {
      (void) send(pipe, message_e::startup_error);
      return 16;
    }
    const auto codec = static_cast<std::size_t>(start.config.videoFormat);
    const auto dynamic_range_mask = std::uint32_t {1} << video::encoder_t::DYNAMIC_RANGE;
    const auto yuv444_mask = std::uint32_t {1} << video::encoder_t::YUV444;
    if (!start.encoder.supported_codec[codec] ||
        (start.config.dynamicRange && !(start.encoder.codec_capabilities[codec] & dynamic_range_mask)) ||
        (start.config.chromaSamplingType && !(start.encoder.codec_capabilities[codec] & yuv444_mask))) {
      BOOST_LOG(error) << "Video worker: requested stream format is inconsistent with the validated encoder snapshot.";
      (void) send(pipe, message_e::startup_error);
      return 17;
    }
    if (!send(pipe, message_e::encoder_ready)) {
      return 14;
    }

    auto session_mail = std::make_shared<safe::mail_raw_t>();
    auto shutdown = session_mail->event<bool>(mail::shutdown);
    auto packets = video::packet_queue(session_mail, mail::video_packets);
    std::atomic_bool commands_done {false};
    std::mutex write_mutex;

    std::thread commands([&] {
      header_t command {};
      std::vector<std::uint8_t> body;
      while (receive(pipe, command, body)) {
        if (command.type == message_e::shutdown) {
          shutdown->raise(true);
          break;
        } else if (command.type == message_e::idr) {
          session_mail->event<bool>(mail::idr)->raise(true);
        } else if (command.type == message_e::invalidate && body.size() == sizeof(invalidate_t)) {
          invalidate_t wire {};
          std::memcpy(&wire, body.data(), sizeof(wire));
          session_mail->event<std::pair<std::int64_t, std::int64_t>>(mail::invalidate_ref_frames)->raise(
            std::make_pair(wire.first, wire.last)
          );
        } else {
          BOOST_LOG(error) << "Video worker: rejected an out-of-state parent IPC message.";
          shutdown->raise(true);
          break;
        }
      }
      packets->stop();
      commands_done.store(true, std::memory_order_release);
      shutdown->raise(true);
    });

    g_capture_reinitializing.store(false, std::memory_order_release);
    g_capture_generation.store(1, std::memory_order_release);
    std::atomic_bool capture_ready_sent {false};
    std::thread output([&] {
      auto hdr = session_mail->event<video::hdr_info_t>(mail::hdr);
      auto touch = session_mail->event<input::touch_port_t>(mail::touch_port);
      auto chroma = session_mail->event<bool>(mail::chroma_downgrade);
      std::uint64_t admitted_generation = 0;
      while (!shutdown->peek()) {
        bool capture_reinitialized = false;
        generation_t reinitialized_generation {};
        {
          std::lock_guard generation_lock(g_capture_generation_mutex);
          capture_reinitialized = g_capture_reinitializing.exchange(false, std::memory_order_acq_rel);
          reinitialized_generation.value = g_capture_generation.load(std::memory_order_acquire);
        }
        if (capture_reinitialized) {
          // Retire every encoded packet queued by the old capture device before
          // announcing the new generation. It is better to wait for one fresh
          // IDR than to poison the client's new reference tree with an old WGC
          // desktop or an already-destroyed IddCx swapchain.
          while (packets->pop(0ms)) {}
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::capture_reinitializing, &reinitialized_generation, sizeof(reinitialized_generation), kChildPacketSendTimeoutMs)) break;
          continue;
        }
        if (auto packet = packets->pop(20ms)) {
          std::lock_guard generation_lock(g_capture_generation_mutex);
          // A reinit can race the queue pop. Drop this now-stale packet; the
          // next loop drains the old generation and publishes its marker
          // before any replacement packet is tagged or sent.
          if (g_capture_reinitializing.load(std::memory_order_acquire)) {
            continue;
          }
          const auto current_generation = g_capture_generation.load(std::memory_order_acquire);
          const bool generation_needs_idr = admitted_generation != current_generation;
          if (packet->capture_generation != current_generation ||
              !packet_metadata_can_enter_capture_generation(
                packet->capture_generation,
                packet->capture_placeholder,
                packet->is_idr(),
                current_generation,
                generation_needs_idr,
                admitted_generation == 0
              )) {
            // A late packet from an encoder retired during reinit must not be
            // relabelled as belonging to the replacement generation. The one
            // synthetic bootstrap IDR is admissible only before any packet has
            // entered the pipe: it keeps the client connected while the real
            // capture source finishes starting.
            continue;
          }
          if (generation_needs_idr) admitted_generation = current_generation;
          auto encoded_bytes = materialize_packet(*packet);
          if (encoded_bytes.empty()) {
            BOOST_LOG(error) << "Video worker: encoded packet exceeded the 32 MiB IPC limit.";
            shutdown->raise(true);
            break;
          }
          packet_header_t packet_header {};
          packet_header.frame_index = packet->frame_index();
          packet_header.frame_timestamp_ns = packet->frame_timestamp ? serialize_timestamp(*packet->frame_timestamp) : 0;
          packet_header.host_processing_timestamp_ns = packet->host_processing_timestamp ? serialize_timestamp(*packet->host_processing_timestamp) : 0;
          packet_header.data_size = static_cast<std::uint32_t>(encoded_bytes.size());
          packet_header.idr = static_cast<std::uint8_t>(packet->is_idr());
          packet_header.after_rfi = static_cast<std::uint8_t>(packet->after_ref_frame_invalidation);
          packet_header.has_frame_timestamp = static_cast<std::uint8_t>(packet->frame_timestamp.has_value());
          packet_header.has_host_processing_timestamp = static_cast<std::uint8_t>(packet->host_processing_timestamp.has_value());
          packet_header.capture_placeholder = static_cast<std::uint8_t>(packet->capture_placeholder);
          packet_header.capture_generation = packet->capture_generation;
          std::vector<std::uint8_t> body(sizeof(packet_header) + packet_header.data_size);
          std::memcpy(body.data(), &packet_header, sizeof(packet_header));
          std::memcpy(body.data() + sizeof(packet_header), encoded_bytes.data(), packet_header.data_size);
          std::lock_guard lock(write_mutex);
          if (!capture_ready_sent.load(std::memory_order_acquire)) {
            // Readiness means a decodable IDR exists — possibly the synthetic
            // bootstrap frame. The client is admitted immediately; real-frame
            // progress is watched separately by the parent supervisor.
            const generation_t generation {packet_header.capture_generation};
            if (!send(pipe, message_e::capture_ready, &generation, sizeof(generation), kChildPacketSendTimeoutMs)) break;
            capture_ready_sent.store(true, std::memory_order_release);
          }
          if (!send(pipe, message_e::packet, body.data(), static_cast<std::uint32_t>(body.size()), kChildPacketSendTimeoutMs)) break;
        }
        if (auto value = hdr->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::hdr, value.get(), sizeof(video::hdr_info_raw_t), kChildPacketSendTimeoutMs)) break;
        }
        if (auto value = touch->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::touch, &*value, sizeof(*value), kChildPacketSendTimeoutMs)) break;
        }
        if (chroma->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::chroma_downgrade, nullptr, 0, kChildPacketSendTimeoutMs)) break;
        }
      }
      // A failed pipe write means the parent is gone or has given up on this
      // worker. Continuing to capture/encode into a queue nobody drains only
      // delays teardown — end the child's capture session promptly.
      packets->stop();
      shutdown->raise(true);
    });

    video::capture(session_mail, start.config, nullptr);
    if (!capture_ready_sent.load(std::memory_order_acquire)) {
      std::lock_guard lock(write_mutex);
      (void) send(pipe, message_e::startup_error);
    }
    shutdown->raise(true);
    {
      std::lock_guard lock(write_mutex);
      (void) send(pipe, message_e::finished);
    }
    (void) ::CancelIoEx(pipe, nullptr);
    ::CloseHandle(pipe);
    close_pipe.disable();
    if (commands.joinable()) commands.join();
    if (output.joinable()) output.join();
    return 0;
  }

  bool capture(safe::mail_t mail, video::config_t config, void *channel_data) {
    if (is_child_process()) return false;

    thread_local bool bounded_retry_active = false;
    auto shutdown = mail->event<bool>(mail::shutdown);

    const bool direct_vgd = VDISPLAY::is_luminalvgd_active();
    start_t start {};
    start.magic = kProtocolMagic;
    start.protocol_version = kProtocolVersion;
    start.config = config;
    if (!video::export_encoder_probe_snapshot(start.encoder)) {
      if (direct_vgd) {
        // No pipeline of any kind will run for a direct-VGD session. End it
        // promptly (also releases a strict client's held ANNOUNCE) instead of
        // leaving it to linger until the ping windows expire.
        BOOST_LOG(error) << "Video worker: parent has no validated encoder snapshot; ending the direct-VGD session.";
        shutdown->raise(true);
        return true;
      }
      BOOST_LOG(error) << "Video worker: parent has no validated encoder snapshot; using in-process pipeline.";
      return false;
    }
    if (direct_vgd && bounded_retry_active) {
      // The first isolated worker already failed after the parent had admitted
      // a direct ring. Repeating the identical direct start in a fresh child
      // loses the child-local failure latch and strands the client twice.
      // The single bounded retry is therefore the connection-safe backend.
      start.safe_capture = 1;
      BOOST_LOG(warning) << "Video worker: bounded retry is forcing isolated WGC safe-capture.";
    } else if (direct_vgd) {
      const auto display_name = display_device::map_output_name(::config::get_active_output_name());
      const auto target = VDISPLAY::vgd::ring_target_for_worker_display(display_name);
      if (!target) {
        BOOST_LOG(warning) << "Video worker: direct LuminalVGD session has no transferable ring target for '"
                           << display_name << "'; starting isolated WGC safe-capture instead.";
        start.safe_capture = 1;
      } else {
        if (display_name.size() >= sizeof(start.vgd_display_name)) {
          BOOST_LOG(warning) << "Video worker: direct LuminalVGD display name exceeds the IPC limit; "
                                "starting isolated WGC safe-capture instead.";
          start.safe_capture = 1;
        } else {
          start.direct_vgd = 1;
          start.vgd_session_id = target->session_id;
          start.vgd_ring_slots = target->ring_slots;
          start.vgd_transport_flags = target->transport_flags;
          start.vgd_generation = target->generation;
          std::copy(display_name.begin(), display_name.end(), std::begin(start.vgd_display_name));
        }
      }
    }

    // Adopt the prewarmed worker when one is waiting: ownership transfers
    // atomically out of the single slot, so this session's video thread holds
    // every kernel handle exclusively for the full capture lifetime. Fall back
    // to spawning on demand when no (live) prewarmed process exists.
    std::unique_ptr<child_t> child_owner = take_prewarmed();
    if (child_owner) {
      BOOST_LOG(info) << "Video worker: adopted prewarmed isolated process (pid=" << child_owner->pid << ").";
    } else {
      child_owner = std::make_unique<child_t>();
      if (!establish_child(*child_owner)) {
        if (direct_vgd) {
          // Same as the snapshot path: nothing will produce video for this
          // session — end it promptly rather than burning the strict-client
          // ANNOUNCE hold and the ping windows on a known-dead session.
          BOOST_LOG(error) << "Video worker: failed to establish isolated process; ending the direct-VGD session.";
          shutdown->raise(true);
          return true;
        }
        BOOST_LOG(error) << "Video worker: failed to establish isolated process; using in-process pipeline.";
        return false;
      }
      BOOST_LOG(info) << "Video worker: isolated capture/encode process started (pid=" << child_owner->pid << ")";
    }
    child_t &child = *child_owner;
    if (!send(child.pipe, message_e::start, &start, sizeof(start))) {
      BOOST_LOG(error) << "Video worker: failed to send startup state; using in-process pipeline.";
      ::TerminateProcess(child.process, 0xE005);
      (void) ::WaitForSingleObject(child.process, 1000);
      return false;
    }

    // Process launch, DLL loading and startup-state validation have their own
    // deadline. The first-packet clock starts only after the child explicitly
    // accepts the startup state; never charge shader/D3D/NVENC initialization
    // against a misleading 1.5-second catch-all timeout.
    header_t ready_header {};
    std::vector<std::uint8_t> ready_payload;
    bool startup_reinitializing = false;
    const auto publish_sideband = [&](message_e type, const std::vector<std::uint8_t> &body) {
      if (type == message_e::hdr && body.size() == sizeof(video::hdr_info_raw_t)) {
        video::hdr_info_raw_t value {false};
        std::memcpy(&value, body.data(), sizeof(value));
        mail->event<video::hdr_info_t>(mail::hdr)->raise(std::make_unique<video::hdr_info_raw_t>(value));
        return true;
      }
      if (type == message_e::touch && body.size() == sizeof(input::touch_port_t)) {
        input::touch_port_t value {};
        std::memcpy(&value, body.data(), sizeof(value));
        mail->event<input::touch_port_t>(mail::touch_port)->raise(value);
        return true;
      }
      if (type == message_e::chroma_downgrade && body.empty()) {
        mail->event<bool>(mail::chroma_downgrade)->raise(true);
        return true;
      }
      if (type == message_e::capture_reinitializing && body.size() == sizeof(generation_t)) {
        // WGC can observe the final Windows topology normalization at the same
        // time it produces its bootstrap IDR. Preserve that transition across
        // FIRST_PACKET; otherwise steady-state starts with the ordinary
        // three-second watchdog and kills the replacement encoder mid-build.
        startup_reinitializing = true;
        return true;
      }
      return type == message_e::heartbeat && body.empty();
    };

    bool startup_message = false;
    // PROCESS_READY covers child spawn + DLL load + startup validation only
    // (~2 s observed); FIRST_PACKET additionally covers capture-source
    // admission and NVENC session creation, which takes ~7 s on some
    // driver/GPU combinations (observed on RTX 5080). The client-side
    // no-video budget is protected independently by videoThread's late
    // keepalive datagram, so these deadlines guard genuine dysfunction, not
    // the client clock.
    const auto process_ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (!shutdown->peek() && std::chrono::steady_clock::now() < process_ready_deadline) {
      if (receive(child.pipe, ready_header, ready_payload, 100)) {
        startup_message = true;
        break;
      }
    }
    const bool encoder_ready = startup_message && ready_header.type == message_e::encoder_ready;
    if (encoder_ready) {
      BOOST_LOG(info) << "Video worker: PROCESS_READY and startup state accepted; waiting for first encoded packet.";
    }
    bool first_packet_message = false;
    bool capture_ready = false;
    std::uint64_t startup_generation = 0;
    bool startup_failed = false;
    if (encoder_ready) {
      // Wide enough for the slowest observed healthy bring-up: 4K HDR NVENC
      // session creation alone takes ~11 s on this driver, and the 15-second
      // deadline was killing workers ~2 s before success. Client budgets are
      // protected separately (ANNOUNCE hold + runt keepalive); this deadline
      // only guards genuine dysfunction.
      auto first_packet_deadline = std::chrono::steady_clock::now() + 30s;
      while (!shutdown->peek() && std::chrono::steady_clock::now() < first_packet_deadline) {
        if (startup_reinitializing) {
          // A capture reinit during bootstrap retires the one synthetic
          // placeholder, so the next packet requires a real captured frame.
          // Grant the same bounded rebuild window steady state gets instead
          // of terminating a healthy worker at the short bootstrap deadline.
          first_packet_deadline = std::max(first_packet_deadline, std::chrono::steady_clock::now() + 12s);
          startup_reinitializing = false;
        }
        if (!receive(child.pipe, ready_header, ready_payload, 100)) continue;
        first_packet_message = true;
        if (ready_header.type == message_e::capture_ready) {
          generation_t generation {};
          std::memcpy(&generation, ready_payload.data(), sizeof(generation));
          startup_generation = generation.value;
          capture_ready = true;
          break;
        }
        if (ready_header.type == message_e::startup_error || ready_header.type == message_e::finished) {
          startup_failed = true;
          break;
        }
        // Sideband notifications are asynchronous and may legitimately precede
        // the first encoded packet. Publish them now instead of mistaking them
        // for a startup failure or losing one-shot HDR/touch state.
        if (!publish_sideband(ready_header.type, ready_payload)) {
          startup_failed = true;
          break;
        }
      }
    }
    if (!capture_ready) {
      if (!startup_message) {
        BOOST_LOG(error) << "Video worker: PROCESS_READY deadline expired or startup pipe closed before acknowledgement.";
      } else if (!encoder_ready) {
        BOOST_LOG(error) << "Video worker: rejected startup state (message="
                         << static_cast<std::uint32_t>(ready_header.type) << ").";
      } else if (shutdown->peek()) {
        BOOST_LOG(info) << "Video worker: first-packet wait cancelled because the client session ended.";
      } else if (!first_packet_message) {
        BOOST_LOG(error) << "Video worker: FIRST_PACKET deadline expired or worker pipe closed after startup acceptance.";
      } else if (ready_header.type == message_e::startup_error) {
        BOOST_LOG(error) << "Video worker: capture or encoder initialization ended before the first encoded packet.";
      } else if (ready_header.type == message_e::finished) {
        BOOST_LOG(error) << "Video worker: capture pipeline finished before producing its first encoded packet.";
      } else if (startup_failed) {
        BOOST_LOG(error) << "Video worker: expected FIRST_PACKET readiness but received message="
                         << static_cast<std::uint32_t>(ready_header.type) << '.';
      }
      (void) ::CancelIoEx(child.pipe, nullptr);
      ::TerminateProcess(child.process, 0xE006);
      const bool worker_reaped = ::WaitForSingleObject(child.process, 3000) == WAIT_OBJECT_0;
      ::DisconnectNamedPipe(child.pipe);
      if (!worker_reaped) {
        BOOST_LOG(error) << "Video worker: process remains stuck after termination; refusing unsafe in-process GPU fallback.";
        shutdown->raise(true);
        return true;
      }
      const auto health = platf::dxgi::D3D11ProbeDeviceHealth();
      if (FAILED(health)) {
        BOOST_LOG(error) << "Video worker: display health probe failed after worker termination (hresult=0x"
                         << std::hex << health << std::dec << "); refusing unsafe in-process GPU fallback.";
        shutdown->raise(true);
        return true;
      }
      if (direct_vgd && !shutdown->peek() && !bounded_retry_active) {
        BOOST_LOG(warning) << "Video worker: display health is good; keeping the client session alive for one bounded isolated-worker retry.";
        bounded_retry_active = true;
        const auto reset_retry = util::fail_guard([&] { bounded_retry_active = false; });
        return platf::video_worker::capture(std::move(mail), config, channel_data);
      }
      if (direct_vgd) {
        BOOST_LOG(error) << "Video worker: bounded isolated retry exhausted; refusing unsafe in-process GPU fallback.";
        // No pipeline will exist for this session. End it promptly so the
        // client sees a clean failure instead of a silent zero-video stream.
        shutdown->raise(true);
        return true;
      }
      BOOST_LOG(warning) << "Video worker: display health is good; using the in-process pipeline for the non-VGD display.";
      return false;
    }
    BOOST_LOG(info) << "Video worker: FIRST_PACKET received; isolated capture pipeline ready.";
    // Releases the strict-client ANNOUNCE hold: the first encoded packet
    // exists, so the client's post-PLAY no-video clock can start safely.
    mail->event<bool>(mail::video_pipeline_ready)->raise(true);

    const auto now_ms = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    std::atomic_bool reader_done {false};
    std::atomic<std::int64_t> last_frame_ms {0};
    std::atomic<std::int64_t> reinitializing_until_ms {
      startup_reinitializing ? now_ms() + 12000 : 0
    };
    std::atomic_bool preserve_reinit_across_bootstrap_packet {startup_reinitializing};
    std::atomic_bool first_frame_seen {false};
    std::atomic<std::uint64_t> active_capture_generation {startup_generation};
    std::atomic_bool generation_needs_idr {true};
    std::atomic_bool network_attached {mail->event<bool>(mail::video_peer_ready)->peek()};
    // Recovery requests are deliberately owned by this parent process.  The
    // client can report every missing reference frame separately, but passing
    // that burst straight through to NVENC creates an IDR feedback loop: each
    // oversized invalidation becomes another IDR and induces yet more loss.
    std::atomic_bool recovery_idr_inflight {false};
    std::atomic_bool recovery_idr_seen {false};
    // CAPTURE_READY means the child has already produced a bootstrap IDR even
    // though the reader has not consumed its packet yet. Seed the timestamp so
    // the client's mandatory startup request cannot create a duplicate IDR.
    std::atomic<std::int64_t> last_idr_ms {now_ms()};
    std::atomic<std::uint64_t> last_idr_frame {0};
    last_frame_ms.store(now_ms(), std::memory_order_release);

    auto packets = video::packet_queue(mail, mail::video_packets);
    std::thread reader([&] {
      header_t header {};
      std::vector<std::uint8_t> body;
      video::packet_t bootstrap_idr;
      bool any_packet_admitted = false;
      while (receive(child.pipe, header, body)) {
        if (header.type == message_e::finished) break;
        if (header.type == message_e::packet && body.size() >= sizeof(packet_header_t)) {
          packet_header_t packet_header {};
          std::memcpy(&packet_header, body.data(), sizeof(packet_header));
          if (packet_header.data_size <= body.size() - sizeof(packet_header) &&
              body.size() - sizeof(packet_header) == packet_header.data_size) {
            const auto active_generation = active_capture_generation.load(std::memory_order_acquire);
            const bool needs_generation_idr = generation_needs_idr.load(std::memory_order_acquire);
            if (!packet_metadata_can_enter_capture_generation(
                  packet_header.capture_generation,
                  packet_header.capture_placeholder != 0,
                  packet_header.idr != 0,
                  active_generation,
                  needs_generation_idr,
                  !any_packet_admitted
                )) {
              continue;
            }
            any_packet_admitted = true;
            if (packet_header.capture_generation > active_generation) {
              // A packet can win the IPC race against its sideband marker. The
              // predicate above permits that only at a real captured IDR.
              active_capture_generation.store(packet_header.capture_generation, std::memory_order_release);
            }
            if (packet_header.idr) generation_needs_idr.store(false, std::memory_order_release);
            std::vector<std::uint8_t> bytes(packet_header.data_size);
            std::memcpy(bytes.data(), body.data() + sizeof(packet_header), bytes.size());
            auto packet = std::make_unique<video::packet_raw_generic>(std::move(bytes), packet_header.frame_index, packet_header.idr != 0);
            packet->after_ref_frame_invalidation = packet_header.after_rfi != 0;
            packet->capture_placeholder = packet_header.capture_placeholder != 0;
            if (packet_header.has_frame_timestamp) {
              packet->frame_timestamp = deserialize_timestamp(packet_header.frame_timestamp_ns);
            }
            if (packet_header.has_host_processing_timestamp) {
              packet->host_processing_timestamp = deserialize_timestamp(packet_header.host_processing_timestamp_ns);
            }
            packet->channel_data = channel_data;
            if (!packet->capture_placeholder) {
              first_frame_seen.store(true, std::memory_order_release);
              last_frame_ms.store(now_ms(), std::memory_order_release);
            }
            if (packet->is_idr()) {
              last_idr_ms.store(now_ms(), std::memory_order_release);
              last_idr_frame.store(packet_header.frame_index, std::memory_order_release);
            }
            if (!network_attached.load(std::memory_order_acquire)) {
              // Retain only a decodable bootstrap point.  Continuing to drain
              // the child prevents its bounded IPC pipe from back-pressuring
              // NVENC while RTSP authenticates the UDP destination.
              if (packet->is_idr()) bootstrap_idr = std::move(packet);
            } else {
              if (bootstrap_idr) {
                // The retained bootstrap IDR was captured while NVENC was
                // still initializing (5-11 s on this host) — its capture
                // timestamps are honestly ancient, but it is an intentional
                // pre-buffered decode point, not pipeline backlog. Strip
                // the timestamps so the egress age metric skips it (same
                // treatment as minimum-FPS duplicates) instead of opening
                // every session with a multi-second "pipeline backlog"
                // warning.
                bootstrap_idr->frame_timestamp.reset();
                bootstrap_idr->host_processing_timestamp.reset();
                packets->raise(std::move(bootstrap_idr));
              }
              packets->raise(std::move(packet));
            }
            if (!preserve_reinit_across_bootstrap_packet.exchange(false, std::memory_order_acq_rel)) {
              reinitializing_until_ms.store(0, std::memory_order_release);
            } else {
              BOOST_LOG(info) << "Video worker: bootstrap packet preceded a capture reinitialization; retaining the bounded rebuild window until the replacement packet.";
            }
          }
        } else if (header.type == message_e::hdr && body.size() == sizeof(video::hdr_info_raw_t)) {
          video::hdr_info_raw_t value {false};
          std::memcpy(&value, body.data(), sizeof(value));
          mail->event<video::hdr_info_t>(mail::hdr)->raise(std::make_unique<video::hdr_info_raw_t>(value));
        } else if (header.type == message_e::touch && body.size() == sizeof(input::touch_port_t)) {
          input::touch_port_t value {};
          std::memcpy(&value, body.data(), sizeof(value));
          mail->event<input::touch_port_t>(mail::touch_port)->raise(value);
        } else if (header.type == message_e::chroma_downgrade) {
          mail->event<bool>(mail::chroma_downgrade)->raise(true);
        } else if (header.type == message_e::capture_reinitializing && body.size() == sizeof(generation_t)) {
          generation_t generation {};
          std::memcpy(&generation, body.data(), sizeof(generation));
          const auto previous = active_capture_generation.load(std::memory_order_acquire);
          if (generation.value > previous) {
            active_capture_generation.store(generation.value, std::memory_order_release);
            generation_needs_idr.store(true, std::memory_order_release);
            recovery_idr_inflight.store(false, std::memory_order_release);
            recovery_idr_seen.store(false, std::memory_order_release);
            // A retained bootstrap frame belongs to the retired generation; it
            // must never become the first frame a late-attaching client sees.
            bootstrap_idr.reset();
          }
          // A topology or desktop switch invalidates WGC and its NVENC device.
          // Rebuilding a 4K HDR encoder takes 4-6 seconds on the observed host;
          // advertise a bounded grace period rather than applying the normal
          // three-second steady-state stall verdict.
          reinitializing_until_ms.store(now_ms() + 12000, std::memory_order_release);
          BOOST_LOG(info) << "Video worker: capture pipeline is reinitializing; granting a bounded 12-second frame-progress window.";
        } else if (header.type != message_e::heartbeat) {
          BOOST_LOG(error) << "Video worker: rejected an out-of-state child IPC message.";
          break;
        }
      }
      reader_done.store(true, std::memory_order_release);
    });

    auto idr = mail->event<bool>(mail::idr);
    auto invalidate = mail->event<std::pair<std::int64_t, std::int64_t>>(mail::invalidate_ref_frames);
    auto discontinuity = mail->event<bool>(mail::video_discontinuity);
    auto submitted_idr = mail->event<std::int64_t>(mail::video_idr_submitted);
    auto peer_ready = mail->event<bool>(mail::video_peer_ready);
    bool poisoned = false;
    // Suppress the protocol-mandated startup request: CAPTURE_READY already
    // guarantees a decodable IDR exists in the pipe.
    auto suppress_recovery_until = std::chrono::steady_clock::now() + 1000ms;
    auto recovery_sent_at = std::chrono::steady_clock::time_point {};
    std::int64_t recovery_after_frame = -1;
    std::uint64_t coalesced_invalidations = 0;
    while (!shutdown->peek() && !reader_done.load(std::memory_order_acquire)) {
      if (!network_attached.load(std::memory_order_acquire) && peer_ready->peek()) {
        network_attached.store(true, std::memory_order_release);
        // The reader releases its retained bootstrap IDR before the next live
        // packet.  Do not manufacture a second startup IDR here.
        BOOST_LOG(info) << "Video worker: network consumer attached to prewarmed encoder.";
      }

      const auto recovery_now = std::chrono::steady_clock::now();
      while (auto frame = submitted_idr->pop(0ms)) {
        if (recovery_idr_inflight.load(std::memory_order_acquire) &&
            *frame > recovery_after_frame) {
          recovery_idr_seen.store(true, std::memory_order_release);
        }
      }
      if (recovery_idr_seen.exchange(false, std::memory_order_acq_rel)) {
        recovery_idr_inflight.store(false, std::memory_order_release);
        // Keep discarding reports about frames preceding the recovery point
        // while that IDR traverses the broadcaster and client decoder.
        suppress_recovery_until = recovery_now + 1000ms;
        while (invalidate->pop(0ms)) ++coalesced_invalidations;
        BOOST_LOG(info) << "Video worker: recovery IDR submitted to UDP; suppressing obsolete reference invalidations for 1 second.";
      } else if (recovery_idr_inflight.load(std::memory_order_acquire) &&
                 recovery_now - recovery_sent_at > 1500ms) {
        // A lost/failed recovery IDR must not suppress recovery forever.
        recovery_idr_inflight.store(false, std::memory_order_release);
        suppress_recovery_until = recovery_now + 250ms;
        BOOST_LOG(warning) << "Video worker: recovery IDR acknowledgement timed out; reopening the bounded recovery gate.";
      }

      const bool explicit_idr = idr->pop(25ms);
      const bool host_discontinuity = discontinuity->pop(0ms);
      bool valid_invalidation = false;
      std::int64_t merged_first = std::numeric_limits<std::int64_t>::max();
      std::int64_t merged_last = -1;
      while (auto range = invalidate->pop(0ms)) {
        ++coalesced_invalidations;
        if (range->first < 0 || range->second < range->first ||
            recovery_idr_inflight.load(std::memory_order_acquire) ||
            recovery_now < suppress_recovery_until) {
          continue;
        }
        const auto recovery_frame = last_idr_frame.load(std::memory_order_acquire);
        if (recovery_frame != 0 &&
            static_cast<std::uint64_t>(range->second) <= recovery_frame) {
          // The client report names only frames preceding an IDR that has
          // already left the encoder. It cannot affect the new reference tree.
          continue;
        }
        valid_invalidation = true;
        merged_first = std::min(merged_first, range->first);
        merged_last = std::max(merged_last, range->second);
      }

      const auto recent_idr_ms = now_ms() - last_idr_ms.load(std::memory_order_acquire);
      // React to the first missing reference immediately. Waiting for a quiet
      // interval is incorrect because Moonlight repeats/extends the range once
      // per damaged frame, so a 100 ms debounce can postpone recovery forever.
      // Host-detected discontinuities obey the SAME bounded cooldown as client
      // reports: letting them bypass it turned a single misdetection into a
      // self-sustaining ~2 Hz IDR storm (every recovery IDR arrived "late",
      // re-triggering the detector while all deltas were withheld).
      const bool recovery_requested = host_discontinuity || explicit_idr || valid_invalidation;
      if (recovery_requested && network_attached.load(std::memory_order_acquire) &&
          !generation_needs_idr.load(std::memory_order_acquire) &&
          !recovery_idr_inflight.load(std::memory_order_acquire) &&
          recovery_now >= suppress_recovery_until && recent_idr_ms >= 1000) {
        // One IDR is a complete recovery point.  It supersedes the entire
        // merged invalidation range, so never feed that range to NVENC too.
        recovery_idr_inflight.store(true, std::memory_order_release);
        recovery_sent_at = recovery_now;
        recovery_after_frame = static_cast<std::int64_t>(
          last_idr_frame.load(std::memory_order_acquire)
        );
        if (!send(child.pipe, message_e::idr)) {
          BOOST_LOG(error) << "Video worker: failed to send the bounded recovery IDR command.";
          poisoned = true;
          break;
        }
        BOOST_LOG(info) << "Video worker: issued one immediate bounded recovery IDR"
                        << (host_discontinuity ? " for a host transport discontinuity after frame " + std::to_string(recovery_after_frame) : "")
                        << " after coalescing "
                        << coalesced_invalidations << " reference invalidation request(s)"
                        << (valid_invalidation ? " for frames " + std::to_string(merged_first) + "-" + std::to_string(merged_last) : "")
                        << '.';
        coalesced_invalidations = 0;
      }
      const auto silence_ms = now_ms() - last_frame_ms.load(std::memory_order_acquire);
      const auto reinit_until = reinitializing_until_ms.load(std::memory_order_acquire);
      if (reinit_until > now_ms()) {
        continue;
      }
      const auto deadline_ms = first_frame_seen.load(std::memory_order_acquire) ? 3000 : 7000;
      if (silence_ms > deadline_ms) {
        // A full — or merely non-empty — egress queue means encoded frames
        // exist but the client is draining slowly (Wi-Fi congestion, TV
        // decoder stall). That is not a GPU failure: degrade instead of
        // disconnecting, and only a very long sustained stall ends the client.
        // peek() closes the momentary not-full race while the blocked reader
        // completes its raise.
        if (packets->at_capacity() || packets->peek()) {
          if (silence_ms > 15000) {
            BOOST_LOG(error) << "Video worker: session egress remained backpressured for "
                             << silence_ms
                             << " ms; ending this client without classifying the healthy capture worker as a GPU failure.";
            break;
          }
          continue;
        }
        if (reinit_until != 0) {
          BOOST_LOG(error) << "Video worker: bounded capture reinitialization expired without an encoded frame.";
        }
        BOOST_LOG(error) << "Video worker: encoded-frame progress stalled for " << silence_ms
                         << " ms; terminating poisoned GPU worker without blocking the host.";
        poisoned = true;
        break;
      }
    }
    if (!poisoned) {
      (void) send(child.pipe, message_e::shutdown);
    } else {
      ::TerminateProcess(child.process, 0xE001);
    }
    if (::WaitForSingleObject(child.process, 2000) == WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Video worker: bounded shutdown expired; terminating worker process.";
      ::TerminateProcess(child.process, 0xE001);
      (void) ::WaitForSingleObject(child.process, 500);
    }
    // The parent reader may be waiting to enqueue into this session's bounded
    // egress queue. Stop it before joining so transport teardown can never be
    // mistaken for, or converted into, a stuck GPU worker.
    packets->stop();
    (void) ::CancelIoEx(child.pipe, nullptr);
    ::DisconnectNamedPipe(child.pipe);
    if (reader.joinable()) reader.join();
    BOOST_LOG(info) << "Video worker: isolated capture/encode process ended.";
    return true;
  }

  void notify_capture_reinitializing() {
    if (is_child_process()) {
      std::lock_guard generation_lock(g_capture_generation_mutex);
      g_capture_generation.fetch_add(1, std::memory_order_acq_rel);
      g_capture_reinitializing.store(true, std::memory_order_release);
    }
  }
}
