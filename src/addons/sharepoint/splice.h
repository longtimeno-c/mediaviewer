// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Relays (issue #472, options 9 and 10; docs/design/28 "Relays"): the splice
// that copies bytes between two links, and the rendezvous a relay server of
// the person's own holds (tools/sharepoint-relay).
//
// A relay only ever moves ciphertext. The two MediaViewers run their whole
// handshake (CPace, then Noise) and their session end to end through it, so
// it holds no key of theirs and cannot read or change what they say; at most
// it can drop the link, which either end then sees as a dropped link.
//
// The rendezvous: each side opens a TCP connection to the relay and says
// kHelloBytes: "MVRL", the version, three zero bytes and a random 16-byte
// token. The receiver's token is in the code it shows (invite.h), sealed
// under the code's secret, so only someone with the code learns it. Two
// connections with the same token are paired: each gets kPaired, and from
// then on every byte one sends reaches the other. A third with that token is
// closed; so is one that says anything else, or nothing within kHelloMs, or
// sends before it is paired. An unpaired one waits at most `wait_ms`.
//
// The token is not the code's secret and says nothing about it: the relay
// learns that two connections wanted each other, their addresses, the time
// and the size. Nothing here logs an address or a token (rule 6): a server
// counts pairs, never names them. Portable (D9): links only.
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
#include <thread>
#include <vector>

#include "addons/sharepoint/link.h"
#include "addons/sharepoint/net.h"

namespace mv::sharepoint::relay {

inline constexpr std::size_t kTokenBytes = 16;
using token = std::array<std::uint8_t, kTokenBytes>;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHelloBytes = 4 + 1 + 3 + kTokenBytes;
inline constexpr std::uint8_t kPaired = 'P';
// A connection to a relay server says its token within this.
inline constexpr int kHelloMs = 5000;
// The sender's wait to be paired: the receiver is already waiting there.
inline constexpr int kPairWaitMs = 10000;
// tools/sharepoint-relay's port unless told another.
inline constexpr std::uint16_t kDefaultPort = 47614;

[[nodiscard]] token random_token();
[[nodiscard]] std::array<std::uint8_t, kHelloBytes> hello_bytes(const token& t);
// The token, if `bytes` is a whole hello of this version.
[[nodiscard]] std::optional<token> parse_hello(std::span<const std::uint8_t> bytes) noexcept;

// What a splice allows, each way.
struct limits {
  std::uint64_t max_bytes = 64ull << 30;  // per splice; past it the splice ends
  std::uint64_t rate = 0;                 // bytes a second; 0: no cap
  int idle_ms = 120000;                   // nothing either way for this long ends it
  // The tests: every byte as it passes, each way (two threads call it).
  std::function<void(bool a_to_b, std::span<const std::uint8_t>)> tap;
};
enum class ended : std::uint8_t { closed, capped, idle, stopped, failed };
struct splice_result {
  std::uint64_t a_to_b = 0, b_to_a = 0;
  ended why = ended::closed;
};
// Copies both ways (this thread and one more) until a side closes or fails,
// a cap or the idle limit is reached, or `stopped()`; then both links are
// shut down. Holds no lock across I/O.
[[nodiscard]] splice_result splice(link& a, link& b, const limits& lim, const std::function<bool()>& stopped);

// A relay server's client: says `t` on `l` and waits up to `wait_ms` (or
// `stopped()`) to be paired. The link then carries the other side's bytes.
// status::timeout unpaired, ::io when the server closed it (a token in use).
[[nodiscard]] result<std::unique_ptr<link>> join(std::unique_ptr<link> l, const token& t, int wait_ms,
                                                 const std::function<bool()>& stopped);
// join's second half, after the hello was written on `l`.
[[nodiscard]] result<std::unique_ptr<link>> await_pair(std::unique_ptr<link> l, int wait_ms,
                                                       const std::function<bool()>& stopped);

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
// status::io when it closed first (a sender that lost its race).
[[nodiscard]] result<std::unique_ptr<link>> first_bytes(std::unique_ptr<link> l, int wait_ms,
                                                        const std::function<bool()>& stopped, const char* through);

// The rendezvous, as tools/sharepoint-relay runs it (and the tests, in
// process). Its own threads: an accept loop, one per connection waiting to
// be paired (at most max_waiting) and one pair of copiers per splice (at most
// max_pairs). Destroying it closes every connection and joins every thread.
class server {
 public:
  struct options {
    std::uint16_t port = kDefaultPort;  // 0: the OS picks (the tests)
    bool loopback_only = false;
    int max_pairs = 8;
    int max_waiting = 64;
    int wait_ms = 10 * 60 * 1000;  // a code lives 10 minutes
    limits lim;
  };
  struct counts {
    std::uint64_t paired = 0, refused = 0, waiting = 0, active = 0;
    std::uint64_t bytes = 0;  // spliced, both ways, every splice that ended
  };
  [[nodiscard]] static result<std::unique_ptr<server>> start(options o);
  ~server();
  server(const server&) = delete;
  server& operator=(const server&) = delete;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] counts snapshot() const;

 private:
  struct waiter;
  explicit server(options o) : opts_(std::move(o)) {}
  void accept_loop();
  void handle(std::shared_ptr<link> l);
  void reap_locked();

  options opts_;
  std::unique_ptr<net::listener> listener_;
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<token, std::shared_ptr<waiter>> waiting_;  // under m_
  struct worker {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<link> l;
  };
  std::vector<worker> workers_;  // under m_
  counts counts_;                // under m_
};

}  // namespace mv::sharepoint::relay
