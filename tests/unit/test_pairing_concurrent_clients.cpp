/**
 * @file tests/unit/test_pairing_concurrent_clients.cpp
 * @brief Tests for pairing requests from clients that share a uniqueid.
 *
 * Moonlight on Android and iOS sends the same constant uniqueid
 * (0123456789ABCDEF) from every device, so two devices that pair at the same
 * time reach the host with one uniqueid and two peer addresses. Each of them
 * has to get its own pending request, answered by the PIN typed for that
 * request and untouched by the other one. A single peer address may hold only
 * a small number of pending requests at a time.
 *
 * The tests serve the real nvhttp::pair handler from a plain-HTTP
 * Simple-Web-Server on an ephemeral loopback port. The HTTPS server registers
 * the same template, so the plain-HTTP instantiation covers it without
 * certificate fixtures. A peer is a client socket bound to its own 127.0.0.N
 * source address (the whole 127.0.0.0/8 block is loopback), which is the
 * address the handler sees. Every wait for an answer is bounded: a regression
 * fails the test instead of hanging the run.
 *
 * Pending exchanges live in global nvhttp state, and a held response must not
 * outlive its server. TearDown therefore ends every exchange a test started
 * (a phase that is out of order makes the host drop the exchange), checks that
 * nothing is left pending, and only then stops the server. If something is
 * left, the server is kept for the rest of the run so the failure stays in that
 * test. Each test uses its own source addresses so that tests cannot see each
 * other's exchanges.
 */
#include "../tests_common.h"

