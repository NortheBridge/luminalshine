/**
 * @file tests/unit/test_simpleweb_lifetime.cpp
 * @brief Stress test for Simple-Web-Server connection/response lifetime.
 *
 * Regression coverage for the 2026-07-27 crash class (POSTMORTEM-2026-07-27.md,
 * dump luminalshine.exe.52192.dmp): responses held, written, and dropped on
 * non-server threads (the SSE ICE-stream pattern) while clients run keep-alive
 * request loops and disconnect early. Before the NortheBridge fork patch,
 * Response::send() and Response::send_on_delete() could start two concurrent
 * async_write chains on one socket and the keep-alive streambuf hand-off was
 * not serialized with in-flight operations — corrupting the heap or scanning a
 * dangling request streambuf. The patched fork serializes every per-connection
 * operation on the connection strand and routes all writes through one queue.
 *
 * The test uses the plain-HTTP server instantiation: the raced code all lives
 * in ServerBase, shared with the HTTPS instantiation used in production, and
 * plain HTTP avoids certificate fixtures.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <Simple-Web-Server/client_http.hpp>
#include <Simple-Web-Server/server_http.hpp>

#include <gtest/gtest.h>

#include "../tests_common.h"

namespace {
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
  using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;

  class SimpleWebLifetime: public ::testing::Test {
  protected:
    void SetUp() override {
      server = std::make_unique<HttpServer>();
      server->config.port = 0;  // OS-assigned
      server->config.thread_pool_size = 2;  // exercise the strands, not just one runner
      server->config.timeout_request = 1;  // aggressive timeouts: exercise timer vs read races
      server->config.timeout_content = 2;

      server->resource["^/ping$"]["GET"] = [](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request>) {
        response->write("pong");
      };

      // The SSE-shaped hazard: hold the response on a detached thread, stream
      // a few events with explicit send(), then drop the response there so
      // send_on_delete fires on a foreign thread — historically overlapping
      // the still-draining send queue.
      server->resource["^/sse$"]["GET"] = [this](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request>) {
        std::thread([this, response]() mutable {
          response->close_connection_after_response = true;
          response->write({{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}});
          for (int i = 0; i < 5 && !stopping.load(); ++i) {
            *response << "data: tick " << i << "\n\n";
            response->send();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          // Dropped here, possibly with sends still queued.
        }).detach();
      };

      server_thread = std::thread([this]() {
        server->start([this](unsigned short p) {
          port.store(p);
        });
      });
      while (port.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    void TearDown() override {
      stopping.store(true);
      // Give detached SSE threads a beat to drop their responses first.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      server->stop();
      if (server_thread.joinable()) {
        server_thread.join();
      }
    }

    std::unique_ptr<HttpServer> server;
    std::thread server_thread;
    std::atomic<unsigned short> port {0};
    std::atomic<bool> stopping {false};
  };

  TEST_F(SimpleWebLifetime, KeepAliveAndForeignThreadSseStress) {
    constexpr int kClientThreads = 4;
    constexpr auto kDuration = std::chrono::seconds(3);

    std::atomic<int> ok {0};
    std::atomic<int> requests {0};
    std::vector<std::thread> clients;
    const auto deadline = std::chrono::steady_clock::now() + kDuration;
    const std::string host = "127.0.0.1:" + std::to_string(port.load());

    clients.reserve(kClientThreads);
    for (int t = 0; t < kClientThreads; ++t) {
      clients.emplace_back([&, t]() {
        while (std::chrono::steady_clock::now() < deadline) {
          try {
            HttpClient client(host);
            // Keep-alive loop on one connection: each cycle runs the
            // response-complete → streambuf-steal → re-read hand-off.
            for (int i = 0; i < 8; ++i) {
              auto r = client.request("GET", "/ping");
              requests.fetch_add(1);
              if (r->content.string() == "pong") {
                ok.fetch_add(1);
              }
            }
            // Odd threads also open an SSE stream and abandon it early,
            // forcing server-side response drops with writes in flight.
            if (t % 2 == 1) {
              try {
                client.request("GET", "/sse");
                requests.fetch_add(1);
              } catch (...) {
                // Early disconnects / timeouts are expected here.
              }
            }
          } catch (...) {
            // Connection-level errors (timeouts, resets) are acceptable;
            // the assertion is that the server survives and keeps serving.
          }
        }
      });
    }
    for (auto &c : clients) {
      c.join();
    }

    // The server must have survived the stress window and served real
    // traffic. If the old race fires, the process crashes outright (AV /
    // heap corruption), so reaching this point with traffic served is the
    // regression signal.
    EXPECT_GT(requests.load(), 0);
    EXPECT_GT(ok.load(), 0);

    // And it must still be alive and serving afterwards.
    HttpClient after(host);
    auto r = after.request("GET", "/ping");
    EXPECT_EQ(r->content.string(), "pong");
  }
}  // namespace
