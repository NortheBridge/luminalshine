/**
 * @file tools/luminalshine_sessionmon.cpp
 * @brief LuminalShine Session Monitor Service.
 *
 * Independent Windows service (separate from LuminalShineService and
 * LuminalShineXboxBtHelper) that collects per-stream-session telemetry
 * for the Session Details panel in the Web UI.
 *
 * Why a separate service:
 *   - Resiliency: perf-counter sampling and ring-buffer maintenance
 *     should not run on the streaming hot path. A regression in metrics
 *     collection cannot stall encoding or stream delivery.
 *   - Crash isolation: if the main streaming host crashes mid-session,
 *     this service finalises the active session with `stream_ended_at`
 *     so the UI still has the history. If THIS service crashes, the
 *     streaming host logs a one-line warning and continues; the next
 *     ingest attempt re-establishes the pipe.
 *   - Independent lifecycle: SCM can restart this service without
 *     affecting an in-progress stream.
 *
 * IPC contract:
 *   Ingest pipe — `\\.\pipe\LuminalShineSessionMonIngest`. Length-
 *   prefixed JSON frames (one frame per metric sample / event /
 *   lifecycle marker). SYSTEM-only ACL: only processes running as
 *   LocalSystem can connect, which is everything LuminalShine ever
 *   spawns. Fire-and-forget from the producer side; the service never
 *   writes back on this pipe.
 *
 *   Query HTTP — localhost-only HTTP on a random port written to
 *   `%ProgramData%\LuminalShine\session_mon.port` with a SYSTEM-only
 *   ACL on the file. Main service reads the port once at startup and
 *   proxies `/api/sessions/*` requests to it. Bind address is
 *   strictly 127.0.0.1 so the server is unreachable from the
 *   network even if a misconfigured firewall rule exposed the port.
 *
 * Storage:
 *   - In-memory ring buffer keyed by session-uuid, holding 1-second
 *     buckets for up to 24h per session. New samples for an existing
 *     bucket overwrite (latest-wins) rather than aggregate; the
 *     producer is responsible for picking the right summary statistic
 *     (mean for utilisations, sum for events, latest for bitrate).
 *   - On-disk persistence: each session serialises to
 *     `%ProgramData%\LuminalShine\sessions\<uuid>.json` on
 *     `session_ended` and every 5 minutes during the session as a
 *     resilience checkpoint. Retention is indefinite — explicit
 *     deletion via the Web UI removes the file.
 */

