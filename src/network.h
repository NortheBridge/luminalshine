/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <array>
#include <tuple>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <enet/enet.h>

// local includes
#include "utility.h"

namespace net {
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  net_e from_enum_string(const std::string_view &view);
  std::string_view to_enum_string(net_e net);

  net_e from_address(const std::string_view &view);

  /**
   * @brief Compare the leading bits of two IPv6 addresses.
   * @param a The first address, as 16 bytes in network byte order.
   * @param b The second address, as 16 bytes in network byte order.
   * @param prefix_len The number of leading bits to compare, clamped to 128. 128 requires an exact match; 0 matches everything.
   * @return true if the first prefix_len bits of both addresses are equal.
   */
  bool v6_prefix_match(const std::array<unsigned char, 16> &a, const std::array<unsigned char, 16> &b, unsigned prefix_len);

  /**
   * @brief Check whether an IPv6 address is on-link for any local network adapter.
   * @details Enumerates the IPv6 unicast addresses of operational (non-loopback) local adapters and
   *          checks the given address against each address's real per-address on-link prefix length,
   *          so peers reached over on-link global-unicast addresses can be classified as LAN.
   *          The adapter prefix list is cached with a short TTL and refreshed lazily.
   *          Caveat: On point-to-point or provider-bridged links where the ISP shares an on-link prefix
   *          across subscribers, on-link peers are classified LAN; the Web UI remains password-gated.
   *          Windows only; always returns false on other platforms.
   * @param addr The IPv6 address to check.
   * @return true if the address falls within an on-link prefix of an operational local adapter.
   */
  bool is_on_link_ipv6(const boost::asio::ip::address_v6 &addr);

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Get the address family enum value from a string.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Get the binding address to use based on config.
   * @param af Address family.
   * @return The configured bind address or wildcard if not configured.
   */
  std::string get_bind_address(af_e af);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