#include <atomic>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <set>
#include <Simple-Web-Server/server_http.hpp>
#include <src/nvhttp.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
  using tcp = SimpleWeb::asio::ip::tcp;

  // Every wait is bounded so a regression fails the test instead of hanging the run.
  constexpr auto kWait = 5s;

  // The uniqueid Moonlight on Android and iOS sends from every device.
  const std::string kSharedUniqueId = "0123456789ABCDEF";

  // The number of pending requests one peer address may hold.
  constexpr std::size_t kPendingPerPeer = 2;

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

  std::string hex_of(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(text.size() * 2);
    for (const unsigned char c : text) {
      hex += digits[c >> 4];
      hex += digits[c & 0x0f];
    }
    return hex;
  }

  /// A phase-one salt: 32 hex digits, different for each value of @p digit.
  std::string salt_of(const char digit) {
    return std::string(32, digit);
  }

  /// What the host answered, taken from the XML body of its response.
  struct pairing_reply_t {
    int status_code = 0;  ///< root.<xmlattr>.status_code; 0 when no XML body arrived.
    int paired = -1;  ///< root.paired; -1 when the element is absent.
  };

  /**
   * One client connection: connects from a given source address, sends a GET
   * request and keeps the connection open to read the answer, which the host
   * holds back for as long as a request is pending. Connecting and sending are
   * synchronous (loopback), so the host has the request as soon as the
   * constructor returns; only the read is asynchronous, driven by wait().
   */
  class peer_connection_t {
  public:
    peer_connection_t(const std::string &source, const unsigned short port, const std::string &target):
        request {"GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"} {
      SimpleWeb::error_code ec;
      socket.open(tcp::v4(), ec);
      if (!ec) {
        socket.bind(tcp::endpoint(SimpleWeb::make_address(source), 0), ec);
      }
      if (!ec) {
        socket.connect(tcp::endpoint(SimpleWeb::make_address("127.0.0.1"), port), ec);
      }
      if (!ec) {
        SimpleWeb::asio::write(socket, SimpleWeb::asio::buffer(request), ec);
      }
      if (ec) {
        error = ec.message();
        finished = true;
        return;
      }

      // Reads until an error; eof means the host closed the connection after its answer.
      SimpleWeb::asio::async_read(socket, SimpleWeb::asio::dynamic_buffer(bytes), [this](const SimpleWeb::error_code &, std::size_t) {
        finished = true;
      });
    }

    ~peer_connection_t() {
      // Cancel anything still pending and let it complete before its buffer goes away.
      SimpleWeb::error_code ignored;
      socket.close(ignored);
      io.restart();
      io.run();
    }

    peer_connection_t(const peer_connection_t &) = delete;
    peer_connection_t &operator=(const peer_connection_t &) = delete;

    /// Reads for at most @p timeout; true once the answer is complete.
    bool wait(const std::chrono::milliseconds timeout) {
      if (!finished) {
        if (io.stopped()) {
          io.restart();
        }
        io.run_for(timeout);
      }
      return finished;
    }

    bool answered() const {
      return finished;
    }

    /// Why connecting or sending failed; empty when the request went out.
    const std::string &connect_error() const {
      return error;
    }

    /// Everything received so far, headers included.
    const std::string &text() const {
      return bytes;
    }

    pairing_reply_t reply() const {
      pairing_reply_t reply;
      const auto body_start = bytes.find("\r\n\r\n");
      if (body_start == std::string::npos) {
        return reply;
      }

      std::istringstream body {bytes.substr(body_start + 4)};
      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_xml(body, tree);
      } catch (const boost::property_tree::ptree_error &) {
        return reply;
      }
      reply.status_code = tree.get<int>("root.<xmlattr>.status_code", 0);
      reply.paired = tree.get<int>("root.paired", -1);
      return reply;
    }

  private:
    const std::string request;
    SimpleWeb::io_context io;
    tcp::socket socket {io};
    std::string bytes;
    std::string error;
    bool finished = false;
  };

  /// The requests of @p address that wait for a PIN or for the client's next phase.
  std::vector<nvhttp::pairing_request_t> pending_from(const std::string &address) {
    std::vector<nvhttp::pairing_request_t> pending;
    for (auto &request : nvhttp::get_pairing_requests()) {
      // Finished exchanges (FAILED, EXPIRED, PAIRED) stay listed for a while; they are not pending.
      const bool open = request.state == nvhttp::pairing_state_e::PENDING_PIN ||
                        request.state == nvhttp::pairing_state_e::AWAITING_CLIENT;
      if (open && request.client_address == address) {
        pending.push_back(std::move(request));
      }
    }
    return pending;
  }

  class PairingConcurrentClients: public ::testing::Test {
  protected:
    struct server_state_t {
      HttpServer http;
      std::promise<unsigned short> port;
      std::atomic<bool> stopped {false};
    };

    void SetUp() override {
      server->http.config.address = "127.0.0.1";
      server->http.config.port = 0;  // OS-assigned
      server->http.config.thread_pool_size = 1;  // the production setting: one io thread
      // A held request must outlive every wait below, so only the handler ends a connection in time.
      server->http.config.timeout_request = 60;
      server->http.config.timeout_content = 60;
      server->http.resource["^/pair$"]["GET"] = [queue = add_cert](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request) mutable {
        nvhttp::pair<SimpleWeb::HTTP>(queue, std::move(response), std::move(request));
      };

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

      const bool drained = port == 0 || end_all_exchanges();

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

      if (!drained) {
        // A pending exchange still holds a response whose deleter refers to
        // this server. Keep the server alive (never destroyed, not even at
        // exit) rather than leave that response dangling for a later test.
        static auto *const kept = new std::vector<std::shared_ptr<server_state_t>>;
        kept->push_back(server);
      }
    }

    /// Sends GET @p target from @p source and keeps the connection for the answer.
    peer_connection_t &send(const std::string &source, const std::string &target) {
      addresses.insert(source);
      connections.push_back(std::make_unique<peer_connection_t>(source, port, target));
      auto &connection = *connections.back();
      if (!connection.connect_error().empty()) {
        ADD_FAILURE() << "could not send from " << source << ": " << connection.connect_error();
      }
      return connection;
    }

    /// Sends phase one of a pairing exchange: the request the host holds until a PIN arrives.
    peer_connection_t &phase_one(const std::string &source, const std::string &unique_id, const std::string &salt, const std::string &certificate) {
      started.emplace(source, unique_id);
      return send(source, "/pair?uniqueid=" + unique_id + "&phrase=getservercert&salt=" + salt + "&clientcert=" + hex_of(certificate));
    }

    /// Waits until @p address holds at least @p count pending requests; @p connection is the request that should be among them.
    ::testing::AssertionResult becomes_pending(const std::string &address, peer_connection_t &connection, const std::size_t count = 1) {
      wait_until([&]() {
        connection.wait(1ms);  // also notices an answer that arrives instead of a pending request
        return pending_from(address).size() >= count || connection.answered();
      });

      const auto pending = pending_from(address).size();
      if (pending >= count) {
        return ::testing::AssertionSuccess();
      }
      return ::testing::AssertionFailure()
             << address << " holds " << pending << " pending request(s), expected " << count << "; "
             << (connection.answered() ? "the host answered the request instead: " + connection.text() : std::string {"no answer arrived either"});
    }

    // Moves every exchange this test started out of the pairing map. The phase
    // below is out of order in all of them (and too short in the last phase),
    // so the host drops the exchange and answers the request it was holding.
    // Returns false if a request is still pending afterwards.
    bool end_all_exchanges() {
      for (const auto &[source, unique_id] : started) {
        auto &connection = send(source, "/pair?uniqueid=" + unique_id + "&clientpairingsecret=00");
        EXPECT_TRUE(connection.wait(kWait)) << "no answer to the request that ends the exchange of " << source;
      }
      for (const auto &connection : connections) {
        EXPECT_TRUE(connection->wait(kWait)) << "a request was left without an answer: " << connection->text();
      }
      bool drained = true;
      for (const auto &address : addresses) {
        const bool empty = wait_until([&]() {
          return pending_from(address).empty();
        });
        EXPECT_TRUE(empty) << "pairing requests from " << address << " were left behind";
        drained = drained && empty;
      }
      return drained;
    }

    std::shared_ptr<server_state_t> server = std::make_shared<server_state_t>();
    std::shared_ptr<safe::queue_t<crypto::x509_t>> add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);
    std::thread server_thread;
    unsigned short port = 0;

    std::set<std::pair<std::string, std::string>> started;  ///< (source address, uniqueid) of every phase one sent
    std::set<std::string> addresses;  ///< every source address used
    std::vector<std::unique_ptr<peer_connection_t>> connections;
  };

  TEST_F(PairingConcurrentClients, PeersSharingAUniqueIdEachGetAPendingRequest) {
    auto &first = phase_one("127.0.0.2", kSharedUniqueId, salt_of('a'), "cert-a");
    ASSERT_TRUE(becomes_pending("127.0.0.2", first));

    auto &second = phase_one("127.0.0.3", kSharedUniqueId, salt_of('b'), "cert-b");
    ASSERT_TRUE(becomes_pending("127.0.0.3", second));

    const auto first_pending = pending_from("127.0.0.2");
    const auto second_pending = pending_from("127.0.0.3");
    ASSERT_EQ(first_pending.size(), 1U);
    ASSERT_EQ(second_pending.size(), 1U);
    EXPECT_NE(first_pending[0].pairing_id, second_pending[0].pairing_id);
    EXPECT_EQ(first_pending[0].state, nvhttp::pairing_state_e::PENDING_PIN);
    EXPECT_EQ(second_pending[0].state, nvhttp::pairing_state_e::PENDING_PIN);
  }

  TEST_F(PairingConcurrentClients, PinReachesOnlyTheRequestItIsAddressedTo) {
    auto &first = phase_one("127.0.0.4", kSharedUniqueId, salt_of('a'), "cert-a");
    ASSERT_TRUE(becomes_pending("127.0.0.4", first));
    auto &second = phase_one("127.0.0.5", kSharedUniqueId, salt_of('b'), "cert-b");
    ASSERT_TRUE(becomes_pending("127.0.0.5", second));

    const auto first_pending = pending_from("127.0.0.4");
    const auto second_pending = pending_from("127.0.0.5");
    ASSERT_EQ(first_pending.size(), 1U);
    ASSERT_EQ(second_pending.size(), 1U);

    const auto delivery = nvhttp::submit_pin("1234", "device-b", second_pending[0].pairing_id);
    EXPECT_EQ(delivery.result, nvhttp::pin_result_e::PIN_DELIVERED);
    EXPECT_EQ(delivery.pairing_id, second_pending[0].pairing_id);

    // The second peer's held request is answered: phase one succeeded.
    ASSERT_TRUE(second.wait(kWait)) << "the PIN did not reach the request it was addressed to";
    const auto reply = second.reply();
    EXPECT_EQ(reply.status_code, 200) << second.text();
    EXPECT_EQ(reply.paired, 1) << second.text();

    // The first peer is still waiting for a PIN of its own.
    EXPECT_FALSE(first.wait(200ms)) << "the first peer was answered: " << first.text();
    const auto first_after = pending_from("127.0.0.4");
    ASSERT_EQ(first_after.size(), 1U);
    EXPECT_EQ(first_after[0].pairing_id, first_pending[0].pairing_id);
    EXPECT_EQ(first_after[0].state, nvhttp::pairing_state_e::PENDING_PIN);
  }

  TEST_F(PairingConcurrentClients, LaterPhaseFromAnotherPeerLeavesAPendingRequestAlone) {
    auto &first = phase_one("127.0.0.6", kSharedUniqueId, salt_of('a'), "cert-a");
    ASSERT_TRUE(becomes_pending("127.0.0.6", first));
    const auto before = pending_from("127.0.0.6");
    ASSERT_EQ(before.size(), 1U);

    // Same uniqueid, other peer, a phase that only the first peer's exchange could answer.
    auto &other = send("127.0.0.7", "/pair?uniqueid=" + kSharedUniqueId + "&clientpairingsecret=00");
    ASSERT_TRUE(other.wait(kWait)) << "the host did not answer the request of the other peer";
    EXPECT_NE(other.reply().status_code, 200) << other.text();
    EXPECT_NE(other.reply().paired, 1) << other.text();

    EXPECT_FALSE(first.wait(200ms)) << "the first peer was answered: " << first.text();
    const auto after = pending_from("127.0.0.6");
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(after[0].pairing_id, before[0].pairing_id);
    EXPECT_EQ(after[0].state, nvhttp::pairing_state_e::PENDING_PIN);
  }

  TEST_F(PairingConcurrentClients, RetryFromTheSamePeerKeepsOnePendingRequest) {
    auto &first = phase_one("127.0.0.8", kSharedUniqueId, salt_of('a'), "cert-a");
    ASSERT_TRUE(becomes_pending("127.0.0.8", first));
    const auto original = pending_from("127.0.0.8");
    ASSERT_EQ(original.size(), 1U);

    // Moonlight retries with the same certificate and a fresh salt while it waits for the PIN.
    auto &retry = phase_one("127.0.0.8", kSharedUniqueId, salt_of('b'), "cert-a");
    ASSERT_TRUE(first.wait(kWait)) << "the replaced request was not answered";
    EXPECT_EQ(first.reply().status_code, 409) << first.text();

    const auto current = pending_from("127.0.0.8");
    ASSERT_EQ(current.size(), 1U);
    EXPECT_EQ(current[0].pairing_id, original[0].pairing_id);
    EXPECT_FALSE(retry.wait(200ms)) << "the retry was answered instead of held: " << retry.text();
  }

  TEST_F(PairingConcurrentClients, OnePeerCanHoldAtMostTwoPendingRequests) {
    static_assert(kPendingPerPeer == 2);
    auto &first = phase_one("127.0.0.9", "00000000000000A1", salt_of('a'), "cert-a1");
    ASSERT_TRUE(becomes_pending("127.0.0.9", first, 1));
    auto &second = phase_one("127.0.0.9", "00000000000000A2", salt_of('b'), "cert-a2");
    ASSERT_TRUE(becomes_pending("127.0.0.9", second, 2));

    // The third request is refused right away instead of being held.
    auto &third = phase_one("127.0.0.9", "00000000000000A3", salt_of('c'), "cert-a3");
    ASSERT_TRUE(third.wait(kWait)) << "the third request of one peer was held instead of being answered";
    EXPECT_EQ(third.reply().status_code, 503) << third.text();
    EXPECT_EQ(third.reply().paired, 0) << third.text();
    EXPECT_EQ(pending_from("127.0.0.9").size(), kPendingPerPeer);

    // The limit belongs to the peer address: another peer can still open a request.
    auto &other = phase_one("127.0.0.10", "00000000000000B1", salt_of('d'), "cert-b1");
    EXPECT_TRUE(becomes_pending("127.0.0.10", other));
  }

  TEST_F(PairingConcurrentClients, PeerAtItsLimitCanStillRetryAPendingRequest) {
    auto &first = phase_one("127.0.0.11", "00000000000000C1", salt_of('a'), "cert-c1");
    ASSERT_TRUE(becomes_pending("127.0.0.11", first, 1));
    auto &second = phase_one("127.0.0.11", "00000000000000C2", salt_of('b'), "cert-c2");
    ASSERT_TRUE(becomes_pending("127.0.0.11", second, 2));

    // A retry replaces its own pending request, so it does not count against the limit.
    auto &retry = phase_one("127.0.0.11", "00000000000000C1", salt_of('c'), "cert-c1");
    ASSERT_TRUE(first.wait(kWait)) << "the replaced request was not answered";
    EXPECT_EQ(first.reply().status_code, 409) << first.text();
    EXPECT_FALSE(retry.wait(200ms)) << "the retry was answered instead of held: " << retry.text();
    EXPECT_EQ(pending_from("127.0.0.11").size(), kPendingPerPeer);
  }

  TEST_F(PairingConcurrentClients, OutOfOrderPhaseEndsOnlyThePeersOwnRequest) {
    auto &first = phase_one("127.0.0.12", kSharedUniqueId, salt_of('a'), "cert-a");
    ASSERT_TRUE(becomes_pending("127.0.0.12", first));
    auto &second = phase_one("127.0.0.13", kSharedUniqueId, salt_of('b'), "cert-b");
    ASSERT_TRUE(becomes_pending("127.0.0.13", second));
    const auto before = pending_from("127.0.0.12");
    ASSERT_EQ(before.size(), 1U);

    // Out of order for the second peer's exchange, so the host ends that exchange.
    auto &out_of_order = send("127.0.0.13", "/pair?uniqueid=" + kSharedUniqueId + "&clientpairingsecret=00");
    ASSERT_TRUE(out_of_order.wait(kWait)) << "the host did not answer the out-of-order phase";
    EXPECT_EQ(out_of_order.reply().status_code, 400) << out_of_order.text();
    ASSERT_TRUE(second.wait(kWait)) << "the ended exchange's held request was not answered";
    EXPECT_EQ(second.reply().status_code, 400) << second.text();
    EXPECT_TRUE(pending_from("127.0.0.13").empty());

    // The first peer's request with the same uniqueid is still waiting for its PIN.
    EXPECT_FALSE(first.wait(200ms)) << "the first peer was answered: " << first.text();
    const auto after = pending_from("127.0.0.12");
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(after[0].pairing_id, before[0].pairing_id);
    EXPECT_EQ(after[0].state, nvhttp::pairing_state_e::PENDING_PIN);
  }
}  // namespace
