// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Tunnel (the owner's ask of 2026-11-12; docs/plans/driftwire-menu-and-tunnel.md,
// docs/design/28 "The Tunnel"): a local proxy that sends a person's own web
// traffic through their own Driftwire relay server, so a network that filters
// sites (and blocks the common VPNs) cannot stop it.
//
// SOCKS5 (CONNECT, no authentication) and HTTP (CONNECT, and a plain
// http:// request in absolute form) on 127.0.0.1, on a port kept from
// the last run when it is free, else one the OS picks. Each connection
// becomes one forward at the relay server (splice.h `forward`: CPace on the
// server key, then Noise; the host and port sealed), and the bytes are
// copied both ways. Browsers, the Claude app and command-line tools
// (HTTPS_PROXY, ALL_PROXY) use it; no VPN driver, no OS entitlement, nothing
// changed in the system's settings. Names are resolved at the relay (SOCKS5
// with a name, CONNECT), so the network's DNS sees none of them.
//
// What it does not do: UDP (QUIC falls back to TCP in every browser), and
// anything for a program that ignores proxy settings. Any program of this
// machine's users can connect to the loopback port while it runs (as with
// any local proxy); it can only reach what the relay forwards.
//
// Nothing is logged with a host, an address or a port (rule 6): counts only.
// Portable (D9): links and the net port only.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/driftwire/internet.h"
#include "addons/driftwire/link.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/splice.h"
#include "addons/driftwire/tls.h"

namespace mv::driftwire::tunnel {

// Streams through the tunnel at once (each its own forward at the server).
inline constexpr int kMaxStreams = 128;
// Forwards being opened at once: the relay server takes 4 handshakes at once
// from one source (splice.h server::kPerSource), and a page opens many
// connections together; the rest wait their turn.
inline constexpr int kOpensAtOnce = 3;
// A local client says its request within this.
inline constexpr int kRequestMs = 10000;
// An HTTP request head, at most.
inline constexpr std::size_t kMaxHeadBytes = 16 * 1024;

// ---- parsing (pure; tested) ---------------------------------------------------------------

// "host:port", "[v6]:port"; `default_port` when there is none (0: required).
struct target {
  std::string host;  // without brackets
  std::uint16_t port = 0;
};
[[nodiscard]] std::optional<target> parse_authority(std::string_view text, std::uint16_t default_port);

// An HTTP proxy request's head (through the blank line).
struct http_ask {
  bool connect = false;  // CONNECT: answer 200, then the bytes as they are
  target to;
  // A plain request: the head to send on, in origin form, without the
  // proxy's own headers, and "Connection: close" (one request per stream:
  // the next may be for another host).
  std::string head;
};
[[nodiscard]] std::optional<http_ask> parse_http_head(std::string_view head);

// SOCKS5's request after the method choice: VER 5, CMD, RSV, ATYP, address,
// port. nullopt when it is not a whole request (`needed` says how many bytes
// it needs, when that is known); `reply` the SOCKS5 error to send when it is
// whole but not one this proxy does (a command other than CONNECT: 7; an
// address type it does not know: 8), else 0.
struct socks_ask {
  target to;
  std::uint8_t reply = 0;
};
[[nodiscard]] std::optional<socks_ask> parse_socks_request(std::span<const std::uint8_t> bytes, std::size_t& needed);

// The system proxy (owner, 2026-11-12; sysproxy.h): the computer's automatic
// proxy setting points at a PAC file this proxy serves on its own port.
// It sends everything but local names and private addresses to SOCKS5 here
// (names resolved at the relay), and goes DIRECT when the proxy does not
// answer, so a dead tunnel never leaves the machine without the network.
inline constexpr std::string_view kPacPath = "/driftwire-tunnel.pac";
[[nodiscard]] std::string pac_url(std::uint16_t port);
[[nodiscard]] std::string pac_script(std::uint16_t port);
// Whether a head (through its blank line) asks for the PAC file: "GET" of
// kPacPath in origin form (what an OS's proxy client sends; never a proxy
// request, which names a host).
[[nodiscard]] bool asks_for_pac(std::string_view head);

// SOCKS5's reply code for a forward's answer, and HTTP's status line.
[[nodiscard]] std::uint8_t socks_reply_for(relay::forward_answer a) noexcept;
[[nodiscard]] std::string http_reply_for(relay::forward_answer a);

// ---- reaching the relay: the proxy's, and the VPN's (slice 5) -------------------------------

// Where a stream's connection to the relay is kept while it opens, so its
// owner can end it from another thread (its own lock).
class remote_slot {
 public:
  // False (and `l` shut down) when the slot was ended first.
  bool set(std::shared_ptr<link> l) {
    std::lock_guard lock(m_);
    if (ended_) {
      l->shutdown();
      return false;
    }
    l_ = std::move(l);
    return true;
  }
  void end() {
    std::lock_guard lock(m_);
    ended_ = true;
    if (l_) l_->shutdown();
  }

