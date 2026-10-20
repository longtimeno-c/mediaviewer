// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's network port (D9: a portable header, net_posix.cpp for macOS
// and the portable build, net_win.cpp over Winsock).
//
// Only Driftwire opens these, only while its window is open or receiving is
// on (docs/design/28 "What stays put"): the idle viewer has no socket. On the
// local network, IPv4 (the beacon goes out with a TTL of 1). Over the
// internet (#472 PR 2, opt-in, off by default): a TCP listener on IPv6 and
// IPv4 while a code waits, connects to the addresses in a code, and the
// router's port mapping (UDP to the gateway, SSDP and HTTP to it). #472 PR 3
// on: one dual-stack UDP socket and, for the birthday punch, many more read
// together (wait_readable), and a TCP port that listens and connects at once
// (tcp_rendezvous). Nothing here resolves a name. Nothing is logged with an
// address.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/link.h"
#include "core/result.h"

namespace mv::driftwire::net {

struct endpoint {
  std::string address;  // dotted IPv4 (a beacon's sender), or IPv6 text (#472)
  std::uint16_t port = 0;
};

// An IP address as bytes (IPv4 in the first 4).
struct address {
  bool v6 = false;
  std::array<std::uint8_t, 16> bytes{};
  friend bool operator==(const address&, const address&) = default;
  friend auto operator<=>(const address&, const address&) = default;  // a map key (#472 relays)
};
// Dotted IPv4 or IPv6 text (no zone, no brackets). Portable (net_address.cpp).
[[nodiscard]] std::optional<address> parse_address(std::string_view text) noexcept;
[[nodiscard]] std::string address_text(const address& a);
// Reachable from the internet if nothing in between blocks it: not loopback,
// private (RFC 1918), shared (CGNAT, RFC 6598), link-local, unique local
// (fc00::/7), multicast, documentation or unspecified.
[[nodiscard]] bool is_public(const address& a) noexcept;
// An IPv4 address of a private network (RFC 1918: 10/8, 172.16/12, 192.168/16).
[[nodiscard]] bool is_private_v4(const address& a) noexcept;

// A TCP listener on a port the OS picks: on every interface, or on loopback
// only (the tests).
class listener {
 public:
  virtual ~listener() = default;
  [[nodiscard]] virtual std::uint16_t port() const noexcept = 0;
  // The next connection; status::timeout when none came within `timeout_ms`;
  // status::cancelled after shutdown().
  [[nodiscard]] virtual result<std::unique_ptr<link>> accept(int timeout_ms) = 0;
  // As accept, and the peer's address (#472 relays: the relay server's caps
  // per source; never logged).
  [[nodiscard]] virtual result<std::unique_ptr<link>> accept_from(int timeout_ms, address& from) {
    from = {};
    return accept(timeout_ms);
  }
  virtual void shutdown() noexcept = 0;
};
[[nodiscard]] result<std::unique_ptr<listener>> listen(bool loopback_only = false);

// Connects within `timeout_ms`, over IPv4 or IPv6 as the address is.
[[nodiscard]] result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms);

// #472 PR 2: the listener a code points at. TCP on `port` (0: the OS picks),
// IPv6 and IPv4 both ([::] dual stack; IPv4 alone where there is no IPv6).
// `loopback_only`: ::1 and 127.0.0.1 only (the tests).
[[nodiscard]] result<std::unique_ptr<listener>> listen_internet(std::uint16_t port, bool loopback_only);

// This machine's unicast addresses, every interface that is up.
[[nodiscard]] std::vector<address> local_addresses();
// #502: an interface's network, its address and prefix length.
struct subnet {
  address base;
  int prefix = 0;
};
// The networks of every interface that is up (loopback's too).
[[nodiscard]] std::vector<subnet> local_subnets();
// Whether `a` is on one of `nets` (on this machine's link), or loopback, or
// link-local (169.254/16, fe80::/10): what the LAN listener and the beacon
// take (#502). Portable (net_address.cpp).
[[nodiscard]] bool on_link(const address& a, std::span<const subnet> nets) noexcept;
// The IPv4 default gateway (the router a port mapping asks), if any.
[[nodiscard]] std::optional<address> default_gateway();
// This machine's address on the route to `peer` (a UDP socket connected to
// it, then asked; nothing is sent): what a mapping names as its client.
[[nodiscard]] std::optional<address> local_address_toward(const address& peer);

