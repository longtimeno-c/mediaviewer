// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A relay through the person's own computer (issue #472, option 9;
// docs/design/28 "Relays"). A, sending to B, cannot reach the places in B's
// code; R is another computer of the same person, remembered on A (and A on
// R) and marked on both as "my computer", with "Use my other computers to
// relay" on. A meets R as it meets any remembered device (the DHT, the pair's
// secret, R's key checked), then, over that sealed channel:
//
//   A -> R  msg::relay {"places":[...]}: B's places, as B's code holds them
//   R       connects onward to those, and only those, and only to public
//           addresses (never into R's own network)
//   R -> A  msg::relay_answer {"ok":true}, or {"ok":false,"why":...}
//   then    msg::relay_data either way: the bytes of A's TCP-like link to B
//
// A then runs the internet handshake with B (CPace on B's code, then Noise
// XX) through it, end to end. R sees that handshake and the session only as
// ciphertext inside its own channel with A; it holds no key of theirs, and B
// sees R's address as the sender's. One relay at a time on R (it is R's one
// transfer), with splice.h's caps; nothing is logged about either peer.
// Portable (D9).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/channel.h"
#include "addons/driftwire/internet.h"
#include "addons/driftwire/invite.h"
#include "addons/driftwire/race.h"
#include "addons/driftwire/splice.h"

namespace mv::driftwire::relay {

// A device key's short name for the window's controls: 16 hex digits of a
// hash of it (never the key itself).
[[nodiscard]] std::string fingerprint(const std::array<std::uint8_t, 32>& key);

// relay_data's bytes, at most, per message.
inline constexpr std::size_t kDataChunk = 64 * 1024;
// A waits this long for R's answer: R races B's places first.
inline constexpr int kAnswerMs = kReachConnectMs + kConnectMs;
// Own-device relays R allows by default: a cap on each way, and none on the rate.
inline constexpr std::uint64_t kOwnRelayMaxBytes = 256ull << 30;

enum class answer : std::uint8_t {
  ok,
  off,          // R does not relay (its toggle is off, or A is not marked as the person's own there)
  unreachable,  // nothing at B's places answered R either
  refused,      // no place R may connect to (none public), or a request it could not read
};
[[nodiscard]] const char* answer_name(answer a) noexcept;
[[nodiscard]] std::string request_json(const std::vector<net::endpoint>& to);
// At most invite::kMaxPlaces places, each checked (transfer.h's parse_places).
[[nodiscard]] std::vector<net::endpoint> parse_request(std::string_view json);
[[nodiscard]] std::string answer_json(answer a);
[[nodiscard]] std::optional<answer> parse_answer(std::string_view json);
// R connects onward only to a public address (net::is_public) with a port,
// and never to one of its own (`own`: net::local_addresses()), nor into an
// IPv6 /64 it has an address in. `allow_loopback`: and loopback (the tests).
// (R's own public IPv4, reached through its router, is not known here; what
// its router forwards there is open to the whole internet anyway.)
[[nodiscard]] bool may_reach(const net::endpoint& to, bool allow_loopback,
                             std::span<const net::address> own = {}) noexcept;

// ---- a relay server (option 10), the sender's side ----------------------------------
// A code's relay place joins its round this much after the direct places,
// so a direct path that works wins and the relay carries nothing.
inline constexpr int kRelayDelayMs = 1500;
// Adds the code's relay place to `race`: a TCP connection to the server,
// the token, and the wait to be paired (kPairWaitMs: the receiver waits
// there already). The link it gives says through() "relay".
void add_relay(link_race& race, const invite::relay_place& r, const connect_fn& connect,
               std::function<bool()> stopped);

// ---- own-device relay (option 9) ---------------------------------------------------------
// A link over a protocol 2 channel: its bytes travel as msg::relay_data (so
// the channel's own rejoin keeps them, as any message). msg::cancel or the
// channel ending closes it. `keep`: what the channel's own link needs alive
// (the DHT client and UDP socket it was met over).
class channel_link final : public link {
 public:
  channel_link(std::shared_ptr<channel> ch, std::shared_ptr<void> keep, const char* through)
      : keep_(std::move(keep)), ch_(std::move(ch)), through_(through) {}
  ~channel_link() override { ch_->shutdown(); }
  [[nodiscard]] expected write_all(std::span<const std::uint8_t> bytes) override;
  [[nodiscard]] result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override;
  void shutdown() noexcept override { ch_->shutdown(); }
  [[nodiscard]] transport via() const noexcept override { return transport::internet; }
  [[nodiscard]] const char* through() const noexcept override { return through_; }

 private:
  std::shared_ptr<void> keep_;   // declared first: it outlives the channel over it
  std::shared_ptr<channel> ch_;
  const char* through_;
  message m_;             // the reader's own
  std::size_t at_ = 0;    // into m_.body
  bool closed_ = false;   // the reader's own
};

// A: asks R, over `ch` (opened with R and its key checked), to connect
// onward to `to`. On ok, the link to B through R; else `said` says why.
[[nodiscard]] result<std::unique_ptr<link>> open_through(std::shared_ptr<channel> ch,
                                                         const std::vector<net::endpoint>& to,
                                                         std::shared_ptr<void> keep, answer& said);

// R: the rest of a relay A asked for (`request`, a msg::relay already read).
// `allowed`: R's toggle is on and A is marked as the person's own here.
struct serve_options {
  bool allow_loopback = false;
  // R's own addresses (net::local_addresses() when empty and not the tests').
  std::optional<std::vector<net::address>> own;
  connect_fn connect;  // net::connect when empty
  limits lim;
  std::function<bool()> stopped;
};
struct served {
  answer said = answer::refused;
  splice_result spliced;
};
[[nodiscard]] served serve(const std::shared_ptr<channel>& ch, const message& request, bool allowed,
                           const serve_options& o);

}  // namespace mv::driftwire::relay