 private:
  std::mutex m_;
  std::shared_ptr<link> l_;
  bool ended_ = false;
};

// One forward at a time per call: the relay server resolved once (again
// after a failed connect), kOpensAtOnce being opened at once, TLS and the
// knock first when asked (slice 4), then relay::forward. A TCP connection or
// a datagram flow (kind) either way.
class dialer {
 public:
  struct options {
    std::string relay;  // "<address or name>:<port>#<key>"
    bool tls = false;
    std::uint16_t tls_port = tls::kDefaultPort;
    connect_fn connect;  // net::connect when empty
    // With tls: the knock key already derived (tls::derive_knock_key of the
    // relay's key), so make() runs no Argon2id. Its 64 MiB is more than an
    // iOS packet tunnel extension may hold (docs/plans/phone-vpn.md §2.1), so
    // the app derives it and the extension passes it here. Empty: derived here.
    std::optional<tls::knock_key> knock;
  };
  // status::invalid_arg for a relay field that is not one; with tls and no
  // `knock`, Argon2id runs here.
  [[nodiscard]] static result<std::unique_ptr<dialer>> make(options o);
  // `remote` (optional): where the connection to the server is kept while
  // it opens, so stop() and an owner's destructor can end it.
  [[nodiscard]] result<std::unique_ptr<link>> open(const std::string& host, std::uint16_t port,
                                                   relay::forward_answer& said,
                                                   const std::shared_ptr<remote_slot>& remote = nullptr,
                                                   std::uint8_t kind = relay::kForwardTcp);
  void stop();  // every waiting open() returns cancelled
  [[nodiscard]] const std::string& relay_at() const noexcept { return server_.at; }

 private:
  explicit dialer(options o) : opts_(std::move(o)) {}
  options opts_;
  relay_server_field server_;
  std::string sni_;         // the relay's name for TLS ("" for an address)
  tls::knock_key knock_{};  // with opts_.tls
  std::mutex m_;
  std::condition_variable turn_cv_;
  int opening_ = 0;                        // under m_
  bool stopped_ = false;                   // under m_
  std::optional<net::endpoint> resolved_;  // under m_
};

// ---- the proxy ----------------------------------------------------------------------------

class proxy {
 public:
  struct options {
    // "<address or name>:<port>#<key>", the window's "Relay (your own server)".
    std::string relay;
    // The port to try first (the last run's, so a browser set up once keeps
    // working); 0, or taken: one the OS picks.
    std::uint16_t port = 0;
    // A TCP connection to the relay server (net::connect when empty).
    connect_fn connect;
    // Something changed that the window shows (streams opened or ended, a
    // problem). Any of the proxy's threads, never under its lock.
    std::function<void()> changed;
    // Slice 4 (tls.h): reach the relay on its TLS port, with Chrome's
    // ClientHello and the knock, instead of the plain one.
    bool tls = false;
    std::uint16_t tls_port = tls::kDefaultPort;
  };
  struct snapshot {
    std::uint16_t port = 0;
    std::uint32_t open = 0;        // streams now
    std::uint64_t streams = 0;     // streams made, in all
    std::uint64_t failed = 0;      // requests the relay said no to, or could not reach
    // The last stream's failure, when it was the relay's (not the site's):
    // the key, forwarding off, or the server unreachable. "" once one works.
    std::string problem;
  };
  // Listens; status::io when no loopback port could be had. The relay text
  // is checked (internet.h relay_server_of), not resolved: a bad one is
  // status::invalid_arg.
  [[nodiscard]] static result<std::unique_ptr<proxy>> start(options o);
  ~proxy();  // closes every stream and joins every thread
  proxy(const proxy&) = delete;
  proxy& operator=(const proxy&) = delete;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] snapshot state() const;

 private:
  explicit proxy(options o) : opts_(std::move(o)) {}
  void accept_loop();
  // `remote`: where the stream's connection to the server is kept, so the
  // destructor can end a handshake in progress.
  void serve(std::shared_ptr<link> local, const std::shared_ptr<remote_slot>& remote);
  // A forward to `to` at the relay server (waits for a turn among
  // kOpensAtOnce); the stream, or what the server said.
  [[nodiscard]] result<std::unique_ptr<link>> open(const target& to, relay::forward_answer& said,
                                                   const std::shared_ptr<remote_slot>& remote);
  void note(relay::forward_answer a, bool opened);

  options opts_;
  std::unique_ptr<dialer> dial_;
  std::unique_ptr<net::listener> listener_;
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex m_;
  struct worker {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<link> l;
    std::shared_ptr<remote_slot> remote;
  };
  std::vector<worker> workers_;  // under m_
  snapshot snap_;                // under m_ (port set at start)
};

// What the window and `/tunnel` say about the relay server in the field:
// its address without the key ("" when the field is empty or not one).
[[nodiscard]] std::string relay_shown(const std::string& relay_field);

}  // namespace mv::driftwire::tunnel