// A UDP socket: the router's mapping protocols (NAT-PMP, PCP) and UPnP's
// SSDP search. Never the beacon's port.
class udp_socket {
 public:
  virtual ~udp_socket() = default;
  [[nodiscard]] virtual expected send_to(std::span<const std::uint8_t> datagram, const endpoint& to) = 0;
  // One datagram and its sender; status::timeout when none came.
  [[nodiscard]] virtual result<std::size_t> receive_from(std::span<std::uint8_t> into, int timeout_ms,
                                                         endpoint& from) = 0;
  virtual void shutdown() noexcept = 0;
  [[nodiscard]] virtual std::uint16_t port() const noexcept = 0;
  // The OS's socket (an fd, a SOCKET), for wait_readable only.
  [[nodiscard]] virtual std::intptr_t handle() const noexcept = 0;
  // The kernel's send and receive buffers, each `bytes` (best effort).
  virtual void set_buffers(int bytes) noexcept = 0;
};
// IPv4 (or IPv6) on a port the OS picks, bound at once; `multicast_ttl`: the
// TTL of a multicast send (SSDP asks for 2). `loopback_only`: bound to
// 127.0.0.1 / ::1 (the tests' stand-in routers).
[[nodiscard]] result<std::unique_ptr<udp_socket>> open_udp(bool v6, int multicast_ttl = 1, bool loopback_only = false);
// #472 PR 3: one socket for IPv6 and IPv4 both (IPv4 peers as plain dotted
// addresses either way), on `port` (0: the OS picks): the DHT, hole punching
// and the transfer share it, so the hole a probe punches is the one the
// transfer uses. `loopback_only`: ::1 and 127.0.0.1 (the tests).
// `buffer_bytes`: the kernel's buffers (a fast transfer's bursts want the
// default; the birthday punch's 256 extras ask for little, set_buffers()
// grows the one a stream then runs on).
inline constexpr int kUdpBuffers = 4 * 1024 * 1024;
[[nodiscard]] result<std::unique_ptr<udp_socket>> open_udp_dual(std::uint16_t port, bool loopback_only = false,
                                                              int buffer_bytes = kUdpBuffers);
// #472: which of `socks` have a datagram waiting, within `timeout_ms`
// (ready[i] set for each): the hub reads its own socket and the birthday
// punch's many from one thread. The count; status::timeout when none.
[[nodiscard]] result<std::size_t> wait_readable(std::span<udp_socket* const> socks, std::span<bool> ready,
                                                int timeout_ms);
// #472: room for about `n` more sockets in this process. POSIX raises the
// soft RLIMIT_NOFILE (the Mac's default of 256 cannot hold the birthday
// punch's sockets) to 1024 + n, within the hard limit, for the rest of the
// process's life (lowering it again under descriptors other code may have
// opened since would make their next open fail); Windows has no such limit.
// A socket that still does not open is one fewer, not a failure.
void reserve_sockets(int n) noexcept;

// #472: TCP simultaneous open. One port, bound for listening and connecting
// at once (SO_REUSEADDR, and SO_REUSEPORT where the OS has it): connect()s go
// out from it to the peer's candidates while it listens, so a SYN that
// crosses ours in the two NATs opens one connection (simultaneous open), and
// one that arrives after ours was refused or dropped is accepted. A refused
// or failed connect() is tried again, from the same port, every kRetryMs.
// The OSes share the port only within one user account; a process of the
// same user that takes a connection fails the meeting's authenticated hello
// (meet.cpp, #499) and is passed over.
class tcp_rendezvous {
 public:
  static constexpr int kRetryMs = 250;
  virtual ~tcp_rendezvous() = default;
  [[nodiscard]] virtual std::uint16_t port() const noexcept = 0;
  // The next connection to or from one of `peers` (one that comes in is
  // matched by address only: a NAT may change its port), within
  // `timeout_ms`; status::timeout when none, status::cancelled after
  // shutdown(). Attempts still under way carry on into the next call.
  [[nodiscard]] virtual result<std::unique_ptr<link>> next(std::span<const endpoint> peers, int timeout_ms) = 0;
  virtual void shutdown() noexcept = 0;
};
// IPv6 and IPv4 ([::] dual stack; IPv4 alone where there is no IPv6), on a
// port the OS picks; `loopback_only`: 127.0.0.1 (the tests).
[[nodiscard]] result<std::unique_ptr<tcp_rendezvous>> open_rendezvous(bool loopback_only = false);

// A name to addresses (the DHT's bootstrap routers only; nothing else in
// Driftwire resolves a name).
[[nodiscard]] std::vector<address> resolve(const std::string& name);

// The discovery socket: UDP on `port`, joined to kMulticastGroup, sharing the
// port with other MediaViewers on this machine.
class beacon_socket {
 public:
  virtual ~beacon_socket() = default;
  // To the multicast group and to the broadcast address, TTL 1.
  [[nodiscard]] virtual expected send(std::span<const std::uint8_t> datagram) = 0;
  // One datagram and its sender's address; status::timeout when none.
  [[nodiscard]] virtual result<std::size_t> receive(std::span<std::uint8_t> into, int timeout_ms,
                                                    std::string& from_address) = 0;
  virtual void shutdown() noexcept = 0;
};
[[nodiscard]] result<std::unique_ptr<beacon_socket>> open_beacon_socket(std::uint16_t port);

// The name people know this machine by ("Tristan's MacBook Air", the
// computer name on Windows). Never a user name.
[[nodiscard]] std::string machine_name();

// The person's Downloads folder, where received files land unless they pick
// another (#442, as AirDrop): FOLDERID_Downloads on Windows, ~/Downloads on
// the Mac. "" when there is none. A local read; never blocks on the network.
[[nodiscard]] std::string downloads_folder();

// One process at a time: each MediaViewer window is its own process
// (docs/design/18 "One host process"), and only one of them keeps receiving
// at launch when Keep receiving was on (#442). Held until destroyed; the OS
// lets go of it if the process dies. nullptr when another process holds it.
class instance_lock {
 public:
  virtual ~instance_lock() = default;
};
[[nodiscard]] std::unique_ptr<instance_lock> try_lock_instance(const std::string& path);

}  // namespace mv::driftwire::net
