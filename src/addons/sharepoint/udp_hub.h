// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One UDP socket for everything over the internet that is not a code's TCP
// link (issue #472 PR 3): the DHT's queries, the hole-punching probes, and
// the transfer itself. The same socket matters: the hole a probe punches in
// a NAT is the one the transfer then uses (Network.framework cannot share a
// local UDP port across connections, hence POSIX sockets on both hosts,
// net.h).
//
// The first byte says what a datagram is: 'd' a KRPC message (bencoded
// dict), kProbe a punching probe, kStream a packet of a reliable stream.
//
// udp_stream: a reliable, ordered byte pipe over the hub (a link, so the
// session runs over it as over TCP). The spike of #472 asked "QUIC (ngtcp2,
// msquic) or Noise over a simple UDP transport of our own": this is the
// latter, because the link must share the socket with the DHT and the
// punching on both OSes (msquic owns its sockets and does not run on macOS;
// ngtcp2 needs a TLS stack beside Noise), and the session above already
// seals, orders and resumes. It is deliberately small: 1200-byte packets
// (safe under any MTU), cumulative and selective acknowledgements, RTO with
// Karn's rule, fast retransmit after three later packets, NewReno-style
// congestion control, the receiver's free space as a window. No pacing yet;
// a burst is at most the congestion window. Packets are not encrypted here:
// what goes through is the channel's sealed frames (an injected packet fails
// the channel's AEAD, which ends the link, as a forged TCP segment would).
// Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "addons/sharepoint/link.h"
#include "addons/sharepoint/net.h"

namespace mv::sharepoint {

inline constexpr std::uint8_t kProbe = 0xA6;
inline constexpr std::uint8_t kStream = 0xB7;
inline constexpr std::size_t kStreamPayload = 1200;

class udp_hub {
 public:
  using datagram_fn = std::function<void(std::span<const std::uint8_t>, const net::endpoint& from)>;
  // `port` 0: the OS picks.
  [[nodiscard]] static result<std::shared_ptr<udp_hub>> open(std::uint16_t port, bool loopback_only = false);
  ~udp_hub();
  udp_hub(const udp_hub&) = delete;
  udp_hub& operator=(const udp_hub&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] expected send(std::span<const std::uint8_t> datagram, const net::endpoint& to);
  // Handlers, on the hub's reader thread (brief; never blocking).
  void on_dht(datagram_fn fn);
  void on_probe(datagram_fn fn);
  // A reliable stream to `peer`, both sides with the same `id` (the sender
  // picks it during punching). Packets for it from anyone else are dropped.
  [[nodiscard]] std::unique_ptr<link> stream(const net::endpoint& peer, std::uint32_t id);
  // Stops the threads; every stream fails. Idempotent.
  void stop() noexcept;
  // The tests: drop this many in a thousand of the stream packets sent.
  void test_loss(int permille) noexcept { loss_.store(permille); }

  struct stream_state;  // udp_hub.cpp

 private:
  udp_hub() = default;
  void read_loop();
  void timer_loop();

  std::unique_ptr<net::udp_socket> sock_;
  std::uint16_t port_ = 0;
  std::mutex m_;
  datagram_fn dht_, probe_;                                    // under m_
  std::map<std::uint32_t, std::weak_ptr<stream_state>> streams_;  // under m_
  std::atomic<bool> stop_{false};
  std::atomic<int> loss_{0};
  std::thread reader_, timer_;
};

}  // namespace mv::sharepoint
