// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Relays (issue #472, options 9 and 10; docs/design/28 "Relays"): the splice
// that copies bytes between two links, and the rendezvous a relay server of
// the person's own holds (tools/driftwire-relay).
//
// A relay only ever moves ciphertext. The two MediaViewers run their whole
// handshake (CPace, then Noise) and their session end to end through it, so
// it holds no key of theirs and cannot read or change what they say; at most
// it can drop the link, which either end then sees as a dropped link.
//
// The rendezvous (protocol 3, #500). The server has a key of its own (its
// person types it in "Relay (your own server)" with the address); only a
// holder of it can wait there, so the server relays for its person's codes
// and nobody else's. Every connection runs a handshake before it says
// anything that matters, so neither the token nor any proof of the key is
// ever on the wire in the clear, and nothing recorded can be said again:
//   - Both say "MVRL", the version, the role and two zero bytes.
//   - The waiting side (the receiver of a code) adds a random 16-byte
//     session id and its CPace message (cpace.h) on the server key; the
//     server answers with its own. A machine in the middle, or one that
//     answers for the server's name, gets one live guess at the key per
//     connection and nothing to test guesses against offline, however short
//     the key.
//   - The sending side adds the token's id (token_id: a keyed hash that says
//     nothing of the token). A server with no waiter under it closes it at
//     once (senders never wait).
//   - Then Noise_NNpsk0 (noise.h), the server first, keyed by the CPace key
//     (a waiter) or by a key from the token (a sender), all of the above in
//     its prologue. The server's message opens only under the same key; the
//     client's answer, made against the server's fresh ephemeral key, opens
//     only on this connection. A waiter's answer carries its token, sealed.
//   - The server's word, sealed: kRegistered to a waiter once it waits (a
//     receiver closes its old wait only then), and kPaired to both once
//     paired. From then on every byte one sends reaches the other, as it is.
// The token is in the code (invite.h), sealed under the code's secret; the
// key never is. A waiter with the token of one already waiting replaces it
// (a receiver re-registers every kReregisterMs, ahead of NATs that drop an
// idle TCP connection). Anything that is not a hello, a handshake that does
// not open, nothing within kHelloMs, or a byte before the pair, is closed. A
// waiter waits `wait_ms`.
//
// The token is not the code's secret and says nothing about it: the relay
// learns that two connections wanted each other, their addresses, the time
// and the size. Nothing here logs an address or a token (rule 6): a server
// counts pairs, never names them. Portable (D9): links only.
//
// Forwarding (the Tunnel, 2026-11-12; docs/plans/driftwire-menu-and-tunnel.md):
// a key holder may also ask the server to open a TCP connection to a host
// and port and copy bytes both ways, so a person's own web traffic leaves
// through their own server. The same handshake as a waiter's (CPace on the
// key, then Noise NNpsk0), role kForwarder; the client's answer carries the
// host and port, sealed, so a network on the path sees neither. The server
// says one sealed word (kForwarding, or why not), then the bytes go both
// ways as sealed frames (sealed_link), never as they are: a plain HTTP
// request would otherwise show its host to the filter it is meant to pass.
// Never for anyone without the key: not an open proxy. Ports 80 and 443
// unless the server's person says others, a cap on forwards at once, TCP
// only, and only to public addresses (a name that resolves to a private one
// is refused: the server is not a way into its own network).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/driftwire/link.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/noise.h"

