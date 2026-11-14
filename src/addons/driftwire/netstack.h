// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire VPN (slice 5; docs/plans/driftwire-menu-and-tunnel.md, docs/design/28
// "The Tunnel"): the packets a virtual interface hands over become the
// connections the Tunnel already forwards. A small userspace TCP/IP stack,
// the app side of every TCP connection on the interface ("tun2socks"): each
// one accepted becomes a link (link.h) the VPN splices to one forward at the
// relay server (splice.h), exactly as a proxy connection; each UDP datagram
// is handed up with its flow, and the answers come back down.
//
// What it does, because the interface is a pipe on this machine (no loss to
// speak of, no reordering past the OS's own, an MTU the VPN sets):
//   - IPv4 and IPv6 (no options or extension headers past the first: a
//     packet with them, or a fragment, is dropped);
//   - TCP as the server of each connection: SYN answered at once (MSS, no
//     window scaling, no SACK), data taken in order (anything else is
//     dropped with a duplicate ACK; the OS sends it again), a bounded
//     receive window, the send side within the peer's window, go-back-N
//     retransmission on a timer, FIN both ways, RST, and an idle limit;
//   - UDP datagrams up and down;
//   - checksums made and checked (IPv4 header, TCP and UDP with the pseudo
//     header).
// Not: ICMP (a ping gets nothing), IP fragments, TCP options past MSS.
// lwIP was looked at (BSD-3, compatible) and left: it is not in vcpkg, and
// taking a connection to any address needs patches to it.
//
// Threads: input() from the device's reader, the links' read and write from
// splice threads, tick() from a timer; one lock, never held while a packet
// goes out (out() runs after it is let go). Portable (D9): no OS calls.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "addons/driftwire/link.h"
#include "addons/driftwire/net.h"

namespace mv::driftwire::vpn {

// One flow: the app's side (src) and where it was going (dst).
struct flow {
  bool v6 = false;
  std::uint8_t proto = 0;  // 6 TCP, 17 UDP
  net::address src, dst;
  std::uint16_t sport = 0, dport = 0;
  friend auto operator<=>(const flow&, const flow&) = default;
};

inline constexpr std::uint8_t kTcp = 6, kUdp = 17;
inline constexpr std::size_t kMtu = 1400;
inline constexpr std::size_t kRecvWindow = 64 * 1024 - 1;  // no window scaling
inline constexpr std::size_t kSendBuffer = 256 * 1024;
inline constexpr int kRtoMs = 400;
inline constexpr int kMaxRetries = 8;
inline constexpr int kIdleMs = 10 * 60 * 1000;
inline constexpr std::size_t kMaxConnections = 1024;

// Pure helpers (tested): the Internet checksum, and a packet made whole.
[[nodiscard]] std::uint16_t checksum(std::span<const std::uint8_t> bytes, std::uint32_t sum = 0) noexcept;
// A TCP or UDP packet from `f.dst` back to `f.src` (the reply direction),
// IPv4 or IPv6 as the flow is, checksums made.
struct tcp_fields {
  std::uint32_t seq = 0, ack = 0;
  std::uint8_t flags = 0;  // FIN 1, SYN 2, RST 4, PSH 8, ACK 16
  std::uint16_t window = 0;
  std::uint16_t mss = 0;  // an MSS option when non-zero (a SYN)
};
inline constexpr std::uint8_t kFin = 1, kSyn = 2, kRst = 4, kPsh = 8, kAck = 16;
[[nodiscard]] std::vector<std::uint8_t> make_tcp(const flow& f, const tcp_fields& t,
                                                 std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> make_udp(const flow& f, std::span<const std::uint8_t> payload);
// A packet read: its flow (as the app sent it), its TCP fields and payload.
struct parsed {
  flow f;
  tcp_fields tcp;
  std::span<const std::uint8_t> payload;
};
// nullopt: not TCP or UDP over IPv4 / IPv6 we take, or a bad checksum.
[[nodiscard]] std::optional<parsed> parse(std::span<const std::uint8_t> packet);

class stack {
 public:
  struct callbacks {
    // A packet to the interface (to the app). Never under the stack's lock.
    std::function<void(std::vector<std::uint8_t>)> out;
    // A TCP connection the app opened, accepted: its bytes as a link (the
    // VPN splices it to a forward to f.dst:f.dport). On the input thread.
    std::function<void(std::unique_ptr<link>, const flow&)> on_tcp;
    // A datagram the app sent.
    std::function<void(const flow&, std::span<const std::uint8_t>)> on_udp;
    // A resend now falls due at `due`, which may be sooner than the
    // next_deadline() the timer sleeps to: a connection came, a segment went
    // out with none outstanding, a backed-off timer was reset. Called under
    // the stack's lock, from any thread: take only a leaf lock (the runner's
    // timer mutex), never call back into the stack.
    std::function<void(std::chrono::steady_clock::time_point due)> timer;
  };
  explicit stack(callbacks cb);
  // Every connection reset. Every link on_tcp handed out is destroyed
  // before the stack (vpn::runner joins its streams first).
  ~stack();
  stack(const stack&) = delete;
  stack& operator=(const stack&) = delete;

  // A packet from the interface.
  void input(std::span<const std::uint8_t> packet);
  // A datagram back to the app on `f` (as on_udp named it).
  void udp_reply(const flow& f, std::span<const std::uint8_t> payload);
  // Retransmission and idle limits: call at next_deadline(), and again
  // whenever `callbacks::timer` says it may have moved.
  void tick();
  // When tick() next has work (a resend, an idle limit), just past it so the
  // tick that wakes then finds it due; nullopt with no connections. An idle
  // stack has nothing to do for kIdleMs, so its timer need not wake before.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_deadline() const;
  [[nodiscard]] std::size_t connections() const;

 private:
  struct conn;
  class tcp_link;
  friend class tcp_link;
  using clock = std::chrono::steady_clock;
  void on_tcp_packet(const parsed& p, std::vector<std::vector<std::uint8_t>>& out,
                     std::unique_ptr<link>& accepted);
  void send_data_locked(conn& c, std::vector<std::vector<std::uint8_t>>& out, bool resend);
  [[nodiscard]] static clock::time_point resend_due(const conn& c);
  void emit(std::vector<std::vector<std::uint8_t>>& out);
  // The link side.
  [[nodiscard]] result<std::size_t> read(const std::shared_ptr<conn>& c, std::span<std::uint8_t> into, int timeout_ms);
  [[nodiscard]] expected write(const std::shared_ptr<conn>& c, std::span<const std::uint8_t> bytes);
  void close(const std::shared_ptr<conn>& c, bool reset);

  callbacks cb_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<flow, std::shared_ptr<conn>> conns_;  // under m_
  bool stopping_ = false;                         // under m_
};

}  // namespace mv::driftwire::vpn
