/**
 * @file tests/unit/test_launch_off_io_thread.cpp
 * @brief Regression test for the 26.09.1 report: a long HTTPS /launch froze
 *        every other GameStream request.
 *
 * The HTTPS server runs one io thread (SimpleWeb::ServerBase's default
 * thread_pool_size of 1), and /launch used to run on it for about 14 seconds
 * (display preparation, encoder probe, application start). A second client's
 * GET /serverinfo got no answer in that time. nvhttp now enqueues /launch,
 * /resume and /cancel on a util::serial_executor_t and writes the response from
 * that worker; the route lambdas below mirror that pattern.
 *
 * Like test_simpleweb_lifetime.cpp this uses the plain-HTTP server
 * instantiation: the code involved lives in ServerBase, which the HTTPS
 * instantiation shares, and plain HTTP avoids certificate fixtures.
 */

#include "src/serial_executor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <Simple-Web-Server/client_http.hpp>
#include <Simple-Web-Server/server_http.hpp>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
  using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;

  // Every wait is bounded so a regression fails the test instead of hanging the run.
  constexpr auto kWait = 5s;

  /// One-shot latch the test opens to let the blocked "launch" finish.
  class gate_t {
  public:
    void open() {
      {
        std::lock_guard lock {mutex};
        opened = true;
      }
      cv.notify_all();
    }

    bool wait(std::chrono::seconds timeout) {
      std::unique_lock lock {mutex};
      return cv.wait_for(lock, timeout, [&]() {
        return opened;
      });
    }

  private:
    std::mutex mutex;
    std::condition_variable cv;
    bool opened {false};
  };

  /// Opens the gate when the test body unwinds, before any client thread is joined.
  struct open_on_exit_t {
    gate_t &gate;

    ~open_on_exit_t() {
      gate.open();
    }
  };

  template<class Predicate>
  bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return predicate();
  }

  class LaunchOffIoThread: public ::testing::Test {
  protected:
    void SetUp() override {
      server = std::make_unique<HttpServer>();
      server->config.port = 0;  // OS-assigned
      server->config.thread_pool_size = 1;  // the production setting: one io thread

      // Answered inline on the io thread, like serverinfo.
      server->resource["^/ping$"]["GET"] = [](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request>) {
        response->write("pong");
      };

      // Like /launch: enqueued, then answered from the worker thread once the
      // long operation (here: the gate) is done.
      server->resource["^/slow$"]["GET"] = [this](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request) {
        const bool queued = executor.post([this, response, request]() {
          slow_started.store(true);
          gate.wait(10s);
          response->write("slow-done");
          response->close_connection_after_response = true;
        });
        if (!queued) {
          response->write(SimpleWeb::StatusCode::server_error_service_unavailable);
        }
      };

      // A task that has to wait behind /slow in the FIFO queue.
      server->resource["^/queued$"]["GET"] = [this](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request>) {
        const bool queued = executor.post([this, response]() {
          queued_ran.store(true);
          response->write("queued-done");
          response->close_connection_after_response = true;
        });
        if (queued) {
          queued_posted.store(true);
        } else {
          response->write(SimpleWeb::StatusCode::server_error_service_unavailable);
        }
      };

      server_thread = std::thread([this]() {
        server->start([this](unsigned short p) {
          port.store(p);
        });
      });
      ASSERT_TRUE(wait_until([this]() {
        return port.load() != 0;
      }));
      host = "127.0.0.1:" + std::to_string(port.load());
    }

    void TearDown() override {
      gate.open();
      executor.stop();
      server->stop();
      if (server_thread.joinable()) {
        server_thread.join();
      }
    }

    /// GET @p path on a fresh connection; returns false if the request failed or timed out.
    bool get(const std::string &path, long timeout_seconds, std::string &body) {
      try {
        HttpClient client(host);
        client.config.timeout = timeout_seconds;
        body = client.request("GET", path)->content.string();
        return true;
      } catch (...) {
        return false;
      }
    }

    std::unique_ptr<HttpServer> server;
    // Declared after the server so it is destroyed first: no queued task may
    // outlive the server whose responses it holds.
    util::serial_executor_t executor;
    std::thread server_thread;
    std::atomic<unsigned short> port {0};
    std::string host;

    gate_t gate;
    std::atomic<bool> slow_started {false};
    std::atomic<bool> queued_posted {false};
    std::atomic<bool> queued_ran {false};
  };

  TEST_F(LaunchOffIoThread, PingIsAnsweredWhileSlowIsInProgress) {
    std::string slow_body;
    std::atomic<bool> slow_done {false};
    std::jthread slow_client {[&]() {
      get("/slow", 15, slow_body);
      slow_done.store(true);
    }};
    open_on_exit_t release_on_exit {gate};

    ASSERT_TRUE(wait_until([this]() {
      return slow_started.load();
    }));

    // The other client's request: answered promptly although /slow is still running.
    std::string ping_body;
    const bool ping_ok = get("/ping", 3, ping_body);
    EXPECT_TRUE(ping_ok) << "/ping got no answer while /slow was in progress";
    EXPECT_EQ(ping_body, "pong");
    EXPECT_FALSE(slow_done.load()) << "/slow finished before the test released it";

    gate.open();
    ASSERT_TRUE(wait_until([&]() {
      return slow_done.load();
    }));
    EXPECT_EQ(slow_body, "slow-done");
  }

  TEST_F(LaunchOffIoThread, StoppingWithAPendingTaskTerminatesCleanly) {
    std::string slow_body;
    std::string queued_body;
    std::atomic<bool> slow_done {false};
    std::jthread slow_client {[&]() {
      get("/slow", 15, slow_body);
      slow_done.store(true);
    }};
    std::jthread queued_client {[&]() {
      get("/queued", 15, queued_body);
    }};
    open_on_exit_t release_on_exit {gate};

    ASSERT_TRUE(wait_until([this]() {
      return slow_started.load();
    }));
    ASSERT_TRUE(wait_until([this]() {
      return queued_posted.load();
    }));

    // Stop while /slow runs and /queued waits behind it.
    std::jthread stopper {[this]() {
      executor.stop();
    }};
    EXPECT_TRUE(wait_until([this]() {
      return !executor.post([]() {
      });
    }))
      << "stop() kept accepting work";

    gate.open();
    stopper.join();

    // The running task completed and answered; the queued one was dropped.
    ASSERT_TRUE(wait_until([&]() {
      return slow_done.load();
    }));
    EXPECT_EQ(slow_body, "slow-done");
    EXPECT_FALSE(queued_ran.load());

    // The server stops with the dropped task's response still open; the
    // waiting client is released by the connection close, not left hanging.
    server->stop();
    server_thread.join();
    queued_client.join();
    EXPECT_TRUE(queued_body.empty());
  }
}  // namespace