namespace mv::driftwire::relay {

inline constexpr std::size_t kTokenBytes = 16;
using token = std::array<std::uint8_t, kTokenBytes>;
inline constexpr std::size_t kIdBytes = 16;
using token_id_bytes = std::array<std::uint8_t, kIdBytes>;
inline constexpr std::uint8_t kVersion = 3;  // #500: 2 said the token and a replayable proof in the clear
inline constexpr std::uint8_t kSender = 0, kWaiter = 1, kForwarder = 2;
// "MVRL", the version, the role, two zero bytes.
inline constexpr std::size_t kHeadBytes = 8;
inline constexpr std::size_t kSessionIdBytes = 16;  // a waiter's CPace sid
// The server's word, sealed: one byte and its tag.
inline constexpr std::uint8_t kRegistered = 'R', kPaired = 'P';
// The server's word to a forwarder: forwarding, or why not.
inline constexpr std::uint8_t kForwarding = 'F', kForwardOff = 'O', kForwardPort = 'X', kForwardBusy = 'B',
                              kForwardUnreachable = 'U', kForwardNotPublic = 'N';
inline constexpr std::size_t kWordBytes = 1 + noise::kTagBytes;
// A connection to a relay server finishes its handshake within this.
inline constexpr int kHelloMs = 5000;
// The sender's wait for kPaired: the receiver is already waiting there, so
// the answer is at once (or a close).
inline constexpr int kPairWaitMs = 5000;
// A waiting receiver registers again this often, under the same token.
inline constexpr int kReregisterMs = 60000;
// tools/driftwire-relay's port unless told another.
inline constexpr std::uint16_t kDefaultPort = 47614;
// A server key, as typed after '#', at least this long.
inline constexpr std::size_t kMinKeyChars = 16;
// A forward's host, as asked: a name or an address, at most this long.
inline constexpr std::size_t kMaxHostBytes = 253;
// The server's connect to a forward's host, every address of it in all.
inline constexpr int kForwardConnectMs = 8000;
// A forwarder's wait for the server's word: the handshake and that connect.
inline constexpr int kForwardWaitMs = kHelloMs + kForwardConnectMs;
// A sealed frame's plaintext, at most.
inline constexpr std::size_t kSealedChunk = 16 * 1024;

[[nodiscard]] token random_token();
// A fresh server key: 26 characters of Crockford base32 (130 bits).
[[nodiscard]] std::string random_key();
// What a sender names its token by: a keyed BLAKE2b of it.
[[nodiscard]] token_id_bytes token_id(const token& t);
// The head's role, if a head of this version.
[[nodiscard]] std::optional<std::uint8_t> parse_head(std::span<const std::uint8_t> head) noexcept;

// What a splice allows, each way.
struct limits {
  std::uint64_t max_bytes = 64ull << 30;  // per splice; past it the splice ends
  std::uint64_t rate = 0;                 // bytes a second; 0: no cap
  int idle_ms = 120000;                   // nothing either way for this long ends it
  // The tests: every byte as it passes, each way (two threads call it).
  std::function<void(bool a_to_b, std::span<const std::uint8_t>)> tap;
};
// A rate cap's bucket holds at most this long of bytes (an idle pair may
// burst only that much), and never less than one buffer.
inline constexpr double kBurstSeconds = 0.25;
enum class ended : std::uint8_t { closed, capped, idle, stopped, failed };
struct splice_result {
  std::uint64_t a_to_b = 0, b_to_a = 0;
  ended why = ended::closed;
};
// Copies both ways (this thread and one more) until a side closes or fails,
// a cap or the idle limit is reached, or `stopped()`; then both links are
// shut down. Holds no lock across I/O. Never throws (no thread: failed).
[[nodiscard]] splice_result splice(link& a, link& b, const limits& lim, const std::function<bool()>& stopped) noexcept;

// A relay server's sender: its handshake for `t` on `l`, then the wait for
// kPaired, within `wait_ms` in all. status::io when the server closed it (no
// waiter for the token).
[[nodiscard]] result<std::unique_ptr<link>> join(std::unique_ptr<link> l, const token& t, int wait_ms,
                                                 const std::function<bool()>& stopped);
// A waiter registered at a relay server: its link, and what opens the
// server's later word.
class waiting {
 public:
  explicit waiting(std::unique_ptr<link> l) : l_(std::move(l)) {}
  [[nodiscard]] link& l() noexcept { return *l_; }
  // The link, for the pair's bytes (after kPaired).
  [[nodiscard]] std::unique_ptr<link> release() noexcept { return std::move(l_); }
  void shutdown() noexcept {
    if (l_) l_->shutdown();
  }

