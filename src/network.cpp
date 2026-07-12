/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <vector>

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

#ifdef _WIN32
  // platform includes
  // clang-format off
  #include <winsock2.h>
  #include <ws2ipdef.h>
  #include <iphlpapi.h>
  // clang-format on
#endif

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("100.64.0.0/10"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  bool v6_prefix_match(const std::array<unsigned char, 16> &a, const std::array<unsigned char, 16> &b, unsigned prefix_len) {
    if (prefix_len > 128) {
      prefix_len = 128;
    }

    const auto full_bytes = prefix_len / 8;
    const auto remaining_bits = prefix_len % 8;

    if (!std::equal(a.begin(), a.begin() + full_bytes, b.begin())) {
      return false;
    }

    if (remaining_bits == 0) {
      return true;
    }

    const auto mask = static_cast<unsigned char>(0xFF << (8 - remaining_bits));
    return (a[full_bytes] & mask) == (b[full_bytes] & mask);
  }

#ifdef _WIN32
  namespace {
    struct onlink_prefix_t {
      std::array<unsigned char, 16> addr;
      unsigned prefix_len;
    };

    /**
     * @brief Enumerate the on-link IPv6 prefixes of operational local adapters.
     * @details Collects {address, OnLinkPrefixLength} pairs for every preferred IPv6 unicast address
     *          of operational, non-loopback adapters.
     * @return The collected prefix list; empty on failure.
     */
    std::vector<onlink_prefix_t> enumerate_onlink_prefixes() {
      std::vector<onlink_prefix_t> prefixes;

      constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

      ULONG size = 16 * 1024;
      std::vector<unsigned char> buffer(size);
      auto result = GetAdaptersAddresses(AF_INET6, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);

      // The adapter list can change between calls; retry with the reported size a few times
      for (int attempt = 0; result == ERROR_BUFFER_OVERFLOW && attempt < 3; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET6, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
      }

      if (result != NO_ERROR) {
        BOOST_LOG(warning) << "GetAdaptersAddresses() failed while collecting on-link IPv6 prefixes: "sv << result;
        return prefixes;
      }

      for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
          continue;
        }

        for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
          if (unicast->DadState != IpDadStatePreferred) {
            continue;
          }

          const auto *sa = unicast->Address.lpSockaddr;
          if (sa == nullptr || sa->sa_family != AF_INET6) {
            continue;
          }

          const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);

          onlink_prefix_t prefix {};
          std::copy_n(reinterpret_cast<const unsigned char *>(sin6->sin6_addr.s6_addr), prefix.addr.size(), prefix.addr.begin());
          prefix.prefix_len = std::min<unsigned>(unicast->OnLinkPrefixLength, 128);
          prefixes.emplace_back(prefix);
        }
      }

      return prefixes;
    }

    std::mutex onlink_prefix_mutex;
    std::vector<onlink_prefix_t> onlink_prefix_cache;
    std::chrono::steady_clock::time_point onlink_prefix_refreshed;
    bool onlink_prefix_cache_valid = false;
  }  // namespace

  bool is_on_link_ipv6(const boost::asio::ip::address_v6 &addr) {
    constexpr auto cache_ttl = std::chrono::seconds(60);

    std::lock_guard lock {onlink_prefix_mutex};

    const auto now = std::chrono::steady_clock::now();
    if (!onlink_prefix_cache_valid || now - onlink_prefix_refreshed >= cache_ttl) {
      onlink_prefix_cache = enumerate_onlink_prefixes();
      onlink_prefix_refreshed = now;
      onlink_prefix_cache_valid = true;
    }

    const std::array<unsigned char, 16> addr_bytes = addr.to_bytes();
    return std::any_of(onlink_prefix_cache.begin(), onlink_prefix_cache.end(), [&addr_bytes](const onlink_prefix_t &prefix) {
      return v6_prefix_match(addr_bytes, prefix.addr, prefix.prefix_len);
    });
  }
#else
  bool is_on_link_ipv6(const boost::asio::ip::address_v6 &) {
    return false;
  }
#endif

  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }

      // Global-unicast peers that are on-link for a local adapter are still LAN,
      // even though they fall outside the static LAN ranges above.
      if (is_on_link_ipv6(addr.to_v6())) {
        BOOST_LOG(debug) << "Classifying "sv << addr.to_string() << " as LAN: on-link IPv6 prefix match"sv;
        return LAN;
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  std::string_view af_to_any_address_string(const af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  std::string get_bind_address(const af_e af) {
    // If bind_address is configured, use it
    if (!config::sunshine.bind_address.empty()) {
      return config::sunshine.bind_address;
    }

    // Otherwise use the wildcard address for the given address family
    return std::string(af_to_any_address_string(af));
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    const auto bind_addr = net::get_bind_address(af);
    enet_address_set_host(&addr, bind_addr.c_str());
    enet_address_set_port(&addr, port);

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Sunshine";
  }
}  // namespace net
