/**
 * @file tests/unit/test_nvhttp_not_found.cpp
 * @brief Regression test for the GameStream default (not found) handler.
 *
 * nvhttp::not_found answers every GET without a route on both GameStream
 * servers, for example /unpair, which this host does not implement. It used to
 * stream a second, header-less "HTTP/1.1 404 NOT FOUND" block after the 200
 * response it had already written, so 88 bytes trailed the declared
 * Content-Length on the connection (NortheBridge/luminalshine#194).
 *
 * The test serves the real handler from a plain-HTTP Simple-Web-Server on an
 * ephemeral loopback port and reads the raw bytes until the server closes the
 * connection. The HTTPS server registers the same template, so the plain-HTTP
 * instantiation covers it without certificate fixtures. Every wait is bounded:
 * a regression fails the test instead of hanging the run.
 */
#include "../tests_common.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <Simple-Web-Server/server_http.hpp>
#include <src/nvhttp.h>
#include <string>
#include <string_view>
#include <thread>

namespace {
  using namespace std::chrono_literals;
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

  // The response the 26.09.1 host sent first, byte for byte. Moonlight clients
  // read status_code="404" from the body, so it must not change.
  constexpr std::string_view expected_head = "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n";
  constexpr std::string_view expected_body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root status_code=\"404\"/>";
  static_assert(expected_body.size() == 64);

  struct round_trip_t {
    std::string bytes;  ///< Everything the server sent, in order.
    SimpleWeb::error_code end;  ///< Why reading stopped: eof when the server closed the connection.
    bool timed_out = false;
  };

  /**
   * Send one raw request and read until the server closes the connection.
   * Asynchronous I/O on a private io_context bounded by run_for(), so no step
   * can block past the deadline.
   */
  round_trip_t round_trip(unsigned short port, const std::string &request, std::chrono::seconds deadline) {
    round_trip_t result;
    bool finished = false;
    SimpleWeb::io_context io;
    SimpleWeb::asio::ip::tcp::socket socket(io);

    const auto finish = [&](const SimpleWeb::error_code &ec) {
      result.end = ec;
      finished = true;
    };

    socket.async_connect(SimpleWeb::asio::ip::tcp::endpoint(SimpleWeb::make_address("127.0.0.1"), port), [&](const SimpleWeb::error_code &connect_ec) {
      if (connect_ec) {
        finish(connect_ec);
        return;
      }
      SimpleWeb::asio::async_write(socket, SimpleWeb::asio::buffer(request), [&](const SimpleWeb::error_code &write_ec, std::size_t) {
        if (write_ec) {
          finish(write_ec);
          return;
        }
        // Reads until an error; eof means the server closed the connection.
        SimpleWeb::asio::async_read(socket, SimpleWeb::asio::dynamic_buffer(result.bytes), [&](const SimpleWeb::error_code &read_ec, std::size_t) {
          finish(read_ec);
        });
      });
    });

    io.run_for(deadline);
    result.timed_out = !finished;

    // Cancel anything still pending and let it complete before its buffers go away.
    SimpleWeb::error_code ignored;
    socket.close(ignored);
    io.restart();
    io.run();
    return result;
  }

  class NvhttpNotFound: public ::testing::Test {
  protected:
    struct server_state_t {
      HttpServer http;
      std::promise<unsigned short> port;
      std::atomic<bool> stopped {false};
    };

    void SetUp() override {
      server->http.config.address = "127.0.0.1";
      server->http.config.port = 0;  // OS-assigned
      // Far past the client deadline, so only the handler can close the connection in time.
      server->http.config.timeout_request = 60;
      server->http.config.timeout_content = 60;
      server->http.default_resource["GET"] = nvhttp::not_found<SimpleWeb::HTTP>;

      auto port_ready = server->port.get_future();
      server_thread = std::thread([state = server]() {
        try {
          state->http.start([raw = state.get()](unsigned short bound_port) {
            raw->port.set_value(bound_port);
          });
        } catch (...) {
          try {
            state->port.set_exception(std::current_exception());
          } catch (const std::future_error &) {
            // The port was already published; the test body reports the failure.
          }
        }
        state->stopped.store(true);
      });

      ASSERT_EQ(port_ready.wait_for(10s), std::future_status::ready) << "the server did not start listening";
      port = port_ready.get();  // rethrows a failed start()
    }

    void TearDown() override {
      if (!server_thread.joinable()) {
        return;
      }
      // stop() does nothing until start() has opened the acceptor, so repeat
      // it until the server thread returns, within a deadline.
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (!server->stopped.load() && std::chrono::steady_clock::now() < deadline) {
        server->http.stop();
        std::this_thread::sleep_for(10ms);
      }
      if (server->stopped.load()) {
        server_thread.join();
      } else {
        ADD_FAILURE() << "the server thread did not stop";
        server_thread.detach();  // it holds its own reference to the server state
      }
    }

    std::shared_ptr<server_state_t> server = std::make_shared<server_state_t>();
    std::thread server_thread;
    unsigned short port = 0;
  };

  TEST_F(NvhttpNotFound, UnknownPathGetsOneResponseThenClose) {
    // Moonlight's unpair request: this host has no /unpair route.
    const std::string request =
      "GET /unpair?uniqueid=0123456789ABCDEF&uuid=00000000-0000-0000-0000-000000000000 HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "\r\n";

    const auto result = round_trip(port, request, 10s);

    // HTTP/1.1 defaults to keep-alive, so the close has to come from the handler.
    ASSERT_FALSE(result.timed_out) << "the server kept the connection open; received: " << result.bytes;
    EXPECT_EQ(result.end, SimpleWeb::asio::error::eof) << result.end.message();

    // Exactly the 200 response and its Content-Length body. A keep-alive client
    // would read anything after it as the start of the next response.
    EXPECT_EQ(result.bytes, std::string(expected_head) + std::string(expected_body));
  }
}  // namespace