 private:
  friend result<std::unique_ptr<waiting>> register_waiter(std::unique_ptr<link>, const token&, const std::string&,
                                                          int);
  friend expected wait_paired(waiting&, int, const std::function<bool()>&);
  std::unique_ptr<link> l_;
  noise::cipher_state from_server_;
  std::array<std::uint8_t, kWordBytes> word_{};  // a word read in part
  std::size_t got_ = 0;
};
// A waiter: the handshake with the server key `key`, the token sealed, and
// the server's kRegistered, within `wait_ms`. status::permission_denied when
// the server did not prove the key (the wrong key, or not that server).
[[nodiscard]] result<std::unique_ptr<waiting>> register_waiter(std::unique_ptr<link> l, const token& t,
                                                               const std::string& key, int wait_ms);
// Waits up to `wait_ms` for kPaired on a registered waiter: ok when paired,
// status::timeout (still waiting: the link stays as it is), else the server
// closed it (replaced, refused, or gone).
[[nodiscard]] expected wait_paired(waiting& w, int wait_ms, const std::function<bool()>& stopped);

// A link whose bytes go as sealed frames (a 2-byte length, then the AEAD of
// at most kSealedChunk bytes) under one key each way: a forward's stream
// between the client and the server. A frame that does not open closes it.
// One reader and one writer at a time (on two threads, as a splice).
class sealed_link final : public link {
 public:
  sealed_link(std::unique_ptr<link> inner, std::unique_ptr<noise::cipher_state> send,
              std::unique_ptr<noise::cipher_state> recv, const char* through = "relay")
      : inner_(std::move(inner)), send_(std::move(send)), recv_(std::move(recv)), through_(through) {}
  [[nodiscard]] expected write_all(std::span<const std::uint8_t> bytes) override;
  [[nodiscard]] result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override;
  void shutdown() noexcept override { inner_->shutdown(); }
  [[nodiscard]] transport via() const noexcept override { return inner_->via(); }
  [[nodiscard]] const char* through() const noexcept override { return through_; }

 private:
  std::unique_ptr<link> inner_;
  std::unique_ptr<noise::cipher_state> send_, recv_;
  const char* through_;
  // The reader's own: a frame read in part, and plaintext not yet handed out.
  std::array<std::uint8_t, 2> len_{};
  std::size_t len_got_ = 0;
  std::vector<std::uint8_t> frame_;
  std::size_t frame_got_ = 0;
  std::vector<std::uint8_t> plain_;
  std::size_t plain_at_ = 0;
};

// What a forward came to, on the client's side.
enum class forward_answer : std::uint8_t {
  ok,
  key,          // the server did not prove the key (the wrong key, or not that server)
  off,          // the server does not forward
  port,         // not a port it forwards to
  busy,         // its forwards are all in use
  unreachable,  // nothing at that host answered it (or the name did not resolve)
  not_public,   // the host is on a private network (or the server itself)
  failed,       // the connection to the server failed or closed
};
[[nodiscard]] const char* forward_answer_name(forward_answer a) noexcept;
// A forwarder: the handshake with the server key `key`, the host and port
// sealed, and the server's word, within `wait_ms`. On ok, the stream to the
// host (a sealed_link). `said` says what came of it either way. `host`: a
// name or an address (no brackets), 1..kMaxHostBytes.
[[nodiscard]] result<std::unique_ptr<link>> forward(std::unique_ptr<link> l, const std::string& key,
                                                    const std::string& host, std::uint16_t port, int wait_ms,
                                                    forward_answer& said);
// A host a forward may name: 1..kMaxHostBytes of letters, digits, '.', '-',
// '_' and ':' (an IPv6 address). Pure.
[[nodiscard]] bool forward_host_ok(std::string_view host) noexcept;

// A link through a relay (through() says so, for the window), whose first
// bytes may already have been read: they come first.
class relayed_link final : public link {
 public:
  relayed_link(std::unique_ptr<link> inner, std::vector<std::uint8_t> first, const char* through)
      : inner_(std::move(inner)), first_(std::move(first)), through_(through) {}
  [[nodiscard]] expected write_all(std::span<const std::uint8_t> bytes) override { return inner_->write_all(bytes); }
  [[nodiscard]] result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override;
  void shutdown() noexcept override { inner_->shutdown(); }
  [[nodiscard]] transport via() const noexcept override { return inner_->via(); }
  [[nodiscard]] const char* through() const noexcept override { return through_; }
  [[nodiscard]] bool punched() const noexcept override { return inner_->punched(); }

