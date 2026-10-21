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
// Extra sockets (#472, the birthday punch): open_extras() adds sockets the
// same reader polls with the main one; a probe says which socket it came in
// on, and a stream can run on any of them. Only while a hard NAT is being
// punched; close_extras() lets them go.
// Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "addons/driftwire/link.h"
#include "addons/driftwire/net.h"

namespace mv::driftwire {

inline constexpr std::uint8_t kProbe = 0xA6;
inline constexpr std::uint8_t kStream = 0xB7;
inline constexpr std::size_t kStreamPayload = 1200;
// A stream's receive buffer, at most: the bytes waiting to be read and the
// packets held out of order together (#499: the peer is not yet
// authenticated while the handshake runs over it, so it cannot make this
// side hold more).
inline constexpr std::size_t kStreamMaxRx = 16u * 1024 * 1024;

class udp_hub : public std::enable_shared_from_this<udp_hub> {
 public:
  using datagram_fn = std::function<void(std::span<const std::uint8_t>, const net::endpoint& from)>;
  // A probe and the socket it came in on (0: the main one).
  using probe_fn = std::function<void(std::span<const std::uint8_t>, const net::endpoint& from, std::uint32_t sock)>;
  // The tests: every socket the hub opens goes through this (a stand-in NAT).
  using socket_wrap = std::function<std::unique_ptr<net::udp_socket>(std::unique_ptr<net::udp_socket>)>;
  // `port` 0: the OS picks.
  [[nodiscard]] static result<std::shared_ptr<udp_hub>> open(std::uint16_t port, bool loopback_only = false,
                                                             socket_wrap wrap = {});
  ~udp_hub();
  udp_hub(const udp_hub&) = delete;
  udp_hub& operator=(const udp_hub&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] expected send(std::span<const std::uint8_t> datagram, const net::endpoint& to);
  // From socket `sock` (0: the main one); status::not_found when it is closed.
  [[nodiscard]] expected send(std::span<const std::uint8_t> datagram, const net::endpoint& to, std::uint32_t sock);
  // Up to `n` more sockets on ports the OS picks, read by the same thread;
  // their ids (fewer when the OS runs out).
  [[nodiscard]] std::vector<std::uint32_t> open_extras(int n);
  // Closes these extra sockets (a stream on one fails).
  void close_extras(std::span<const std::uint32_t> ids);
  [[nodiscard]] std::size_t extra_count();
  // Handlers, on the hub's reader thread (brief; never blocking, never
  // setting a handler). Setting one waits for a call of the old one that is
  // running: after on_dht(nullptr) returns, the old handler is not called.
  void on_dht(datagram_fn fn);
  void on_probe(probe_fn fn);
  // A reliable stream to `peer`, both sides with the same `id` (the sender
  // picks it during punching). Packets for it from anyone else are dropped.
  // `learn`: the peer is whoever sends the first packet of this id (the
  // receiver of a punched path: the sender chose which of the paths found).
  // `sock`: the socket it runs on (an extra, after a birthday punch).
  [[nodiscard]] std::unique_ptr<link> stream(const net::endpoint& peer, std::uint32_t id, bool learn = false,
                                             std::uint32_t sock = 0);
  // Stops the threads; every stream fails. Idempotent.
  void stop() noexcept;
  // The tests: drop this many in a thousand of the stream packets sent.
  void test_loss(int permille) noexcept { loss_.store(permille); }
  // What a stream's sender did so far (#529: the stream bench); nullopt once
  // it is gone.
  struct stream_stats {
    std::uint64_t packets = 0;      // data packets sent, first copies
    std::uint64_t retransmits = 0;  // copies sent again (RACK, RTO, probe)
    std::uint64_t rtos = 0;         // retransmission timeouts
    std::uint64_t probes = 0;       // tail loss probes
    std::uint64_t episodes = 0;     // loss episodes (cwnd cut once each)
    double srtt_ms = 0, cwnd = 0, ssthresh = 0, max_cwnd = 0;
  };
  [[nodiscard]] std::optional<stream_stats> stats(std::uint32_t id);

  struct stream_state;  // udp_hub.cpp

 private:
  udp_hub() = default;
  void read_loop();
  void timer_loop();
  void take(std::span<const std::uint8_t> d, const net::endpoint& from, std::uint32_t sock_id,
            const std::shared_ptr<net::udp_socket>& sock);
  expected send_on(net::udp_socket& sock, std::span<const std::uint8_t> datagram, const net::endpoint& to);

  std::shared_ptr<net::udp_socket> sock_;  // the main socket, id 0
  std::uint16_t port_ = 0;
  bool loopback_ = false;
  socket_wrap wrap_;
  std::mutex m_;
  std::mutex calling_m_;                                       // held while a handler runs
  datagram_fn dht_;                                            // under m_
  probe_fn probe_;                                             // under m_
  std::map<std::uint32_t, std::shared_ptr<net::udp_socket>> extras_;  // under m_
  std::uint32_t next_extra_ = 1;                               // under m_
  std::atomic<bool> has_extras_{false};
  std::map<std::uint32_t, std::weak_ptr<stream_state>> streams_;  // under m_
  std::atomic<bool> stop_{false};
  std::atomic<int> loss_{0};
  std::thread reader_, timer_;
};

}  // namespace mv::driftwire
