#include "video_worker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <mutex>
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
    };

#pragma pack(push, 1)
    struct header_t {
      message_e type;
      std::uint32_t size;
    };
    struct packet_header_t {
      std::int64_t frame_index;
      std::uint32_t data_size;
      std::uint8_t idr;
      std::uint8_t after_rfi;
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
    constexpr std::uint32_t kProtocolVersion = 4;
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

    bool wait_overlapped(HANDLE pipe, OVERLAPPED &overlapped, DWORD timeout, DWORD &transferred) {
      const DWORD wait = ::WaitForSingleObject(overlapped.hEvent, timeout);
      if (wait != WAIT_OBJECT_0) {
        (void) ::CancelIoEx(pipe, &overlapped);
        // OVERLAPPED and its event are stack-owned. They must remain alive
        // until cancellation is observed as complete.
        (void) ::WaitForSingleObject(overlapped.hEvent, INFINITE);
        DWORD ignored = 0;
        (void) ::GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
        return false;
      }
      return ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    }

    bool write_exact(HANDLE pipe, const void *data, std::uint32_t size) {
      const auto *cursor = static_cast<const std::uint8_t *>(data);
      while (size) {
        DWORD written = 0;
        OVERLAPPED overlapped {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        const BOOL started = ::WriteFile(pipe, cursor, size, &written, &overlapped);
        const DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
        const bool ok = started || (error == ERROR_IO_PENDING && wait_overlapped(pipe, overlapped, 500, written));
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
      while (size) {
        DWORD read = 0;
        OVERLAPPED overlapped {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        const BOOL started = ::ReadFile(pipe, cursor, size, &read, &overlapped);
        const DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
        const bool ok = started || (error == ERROR_IO_PENDING && wait_overlapped(pipe, overlapped, timeout, read));
        ::CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) {
          return false;
        }
        cursor += read;
        size -= read;
      }
      return true;
    }

    bool send(HANDLE pipe, message_e type, const void *payload = nullptr, std::uint32_t size = 0) {
      const header_t header {type, size};
      return write_exact(pipe, &header, sizeof(header)) && (!size || write_exact(pipe, payload, size));
    }

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
          case message_e::capture_ready:
          case message_e::startup_error: return header.size == 0;
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
      return !header.size || read_exact(pipe, payload.data(), header.size, timeout);
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
  }

  bool is_child_process() {
    return !g_child_pipe.empty();
  }

  void set_child_pipe(std::string pipe_name, std::uint32_t parent_pid) {
    g_child_pipe = std::move(pipe_name);
    g_parent_pid = parent_pid;
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
          start.vgd_generation == 0 || display_end == std::end(start.vgd_display_name) ||
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
                      << " generation " << start.vgd_generation
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
      commands_done.store(true, std::memory_order_release);
      shutdown->raise(true);
    });

    std::thread output([&] {
      auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
      auto hdr = session_mail->event<video::hdr_info_t>(mail::hdr);
      auto touch = session_mail->event<input::touch_port_t>(mail::touch_port);
      auto chroma = session_mail->event<bool>(mail::chroma_downgrade);
      bool capture_ready_sent = false;
      while (!shutdown->peek()) {
        if (auto packet = packets->pop(20ms)) {
          auto encoded_bytes = materialize_packet(*packet);
          if (encoded_bytes.empty()) {
            BOOST_LOG(error) << "Video worker: encoded packet exceeded the 32 MiB IPC limit.";
            shutdown->raise(true);
            break;
          }
          packet_header_t packet_header {
            packet->frame_index(), static_cast<std::uint32_t>(encoded_bytes.size()),
            static_cast<std::uint8_t>(packet->is_idr()),
            static_cast<std::uint8_t>(packet->after_ref_frame_invalidation)
          };
          std::vector<std::uint8_t> body(sizeof(packet_header) + packet_header.data_size);
          std::memcpy(body.data(), &packet_header, sizeof(packet_header));
          std::memcpy(body.data() + sizeof(packet_header), encoded_bytes.data(), packet_header.data_size);
          std::lock_guard lock(write_mutex);
          if (!capture_ready_sent) {
            // This is truthful readiness: display capture, conversion and the
            // encoder have all succeeded and a valid first packet exists.
            if (!send(pipe, message_e::capture_ready)) break;
            capture_ready_sent = true;
          }
          if (!send(pipe, message_e::packet, body.data(), static_cast<std::uint32_t>(body.size()))) break;
        }
        if (auto value = hdr->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::hdr, value.get(), sizeof(video::hdr_info_raw_t))) break;
        }
        if (auto value = touch->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::touch, &*value, sizeof(*value))) break;
        }
        if (chroma->pop(0ms)) {
          std::lock_guard lock(write_mutex);
          if (!send(pipe, message_e::chroma_downgrade)) break;
        }
      }
    });

    video::capture(session_mail, start.config, nullptr);
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

    const bool direct_vgd = VDISPLAY::is_luminalvgd_active();
    start_t start {};
    start.magic = kProtocolMagic;
    start.protocol_version = kProtocolVersion;
    start.config = config;
    if (!video::export_encoder_probe_snapshot(start.encoder)) {
      BOOST_LOG(error) << "Video worker: parent has no validated encoder snapshot; using in-process pipeline.";
      return direct_vgd;
    }
    if (direct_vgd) {
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

    const std::string pipe_name = random_pipe_name();
    if (pipe_name.empty()) return direct_vgd;
    const auto full_name = L"\\\\.\\pipe\\" + widen_ascii(pipe_name);
    child_t child;
    SECURITY_ATTRIBUTES pipe_security {};
    PSECURITY_DESCRIPTOR pipe_descriptor = nullptr;
    if (!make_pipe_security(pipe_security, pipe_descriptor)) {
      BOOST_LOG(error) << "Video worker: failed to construct a restricted named-pipe ACL; using in-process pipeline.";
      return direct_vgd;
    }
    auto free_pipe_descriptor = util::fail_guard([&] { ::LocalFree(pipe_descriptor); });
    child.pipe = ::CreateNamedPipeW(full_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 4 * 1024 * 1024, 64 * 1024, 0, &pipe_security);
    if (child.pipe == INVALID_HANDLE_VALUE || !launch(child, pipe_name)) {
      BOOST_LOG(error) << "Video worker: failed to establish isolated process; using in-process pipeline.";
      return direct_vgd;
    }
    OVERLAPPED connect {};
    connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
      if (connect.hEvent) ::CloseHandle(connect.hEvent);
      ::TerminateProcess(child.process, 0xE003);
      (void) ::WaitForSingleObject(child.process, 1000);
      BOOST_LOG(error) << "Video worker: child did not connect within 10 seconds.";
      return direct_vgd;
    }
    if (connect.hEvent) ::CloseHandle(connect.hEvent);
    ULONG client_pid = 0;
    if (!::GetNamedPipeClientProcessId(child.pipe, &client_pid) || client_pid != child.pid) {
      BOOST_LOG(error) << "Video worker: rejected unexpected pipe client pid=" << client_pid
                       << " expected=" << child.pid;
      ::TerminateProcess(child.process, 0xE004);
      return direct_vgd;
    }
    BOOST_LOG(info) << "Video worker: isolated capture/encode process started (pid=" << child.pid << ")";
    if (!send(child.pipe, message_e::start, &start, sizeof(start))) {
      BOOST_LOG(error) << "Video worker: failed to send startup state; using in-process pipeline.";
      ::TerminateProcess(child.process, 0xE005);
      (void) ::WaitForSingleObject(child.process, 1000);
      return false;
    }

    // Do not hand the session to a worker that has not accepted the parent's
    // validated encoder state and completed its capture-only bootstrap. This
    // deadline deliberately fits inside the client's initial-frame timeout.
    header_t ready_header {};
    std::vector<std::uint8_t> ready_payload;
    const bool encoder_ready = receive(child.pipe, ready_header, ready_payload, 1500) &&
                               ready_header.type == message_e::encoder_ready;
    const bool capture_ready = encoder_ready && receive(child.pipe, ready_header, ready_payload, 7000) &&
                               ready_header.type == message_e::capture_ready;
    if (!capture_ready) {
      BOOST_LOG(error) << "Video worker: capture bootstrap did not produce a first encoded packet within its deadline.";
      (void) ::CancelIoEx(child.pipe, nullptr);
      ::TerminateProcess(child.process, 0xE006);
      const bool worker_reaped = ::WaitForSingleObject(child.process, 3000) == WAIT_OBJECT_0;
      ::DisconnectNamedPipe(child.pipe);
      if (!worker_reaped) {
        BOOST_LOG(error) << "Video worker: process remains stuck after termination; refusing unsafe in-process GPU fallback.";
        return true;
      }
      if (!encoder_ready || ready_header.type == message_e::startup_error) {
        BOOST_LOG(warning) << "Video worker: failure occurred before GPU capture startup; using in-process pipeline.";
        return direct_vgd;
      }
      const auto health = platf::dxgi::D3D11ProbeDeviceHealth();
      if (FAILED(health)) {
        BOOST_LOG(error) << "Video worker: display health probe failed after worker termination (hresult=0x"
                         << std::hex << health << std::dec << "); refusing unsafe in-process GPU fallback.";
        return true;
      }
      BOOST_LOG(warning) << "Video worker: process exited and display health is good; using in-process pipeline.";
      return direct_vgd;
    }
    BOOST_LOG(info) << "Video worker: validated encoder snapshot accepted; capture pipeline ready.";

    auto shutdown = mail->event<bool>(mail::shutdown);
    std::atomic_bool reader_done {false};
    std::atomic<std::int64_t> last_frame_ms {0};
    std::atomic_bool first_frame_seen {false};
    const auto now_ms = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    last_frame_ms.store(now_ms(), std::memory_order_release);

    std::thread reader([&] {
      header_t header {};
      std::vector<std::uint8_t> body;
      auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
      while (receive(child.pipe, header, body)) {
        if (header.type == message_e::finished) break;
        if (header.type == message_e::packet && body.size() >= sizeof(packet_header_t)) {
          packet_header_t packet_header {};
          std::memcpy(&packet_header, body.data(), sizeof(packet_header));
          if (packet_header.data_size <= body.size() - sizeof(packet_header) &&
              body.size() - sizeof(packet_header) == packet_header.data_size) {
            std::vector<std::uint8_t> bytes(packet_header.data_size);
            std::memcpy(bytes.data(), body.data() + sizeof(packet_header), bytes.size());
            auto packet = std::make_unique<video::packet_raw_generic>(std::move(bytes), packet_header.frame_index, packet_header.idr != 0);
            packet->after_ref_frame_invalidation = packet_header.after_rfi != 0;
            packet->channel_data = channel_data;
            packets->raise(std::move(packet));
            first_frame_seen.store(true, std::memory_order_release);
            last_frame_ms.store(now_ms(), std::memory_order_release);
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
        } else if (header.type != message_e::heartbeat) {
          BOOST_LOG(error) << "Video worker: rejected an out-of-state child IPC message.";
          break;
        }
      }
      reader_done.store(true, std::memory_order_release);
    });

    auto idr = mail->event<bool>(mail::idr);
    auto invalidate = mail->event<std::pair<std::int64_t, std::int64_t>>(mail::invalidate_ref_frames);
    bool poisoned = false;
    while (!shutdown->peek() && !reader_done.load(std::memory_order_acquire)) {
      if (idr->pop(25ms)) (void) send(child.pipe, message_e::idr);
      while (auto range = invalidate->pop(0ms)) {
        const invalidate_t wire {range->first, range->second};
        (void) send(child.pipe, message_e::invalidate, &wire, sizeof(wire));
      }
      const auto silence_ms = now_ms() - last_frame_ms.load(std::memory_order_acquire);
      const auto deadline_ms = first_frame_seen.load(std::memory_order_acquire) ? 3000 : 7000;
      if (silence_ms > deadline_ms) {
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
    (void) ::CancelIoEx(child.pipe, nullptr);
    ::DisconnectNamedPipe(child.pipe);
    if (reader.joinable()) reader.join();
    BOOST_LOG(info) << "Video worker: isolated capture/encode process ended.";
    return true;
  }
}