 private:
  std::unique_ptr<link> inner_;
  std::vector<std::uint8_t> first_;  // the reader's own
  std::size_t at_ = 0;
  const char* through_;
};
// Waits up to `wait_ms` for the first bytes on `l` (a paired rendezvous: the
// sender's hello), then gives the link with them still to read. A receiver
// hands a link on only then: a sender that paired but lost its race to a
// direct place closes without a byte, and is not a sender at all.
// status::io when it closed first.
[[nodiscard]] result<std::unique_ptr<link>> first_bytes(std::unique_ptr<link> l, int wait_ms,
                                                        const std::function<bool()>& stopped, const char* through);

// The rendezvous, as tools/driftwire-relay runs it (and the tests, in
// process). Its own threads: an accept loop, one per connection running its
// handshake (at most kMaxHandshakes at once, kPerSource from one source), one
// per waiter (at most max_waiting, kPerSource from one source) which runs
// its splice once paired (at most max_pairs). Destroying it closes every
// connection and joins every thread.
class server {
 public:
  static constexpr int kMaxHandshakes = 16;
  static constexpr int kPerSource = 4;
  struct options {
    std::string key;  // required: what a waiter's CPace runs on (at least kMinKeyChars)
    std::uint16_t port = kDefaultPort;  // ignored with loopback_only: the OS picks
    bool loopback_only = false;
    int max_pairs = 8;
    int max_waiting = 64;
    int wait_ms = 10 * 60 * 1000;  // a code lives 10 minutes
    limits lim;
    // Forwarding (the Tunnel): the ports a key holder may reach (empty: no
    // forwarding at all), forwards at once, and each forward's limits (a
    // browser keeps a connection open a while between requests).
    std::vector<std::uint16_t> forward_ports{80, 443};
    int max_forwards = 64;
    limits forward_lim = [] {
      limits l;
      l.idle_ms = 10 * 60 * 1000;
      return l;
    }();
    // The tests: forward to loopback and private addresses too.
    bool forward_private = false;
    // How a forward reaches its host (net::resolve, then net::connect, when
    // empty; the tests).
    std::function<result<std::unique_ptr<link>>(const std::string& host, std::uint16_t port, int timeout_ms,
                                                forward_answer& why)>
        forward_connect;
  };
  struct counts {
    std::uint64_t paired = 0, refused = 0, waiting = 0, active = 0;
    std::uint64_t bytes = 0;  // spliced, both ways, every splice that ended
    std::uint64_t forwarded = 0, forwarding = 0;  // forwards made, and open now
    std::uint64_t forward_refused = 0;            // key holders' forwards said no to
  };
  [[nodiscard]] static result<std::unique_ptr<server>> start(options o);
  ~server();
  server(const server&) = delete;
  server& operator=(const server&) = delete;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] counts snapshot() const;
  // Where a connection counts toward kPerSource: its IPv6 /64, or its IPv4 /24.
  [[nodiscard]] static net::address source_of(const net::address& a) noexcept;

 private:
  struct waiter;
  explicit server(options o) : opts_(std::move(o)) {}
  void accept_loop();
  void handle(std::shared_ptr<link> l, const net::address& from);
  void serve_waiter(std::shared_ptr<link> l, const token& t, std::unique_ptr<noise::cipher_state> to_waiter,
                    const net::address& from);
  void serve_forward(std::shared_ptr<link> l, std::string host, std::uint16_t port,
                     std::unique_ptr<noise::cipher_state> to_client, std::unique_ptr<noise::cipher_state> from_client);
  void reap_locked();

  options opts_;
  std::unique_ptr<net::listener> listener_;
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<token_id_bytes, std::shared_ptr<waiter>> waiting_;  // under m_, by the token's id
  // Keyed by source_of(): an IPv6 /64 or an IPv4 /24 (#500: one host has a
  // whole /64 of addresses).
  std::map<net::address, int> handshaking_from_;  // under m_
  std::map<net::address, int> waiting_from_;      // under m_
  int handshaking_ = 0;                               // under m_
  struct worker {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<link> l;
  };
  std::vector<worker> workers_;  // under m_
  counts counts_;                // under m_
};

}  // namespace mv::driftwire::relay
