/**
 * @file src/session_monitor_proxy.cpp
 * @brief Implementation of the HTTP proxy to the session monitor
 *        service.
 *
 * Hand-rolled WinSock HTTP/1.1 client. We deliberately don't pull
 * in Simple-Web-Server's client side here for the same reason the
 * monitor doesn't use it on the server side: keep the failure
 * surface narrow. The single GET/DELETE we send is short, the
 * response is bounded by the monitor's payload caps, and the
 * connection is always to 127.0.0.1 so we don't need TLS / proxy /
 * DNS code paths.
 */
#include "session_monitor_proxy.h"

#include "logging.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <ShlObj.h>
  #include <KnownFolders.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

namespace session_mon::proxy {

  namespace {

    std::mutex      g_port_mtx;
    std::uint16_t   g_port_cached     = 0;
    std::chrono::steady_clock::time_point g_port_cache_expiry {};

    std::filesystem::path port_file_path() {
#ifdef _WIN32
      PWSTR pd = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd)) || !pd) {
        return {};
      }
      std::filesystem::path p {pd};
      CoTaskMemFree(pd);
      return p / L"LuminalShine" / L"session_mon.port";
#else
      return {};
#endif
    }

    /// Re-read the port file. The monitor writes the file on every
    /// service start, so after a restart we may have a stale cached
    /// value — refresh on demand. The 30s TTL avoids hammering the
    /// filesystem when the monitor is healthy.
    std::uint16_t read_port_locked() {
      auto path = port_file_path();
      if (path.empty()) {
        return 0;
      }
      std::ifstream in(path);
      if (!in) {
        return 0;
      }
      std::uint16_t p = 0;
      in >> p;
      return p;
    }

    std::uint16_t current_port(bool force_refresh) {
      std::lock_guard<std::mutex> lk(g_port_mtx);
      const auto now = std::chrono::steady_clock::now();
      if (!force_refresh && g_port_cached != 0 && now < g_port_cache_expiry) {
        return g_port_cached;
      }
      g_port_cached = read_port_locked();
      g_port_cache_expiry = now + std::chrono::seconds {30};
      return g_port_cached;
    }

#ifdef _WIN32

    bool ensure_wsa_started() {
      static std::once_flag flag;
      static bool ok = false;
      std::call_once(flag, [] {
        WSADATA wsa;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
      });
      return ok;
    }

    /// One-shot HTTP request to 127.0.0.1:<port>. Returns the parsed
    /// response or nullopt on connection / I/O failure.
    std::optional<Response> issue_request(const std::string &method, const std::string &path) {
      if (!ensure_wsa_started()) {
        return std::nullopt;
      }
      auto try_with_port = [&](std::uint16_t port) -> std::optional<Response> {
        if (port == 0) {
          return std::nullopt;
        }
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
          return std::nullopt;
        }
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // Short connect timeout — the monitor is on localhost, so
        // legitimate connect should complete in microseconds. A
        // multi-second hang means the service is down.
        DWORD timeout_ms = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

        if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
          closesocket(sock);
          return std::nullopt;
        }
        std::ostringstream req;
        req << method << ' ' << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Connection: close\r\n"
            << "User-Agent: LuminalShine-session-mon-proxy\r\n"
            << "\r\n";
        const auto req_str = req.str();
        if (send(sock, req_str.data(), static_cast<int>(req_str.size()), 0) == SOCKET_ERROR) {
          closesocket(sock);
          return std::nullopt;
        }
        std::string buf;
        buf.reserve(4096);
        char chunk[4096];
        for (;;) {
          int n = recv(sock, chunk, sizeof(chunk), 0);
          if (n <= 0) {
            break;
          }
          buf.append(chunk, n);
          if (buf.size() > 16 * 1024 * 1024) {
            // 16 MiB cap; the monitor never emits responses near
            // this size but the cap protects the main service from
            // a runaway response.
            break;
          }
        }
        closesocket(sock);

        // Parse HTTP/1.1 response. Simple split: status line, headers, body.
        const auto header_end = buf.find("\r\n\r\n");
        if (header_end == std::string::npos) {
          return std::nullopt;
        }
        const auto status_line_end = buf.find("\r\n");
        if (status_line_end == std::string::npos) {
          return std::nullopt;
        }
        // Status line is "HTTP/1.1 <code> <reason>".
        const auto sp1 = buf.find(' ');
        if (sp1 == std::string::npos) {
          return std::nullopt;
        }
        const auto sp2 = buf.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) {
          return std::nullopt;
        }
        Response resp;
        try {
          resp.status = std::stoi(buf.substr(sp1 + 1, sp2 - sp1 - 1));
        } catch (...) {
          return std::nullopt;
        }
        resp.body = buf.substr(header_end + 4);

        // Pull Content-Type and Content-Disposition through. Other
        // headers are dropped — the main service's outer response
        // layer adds its own.
        auto find_header = [&](std::string_view name) -> std::string {
          const std::string needle = "\r\n" + std::string {name} + ":";
          auto pos = buf.find(needle, status_line_end);
          if (pos == std::string::npos || pos >= header_end) {
            return {};
          }
          pos += needle.size();
          const auto eol = buf.find("\r\n", pos);
          if (eol == std::string::npos) {
            return {};
          }
          auto v = buf.substr(pos, eol - pos);
          while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
            v.erase(v.begin());
          }
          return v;
        };
        auto ct = find_header("Content-Type");
        if (!ct.empty()) {
          resp.content_type = std::move(ct);
        }
        auto cd = find_header("Content-Disposition");
        if (!cd.empty()) {
          resp.extra_headers = "Content-Disposition: " + cd + "\r\n";
        }
        return resp;
      };

      // First attempt with the cached port; on failure refresh and
      // retry once. Covers the "monitor restarted with a new
      // ephemeral port" case without making every request pay the
      // file-read cost.
      auto r = try_with_port(current_port(/*force_refresh=*/false));
      if (!r.has_value()) {
        r = try_with_port(current_port(/*force_refresh=*/true));
      }
      return r;
    }

#else
    std::optional<Response> issue_request(const std::string &, const std::string &) {
      return std::nullopt;
    }
#endif

  }  // namespace

  std::optional<Response> get(const std::string &path) {
    return issue_request("GET", path);
  }

  std::optional<Response> del(const std::string &path) {
    return issue_request("DELETE", path);
  }

}  // namespace session_mon::proxy