#define WIN32_LEAN_AND_MEAN
// clang-format off
// winsock2.h must precede Windows.h so the WSADATA/SOCKET/sockaddr_in
// surface is defined before any later transitive include of windows.h
// drags in the legacy winsock.h declarations.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
// clang-format on
#include <ShlObj.h>
#include <KnownFolders.h>
#include <AclAPI.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

  // -------------------------------------------------------------- constants

  constexpr char kServiceName[]     = "LuminalShineSessionMonitor";
  constexpr wchar_t kPipeName[]     = L"\\\\.\\pipe\\LuminalShineSessionMonIngest";
  constexpr wchar_t kLogFileName[]  = L"luminalshine_sessionmon.log";
  constexpr wchar_t kPortFileName[] = L"session_mon.port";

  // Ring-buffer retention: 24 hours at one bucket per second.
  constexpr std::size_t kRingCapacity = 24 * 60 * 60;

  // Maximum number of ENDED sessions retained in the dashboard.
  // Live sessions are never counted toward this cap so a long-
  // running stream cannot push itself out, and a producer that
  // floods stream_started/stream_ended cycles cannot evict a
  // currently-active session out from under the user. Hard cap;
  // the UI's Session History list is already paginated and 25
  // entries fits one viewport without scrolling on the smallest
  // supported window. Change deliberately, not as a tuning knob.
  constexpr std::size_t kMaxEndedSessions = 25;

  // Persistence checkpoint cadence. The session also flushes
  // immediately on session_ended.
  constexpr std::chrono::seconds kPersistInterval {5 * 60};

  // SCM status state machine helpers — Windows SERVICE_*_PENDING /
  // SERVICE_RUNNING constants are scattered through winsvc.h.

  SERVICE_STATUS_HANDLE g_status_handle = nullptr;
  SERVICE_STATUS        g_status        = {};
  HANDLE                g_stop_event    = nullptr;

  std::FILE *g_log_file = nullptr;
  std::mutex g_log_mtx;

  // ------------------------------------------------------------------ paths

  std::filesystem::path program_data_path() {
    PWSTR pd = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &pd)) || !pd) {
      return {};
    }
    std::filesystem::path p {pd};
    CoTaskMemFree(pd);
    return p;
  }

  std::filesystem::path sessions_dir() {
    auto root = program_data_path();
    if (root.empty()) {
      return {};
    }
    auto p = root / L"LuminalShine" / L"sessions";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
  }

  std::filesystem::path logs_dir() {
    auto root = program_data_path();
    if (root.empty()) {
      return {};
    }
    auto p = root / L"LuminalShine" / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
  }

  std::filesystem::path port_file_path() {
    auto root = program_data_path();
    if (root.empty()) {
      return {};
    }
    return root / L"LuminalShine" / kPortFileName;
  }

  // ----------------------------------------------------------------- logging

  void log_line(const char *level, const std::string &msg) {
    std::lock_guard<std::mutex> guard(g_log_mtx);
    if (!g_log_file) {
      return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(
      g_log_file,
      "%04u-%02u-%02uT%02u:%02u:%02u.%03u %s %s\n",
      st.wYear, st.wMonth, st.wDay,
      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      level, msg.c_str()
    );
    std::fflush(g_log_file);
  }

  void log_info(const std::string &m) { log_line("INFO ", m); }
  void log_warn(const std::string &m) { log_line("WARN ", m); }
  void log_err(const std::string &m)  { log_line("ERROR", m); }

  void open_log() {
    auto path = logs_dir() / kLogFileName;
    if (path.empty()) {
      return;
    }
    std::lock_guard<std::mutex> guard(g_log_mtx);
    _wfopen_s(&g_log_file, path.c_str(), L"a");
  }

  // ------------------------------------------------------------- data model

  /// One time-series point. Stored as (epoch-seconds, value) tuples;
  /// JSON serialisation flattens to `[ts, v]` for compactness — same
  /// shape uPlot consumes on the Web UI side.
  struct SeriesPoint {
    std::int64_t ts;
    double       value;
  };

  /// Bounded ring buffer specialised for SeriesPoint. Latest-wins
  /// semantics: appending a point whose `ts` matches the most-recent
  /// entry's `ts` replaces the existing value rather than adding a
  /// duplicate bucket. Cheaper than a map for the steady-state
  /// 1Hz-per-series ingest pattern.
  struct Series {
    std::deque<SeriesPoint> points;

    void push(std::int64_t ts, double v) {
      if (!points.empty() && points.back().ts == ts) {
        points.back().value = v;
        return;
      }
      points.push_back({ts, v});
      while (points.size() > kRingCapacity) {
        points.pop_front();
      }
    }

    nlohmann::json to_json() const {
      nlohmann::json out = nlohmann::json::array();
      for (const auto &p : points) {
        out.push_back({p.ts, p.value});
      }
      return out;
    }
  };

  /// A streaming session. `metadata` is the JSON object as produced by
  /// the streaming host at session-started time — passed through
  /// untouched on persistence and query so the producer can add new
  /// fields without changing the monitor.
  struct Session {
    std::string                       id;
    std::int64_t                      started_at_epoch  = 0;
    std::int64_t                      ended_at_epoch    = 0;  // 0 = active
    nlohmann::json                    metadata          = nlohmann::json::object();
    bool                              dirty_since_flush = false;
    std::chrono::steady_clock::time_point last_flush  = std::chrono::steady_clock::now();
    std::map<std::string, Series>     series;

    nlohmann::json to_json() const {
      nlohmann::json out;
      out["id"] = id;
      out["started_at"] = started_at_epoch;
      if (ended_at_epoch != 0) {
        out["stream_ended_at"] = ended_at_epoch;
      } else {
        out["stream_ended_at"] = nullptr;
      }
      out["metadata"] = metadata;
      nlohmann::json s = nlohmann::json::object();
      for (const auto &[name, series] : series) {
        s[name] = series.to_json();
      }
      out["series"] = std::move(s);
      return out;
    }
  };

  /// Sessions store + the lock that guards every mutation and query.
  /// We keep the lock coarse-grained because mutations are 1Hz at peak
  /// (one stream session writing 7 series at 1Hz = 7 mutations/s) and
  /// queries are sub-Hz (user clicks panel + occasional UI refresh).
  /// A finer-grained per-session lock would not change the throughput
  /// envelope and would complicate the persistence / housekeeping
  /// background thread.
  struct Store {
    std::mutex                                       mtx;
    std::unordered_map<std::string, Session>         sessions;
  };

  Store g_store;

  std::int64_t epoch_now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  }

  // --------------------------------------------------------------- ingest

  /// Parse one ingest frame and apply it to the store. Frame format:
  ///
  ///   { "type": "session_started",
  ///     "id": "<uuid>",
  ///     "started_at": <unix-seconds>,
  ///     "metadata": { ... arbitrary producer payload ... } }
  ///
  ///   { "type": "sample",
  ///     "id": "<uuid>",
  ///     "ts": <unix-seconds>,
  ///     "series": { "encode_latency_ms": 4.3,
  ///                 "actual_fps": 118.0,
  ///                 ... } }
  ///
  ///   { "type": "metadata_update",
  ///     "id": "<uuid>",
  ///     "metadata": { ... patch — merged shallow over existing ... } }
  ///
  ///   { "type": "session_ended",
  ///     "id": "<uuid>",
  ///     "ended_at": <unix-seconds> }
  ///
  /// Drop the oldest ended sessions until at most kMaxEndedSessions
  /// remain. Live sessions (ended_at_epoch == 0) are exempt. The
  /// matching on-disk JSON files are unlinked under the lock; the
  /// caller MUST already hold g_store.mtx. Returns true when at
  /// least one session was pruned, so the caller can log it.
  bool prune_excess_ended_sessions_locked() {
    std::vector<std::pair<std::int64_t, std::string>> ended;
    ended.reserve(g_store.sessions.size());
    for (const auto &[id, s] : g_store.sessions) {
      if (s.ended_at_epoch != 0) {
        ended.emplace_back(s.ended_at_epoch, id);
      }
    }
    if (ended.size() <= kMaxEndedSessions) {
      return false;
    }
    // Oldest first by ended_at_epoch; ties broken by id which is
    // deterministic enough for a "least-recently-ended" cull.
    std::sort(ended.begin(), ended.end());
    const std::size_t drop = ended.size() - kMaxEndedSessions;
    for (std::size_t i = 0; i < drop; ++i) {
      const auto &id = ended[i].second;
      g_store.sessions.erase(id);
      std::error_code ec;
      std::filesystem::remove(sessions_dir() / (id + ".json"), ec);
      // Best-effort: a removal failure (file already gone, ACL
      // denial, etc.) is logged but does not abort the prune — the
      // in-memory map has already been corrected and the next
      // startup will eventually rediscover the stale file as a
      // legacy load. We deliberately do NOT touch sessions whose
      // ended_at_epoch == 0 here even on race; the partition above
      // never includes them.
      if (ec) {
        log_warn("prune: could not remove sessions/" + id + ".json: " + ec.message());
      }
    }
    return true;
  }

  /// Unknown frame types are logged and discarded — forward-compatible
  /// with future producer versions.
  void apply_frame(const nlohmann::json &frame) {
    const auto type = frame.value("type", std::string {});
    const auto id   = frame.value("id", std::string {});
    if (type.empty() || id.empty()) {
      log_warn("dropping ingest frame missing type or id");
      return;
    }

    std::lock_guard<std::mutex> guard(g_store.mtx);
    auto &s = g_store.sessions[id];
    if (s.id.empty()) {
      s.id = id;
    }
    s.dirty_since_flush = true;

    if (type == "session_started") {
      s.started_at_epoch = frame.value("started_at", epoch_now_seconds());
      s.ended_at_epoch   = 0;
      if (frame.contains("metadata") && frame["metadata"].is_object()) {
        s.metadata = frame["metadata"];
      }
      return;
    }
    if (type == "sample") {
      const auto ts = frame.value("ts", epoch_now_seconds());
      if (frame.contains("series") && frame["series"].is_object()) {
        for (const auto &[name, val] : frame["series"].items()) {
          if (val.is_number()) {
            s.series[name].push(ts, val.get<double>());
          }
        }
      }
      return;
    }
    if (type == "metadata_update") {
      if (frame.contains("metadata") && frame["metadata"].is_object()) {
        for (const auto &[k, v] : frame["metadata"].items()) {
          s.metadata[k] = v;
        }
      }
      return;
    }
    if (type == "session_ended") {
      s.ended_at_epoch = frame.value("ended_at", epoch_now_seconds());
      // session_ended is the right place to prune: it's the only
      // event that grows the "ended sessions" partition by one.
      // Pruning here also keeps the cap honoured at runtime, not
      // just across restarts. The lock is already held above so the
      // helper sees a consistent view of the store.
      (void) prune_excess_ended_sessions_locked();
      return;
    }
    log_warn("dropping ingest frame with unknown type: " + type);
  }

  /// Read length-prefixed frames from one pipe instance until the
  /// client disconnects. Frames are encoded as:
  ///   [4 bytes little-endian uint32: payload length]
  ///   [payload-length bytes of UTF-8 JSON]
  void handle_pipe_client(HANDLE pipe) {
    for (;;) {
      std::uint32_t len = 0;
      DWORD got = 0;
      if (!ReadFile(pipe, &len, sizeof(len), &got, nullptr) || got != sizeof(len)) {
        break;
      }
      if (len == 0 || len > 1024 * 1024) {
        // Drop malformed / oversized frames defensively. A 1 MiB cap
        // is well above the largest legitimate frame (a full
        // session_started with metadata is < 4 KiB) and protects the
        // service from a misbehaving producer ballooning memory.
        log_warn("dropping ingest frame with unreasonable length");
        break;
      }
      std::vector<char> buf(len);
      DWORD read_total = 0;
      while (read_total < len) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, buf.data() + read_total, len - read_total, &chunk, nullptr) || chunk == 0) {
          read_total = 0;
          break;
        }
        read_total += chunk;
      }
      if (read_total != len) {
        break;
      }
      try {
        auto j = nlohmann::json::parse(std::string_view {buf.data(), buf.size()});
        apply_frame(j);
      } catch (const std::exception &e) {
        log_warn(std::string {"ingest parse failure: "} + e.what());
      }
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }

  /// Build a SECURITY_ATTRIBUTES that allows ONLY the LocalSystem
  /// SID to read / write the pipe. Anything not running as SYSTEM
  /// (which is the entire user session) cannot open the pipe — so
  /// even a compromised user process can't push fake session
  /// telemetry to the dashboard.
  bool build_system_only_sa(SECURITY_ATTRIBUTES &sa, PSECURITY_DESCRIPTOR &sd_out) {
    // SDDL: Owner=LocalSystem, Group=LocalSystem, DACL = Allow
    // (GenericAll) to LocalSystem only.
    constexpr wchar_t kSddl[] = L"O:SYG:SYD:(A;;GA;;;SY)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) {
      return false;
    }
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;
    sd_out = sd;
    return true;
  }

  void pipe_listener_thread() {
    for (;;) {
      SECURITY_ATTRIBUTES  sa  {};
      PSECURITY_DESCRIPTOR sd  = nullptr;
      LPSECURITY_ATTRIBUTES sa_ptr = nullptr;
      if (build_system_only_sa(sa, sd)) {
        sa_ptr = &sa;
      }

      HANDLE pipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        0,            // outbound buffer (we never write)
        64 * 1024,    // inbound buffer
        0,            // default timeout
        sa_ptr
      );
      if (sd) {
        LocalFree(sd);
      }
      if (pipe == INVALID_HANDLE_VALUE) {
        log_err("CreateNamedPipeW failed; ingest disabled for 5s");
        if (WaitForSingleObject(g_stop_event, 5000) == WAIT_OBJECT_0) {
          return;
        }
        continue;
      }

      const BOOL connected =
        ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
      if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
        CloseHandle(pipe);
        return;
      }
      if (!connected) {
        CloseHandle(pipe);
        continue;
      }
      // Detach per-client thread so the listener immediately races
      // back to accept the next connection. Each client conventionally
      // owns one streaming session, so concurrency is low single
      // digits in practice.
      std::thread(handle_pipe_client, pipe).detach();
    }
  }

  // --------------------------------------------------------- HTTP query

  /// Tiny WinSock-based HTTP/1.1 server. We deliberately do NOT pull
  /// in Simple-Web-Server (which the main service uses) — keeping the
  /// monitor's dependency closure to nlohmann::json + the C++
  /// standard library means a Simple-Web-Server / Boost.Asio
  /// regression cannot crash this service. The endpoints are simple
  /// enough that hand-rolling beats dragging in 8 MB of headers.
  ///
  /// Supported endpoints (all GET unless noted):
  ///   GET /api/sessions                     — list of all session IDs + summary
  ///   GET /api/sessions/<id>                — full session JSON
  ///   GET /api/sessions/<id>/export.json    — same content, marked as
  ///                                            an attachment for the
  ///                                            Web UI's download link
  ///   DELETE /api/sessions/<id>             — remove session in-memory + on disk
  std::string http_response(int status, std::string_view content_type, std::string body, std::string extra_headers = {}) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " "
        << (status == 200 ? "OK" : status == 404 ? "Not Found" : status == 400 ? "Bad Request" : "Error")
        << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << extra_headers
        << "\r\n"
        << body;
    return oss.str();
  }

  std::string handle_list_sessions() {
    nlohmann::json arr = nlohmann::json::array();
    std::lock_guard<std::mutex> guard(g_store.mtx);
    for (const auto &[id, s] : g_store.sessions) {
      nlohmann::json e;
      e["id"] = s.id;
      e["started_at"]      = s.started_at_epoch;
      e["stream_ended_at"] = s.ended_at_epoch == 0 ? nlohmann::json {nullptr} : nlohmann::json {s.ended_at_epoch};
      e["metadata"]        = s.metadata;
      arr.push_back(std::move(e));
    }
    return http_response(200, "application/json", arr.dump());
  }

  std::string handle_get_session(const std::string &id, bool as_attachment) {
    std::lock_guard<std::mutex> guard(g_store.mtx);
    auto it = g_store.sessions.find(id);
    if (it == g_store.sessions.end()) {
      return http_response(404, "application/json", "{\"error\":\"not found\"}");
    }
    auto body = it->second.to_json().dump(2);
    std::string extra;
    if (as_attachment) {
      extra = "Content-Disposition: attachment; filename=\"luminalshine-session-" + id + ".json\"\r\n";
    }
    return http_response(200, "application/json", std::move(body), extra);
  }

  std::string handle_delete_session(const std::string &id) {
    {
      std::lock_guard<std::mutex> guard(g_store.mtx);
      auto it = g_store.sessions.find(id);
      if (it == g_store.sessions.end()) {
        return http_response(404, "application/json", "{\"error\":\"not found\"}");
      }
      if (it->second.ended_at_epoch == 0) {
        // Refuse to delete an active session — the producer is still
        // pushing samples to it and the UI's "Disconnect Session"
        // gesture is the correct way to terminate first.
        return http_response(400, "application/json",
          "{\"error\":\"session is active; disconnect it first\"}");
      }
      g_store.sessions.erase(it);
    }
    std::error_code ec;
    std::filesystem::remove(sessions_dir() / (id + ".json"), ec);
    return http_response(200, "application/json", "{\"status\":\"ok\"}");
  }

  std::string route_request(std::string_view method, std::string_view path) {
    // Hand-rolled router — only a handful of routes so the
    // alternatives (regex / a routing library) would be overkill.
    if (method == "GET" && path == "/api/sessions") {
      return handle_list_sessions();
    }
    constexpr std::string_view prefix = "/api/sessions/";
    if (path.starts_with(prefix)) {
      auto rest = std::string {path.substr(prefix.size())};
      if (method == "DELETE") {
        return handle_delete_session(rest);
      }
      if (method == "GET") {
        constexpr std::string_view export_suffix = "/export.json";
        if (rest.size() > export_suffix.size() && rest.ends_with(export_suffix)) {
          rest.resize(rest.size() - export_suffix.size());
          return handle_get_session(rest, /*as_attachment=*/true);
        }
        return handle_get_session(rest, /*as_attachment=*/false);
      }
    }
    return http_response(404, "application/json", "{\"error\":\"no route\"}");
  }

  void write_port_file(std::uint16_t port) {
    // The main service reads this file once at startup to find the
    // monitor's HTTP port. SYSTEM-only ACL via the inherited DACL on
    // %ProgramData%\LuminalShine\ (which the installer sets up to
    // restrict to SYSTEM + Administrators). The file content is just
    // the port number as ASCII.
    auto path = port_file_path();
    if (path.empty()) {
      return;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      log_err("could not write port file at " + path.string());
      return;
    }
    out << port;
  }

  void http_server_thread() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      log_err("WSAStartup failed; HTTP server disabled");
      return;
    }
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
      log_err("socket() failed; HTTP server disabled");
      WSACleanup();
      return;
    }
    // Localhost-only bind. The kernel picks an ephemeral port (we
    // pass port 0); we then read it back via getsockname and write it
    // to the port file for the main service to discover.
    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (bind(listen_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
      log_err("bind() failed; HTTP server disabled");
      closesocket(listen_sock);
      WSACleanup();
      return;
    }
    sockaddr_in bound {};
    int bound_len = sizeof(bound);
    if (getsockname(listen_sock, reinterpret_cast<sockaddr *>(&bound), &bound_len) == SOCKET_ERROR) {
      log_err("getsockname() failed; HTTP server disabled");
      closesocket(listen_sock);
      WSACleanup();
      return;
    }
    const auto port = ntohs(bound.sin_port);
    if (listen(listen_sock, 16) == SOCKET_ERROR) {
      log_err("listen() failed; HTTP server disabled");
      closesocket(listen_sock);
      WSACleanup();
      return;
    }
    write_port_file(port);
    log_info("HTTP query server listening on 127.0.0.1:" + std::to_string(port));

    for (;;) {
      if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
        break;
      }
      // Block in accept; the stop path forcibly closes listen_sock
      // from the service shutdown handler which unblocks us with
      // WSAEINTR / WSAENOTSOCK.
      SOCKET client = accept(listen_sock, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
      // Per-client thread. HTTP requests are tiny and synchronous so
      // a thread-per-request without pooling is plenty for the
      // localhost-only single-consumer (main service) traffic this
      // endpoint sees.
      std::thread([client]() {
        std::string buf;
        buf.reserve(2048);
        char chunk[2048];
        for (;;) {
          int n = recv(client, chunk, sizeof(chunk), 0);
          if (n <= 0) {
            break;
          }
          buf.append(chunk, n);
          if (buf.find("\r\n\r\n") != std::string::npos) {
            break;
          }
          if (buf.size() > 16 * 1024) {
            break;  // header bomb cap
          }
        }
        std::string method, path;
        auto sp1 = buf.find(' ');
        auto sp2 = buf.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
          method = buf.substr(0, sp1);
          path   = buf.substr(sp1 + 1, sp2 - sp1 - 1);
        }
        auto response = route_request(method, path);
        send(client, response.data(), static_cast<int>(response.size()), 0);
        shutdown(client, SD_BOTH);
        closesocket(client);
      }).detach();
    }
    closesocket(listen_sock);
    WSACleanup();
  }

  // ------------------------------------------------------------ persistence

  /// Write one session to disk atomically: temp file in the same
  /// directory + rename. Same crash-safety pattern the main service
  /// already uses for sunshine_state.json.
  bool persist_session(const Session &s) {
    auto dir = sessions_dir();
    if (dir.empty()) {
      return false;
    }
    auto final_path = dir / (s.id + ".json");
    auto tmp_path   = dir / (s.id + ".json.tmp");
    {
      std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      out << s.to_json().dump(2);
      if (!out) {
        return false;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
      std::filesystem::remove(tmp_path, ec);
      return false;
    }
    return true;
  }

  /// On startup, reload every persisted session from disk. Already-
  /// ended sessions come back read-only; if any session has
  /// `ended_at == null` (the main service crashed mid-stream) we
  /// finalise it with the current time as the ended_at marker so the
  /// UI doesn't render it as "active" forever.
  void load_persisted_sessions() {
    auto dir = sessions_dir();
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file() || entry.path().extension() != ".json") {
        continue;
      }
      std::ifstream in(entry.path(), std::ios::binary);
      if (!in) {
        continue;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      try {
        auto j = nlohmann::json::parse(ss.str());
        Session s;
        s.id = j.value("id", entry.path().stem().string());
        s.started_at_epoch = j.value("started_at", std::int64_t {0});
        if (j.contains("stream_ended_at") && j["stream_ended_at"].is_number_integer()) {
          s.ended_at_epoch = j["stream_ended_at"].get<std::int64_t>();
        } else {
          // Crash-recovered session: finalise immediately so the UI
          // doesn't show a stale "active" stream after a restart.
          s.ended_at_epoch = epoch_now_seconds();
        }
        s.metadata = j.value("metadata", nlohmann::json::object());
        if (j.contains("series") && j["series"].is_object()) {
          for (const auto &[name, arr] : j["series"].items()) {
            if (!arr.is_array()) {
              continue;
            }
            Series series;
            for (const auto &p : arr) {
              if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) {
                series.points.push_back({p[0].get<std::int64_t>(), p[1].get<double>()});
              }
            }
            s.series.emplace(name, std::move(series));
          }
        }
        std::lock_guard<std::mutex> guard(g_store.mtx);
        g_store.sessions.emplace(s.id, std::move(s));
      } catch (const std::exception &e) {
        log_warn("could not load session from " + entry.path().string() + ": " + e.what());
      }
    }
    // After rehydrating everything from disk, enforce the ended-
    // sessions cap. Pre-cap installs may have left an unbounded
    // backlog of *.json files in %ProgramData%/LuminalShine/sessions/
    // — this is where they get culled, with the matching map entries
    // dropped in the same atomic step as the on-disk unlink.
    std::lock_guard<std::mutex> guard(g_store.mtx);
    if (prune_excess_ended_sessions_locked()) {
      log_info("startup prune: trimmed session history to "
               + std::to_string(kMaxEndedSessions) + " ended sessions");
    }
  }

  void housekeeping_thread() {
    using namespace std::chrono;
    for (;;) {
      if (WaitForSingleObject(g_stop_event, 30 * 1000) == WAIT_OBJECT_0) {
        return;
      }
      std::vector<Session> snapshots;
      {
        std::lock_guard<std::mutex> guard(g_store.mtx);
        for (auto &[id, s] : g_store.sessions) {
          if (!s.dirty_since_flush) {
            continue;
          }
          const auto now = steady_clock::now();
          // Flush immediately on session_ended (ended_at != 0); flush
          // periodic checkpoints every kPersistInterval otherwise.
          if (s.ended_at_epoch != 0 || (now - s.last_flush) >= kPersistInterval) {
            snapshots.push_back(s);  // copy for write-without-lock
            s.dirty_since_flush = false;
            s.last_flush = now;
          }
        }
      }
      for (const auto &snap : snapshots) {
        if (!persist_session(snap)) {
          log_warn("persist_session failed for " + snap.id);
        }
      }
    }
  }

  // ----------------------------------------------------------- service ctl

  void set_service_status(DWORD state) {
    g_status.dwServiceType        = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState       = state;
    g_status.dwControlsAccepted   = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWin32ExitCode      = 0;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint         = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : 1;
    g_status.dwWaitHint           = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : 30000;
    if (g_status_handle) {
      SetServiceStatus(g_status_handle, &g_status);
    }
  }

  DWORD WINAPI HandlerEx(DWORD control, DWORD /*event*/, LPVOID /*event_data*/, LPVOID /*ctx*/) {
    switch (control) {
      case SERVICE_CONTROL_STOP:
      case SERVICE_CONTROL_SHUTDOWN:
        set_service_status(SERVICE_STOP_PENDING);
        SetEvent(g_stop_event);
        return NO_ERROR;
      case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
      default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
  }

  VOID WINAPI ServiceMain(DWORD /*argc*/, LPSTR * /*argv*/) {
    g_status_handle = RegisterServiceCtrlHandlerExA(kServiceName, HandlerEx, nullptr);
    if (!g_status_handle) {
      return;
    }
    set_service_status(SERVICE_START_PENDING);
    open_log();
    log_info("LuminalShineSessionMonitor starting");

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
      log_err("CreateEvent failed; aborting startup");
      set_service_status(SERVICE_STOPPED);
      return;
    }

    load_persisted_sessions();

    std::thread pipe_th(pipe_listener_thread);
    std::thread http_th(http_server_thread);
    std::thread house_th(housekeeping_thread);

    set_service_status(SERVICE_RUNNING);
    WaitForSingleObject(g_stop_event, INFINITE);
    log_info("LuminalShineSessionMonitor stopping");

    // Final flush — write every dirty session to disk before exit so
    // the UI's "stream ended at" marker is durable across a service
    // restart.
    {
      std::vector<Session> final_snapshots;
      {
        std::lock_guard<std::mutex> guard(g_store.mtx);
        for (auto &[id, s] : g_store.sessions) {
          if (s.ended_at_epoch == 0) {
            s.ended_at_epoch = epoch_now_seconds();
          }
          final_snapshots.push_back(s);
        }
      }
      for (const auto &snap : final_snapshots) {
        persist_session(snap);
      }
    }

    if (pipe_th.joinable()) {
      pipe_th.detach();  // pipe accept may block; OS reaps on process exit
    }
    if (http_th.joinable()) {
      http_th.detach();
    }
    if (house_th.joinable()) {
      house_th.join();
    }

    set_service_status(SERVICE_STOPPED);
  }

}  // namespace

int main(int argc, char * /*argv*/[]) {
  if (argc > 1) {
    // Manual diagnostic mode — useful when iterating the service
    // locally (run the binary directly from a SYSTEM-elevated
    // PowerShell instead of going through SCM). Treats the rest of
    // the process lifetime as the service body; press Ctrl+C to stop.
    open_log();
    log_info("starting in console mode");
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(
      [](DWORD) -> BOOL {
        SetEvent(g_stop_event);
        return TRUE;
      },
      TRUE);
    load_persisted_sessions();
    std::thread pipe_th(pipe_listener_thread);
    std::thread http_th(http_server_thread);
    std::thread house_th(housekeeping_thread);
    WaitForSingleObject(g_stop_event, INFINITE);
    pipe_th.detach();
    http_th.detach();
    house_th.join();
    return 0;
  }

  SERVICE_TABLE_ENTRYA table[] = {
    {const_cast<LPSTR>(kServiceName), ServiceMain},
    {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherA(table)) {
    return 1;
  }
  return 0;
}
