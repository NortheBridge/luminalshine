/**
 * @file src/session_monitor_proxy.h
 * @brief HTTP proxy from the main service's confighttp Web UI
 *        endpoints to the LuminalShineSessionMonitor service's
 *        localhost HTTP server.
 *
 * The Web UI sees `/api/sessions/*` on the main HTTPS port. The
 * monitor lives on a separate process listening on a randomised
 * localhost port. This module bridges them so the Web UI's auth /
 * CORS / TLS layer is reused without exposing the monitor directly.
 *
 * Port discovery: the monitor writes its bound port to
 * `%ProgramData%\LuminalShine\session_mon.port` on startup. We
 * cache the value and re-read on connect failure (handles the
 * monitor restarting and getting a new ephemeral port).
 */
#pragma once

#include <optional>
#include <string>

namespace session_mon::proxy {

  /**
   * @brief Forward a GET request to the monitor. Returns the
   *        monitor's response body + content-type + HTTP status,
   *        or std::nullopt if the monitor is unreachable.
   */
  struct Response {
    int         status       = 0;
    std::string content_type = "application/json";
    std::string body;
    std::string extra_headers;
  };

  std::optional<Response> get(const std::string &path);

  std::optional<Response> del(const std::string &path);

}  // namespace session_mon::proxy
