// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire serve`: the small always-on service a NAS runs so a paired phone
// can list and read its library over one forwarded port (docs/design/28 "The
// NAS service"). It moves bytes and nothing else: no search, no decoding.
//
// It listens on a fixed port on the open internet, so the order of what it
// does for a connection is the design:
//   1. a limit per address, on handshakes in progress from one address, and
//      on handshakes in all; past any the connection is simply closed;
//   2. read_library_hello: a fixed-size prefix and one fixed-size message;
//   3. that message must open under the library access key (one AEAD: no
//      X25519, no ML-KEM, no reply, for anyone without the key), and must not
//      be a replay of one seen since this service started (after a restart a
//      recording of a real hello costs one ML-KEM and gets one reply, and can
//      go no further: message 3 needs the device's key);
//   4. only then does it do any key work or say anything;
//   5. the device key from the finished handshake must be a paired one:
//      paired devices hold a read-only grant on the named folders; a device
//      that is not paired, or was revoked, is told so, sealed, and refused.
//
// A running session is cut within kDevicesRecheckMs of its device being
// revoked. The paired devices and this service's identity and key live in a
// secrets::file_store, owner-only (secrets.h), kept across restarts.
//
// No address, path or file name is ever logged or counted (rule 6); the
// counters here are totals. Never contacts the DHT, a router or a beacon.
// Blocking I/O on its own threads; it has no UI to protect. Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::serve {

// In the store: the 32-byte library access key, and the public key of the
// desktop that claimed this service. The identity and the paired devices are
// known.h's (kIdentityName, kDevicesName).
inline constexpr const char* kAccessKeyName = "library_access_key";
// In the store: the pairing codes `driftwire serve pair-code` made and not yet used (a hash and an
// expiry each). The desktop's codes (the owner's pair_code) are held in memory instead.
inline constexpr const char* kPairCodesName = "library_pair_codes";
inline constexpr std::size_t kPairCodesKept = 16;
inline constexpr std::int64_t kDefaultPairCodeSeconds = 600;
inline constexpr const char* kOwnerName = "owner";

// §7.2 claiming. The desktop writes 64 hex digits to this file under a named
// folder, over the share, then opens a library channel keyed from them.
inline constexpr const char* kClaimTokenFile = ".mediaviewer/claim.token";
inline constexpr const char* kClaimProofFile = ".mediaviewer/claim.proof";
inline constexpr const char* kCatalogueFolder = ".mediaviewer";
inline constexpr const char* kCatalogueLabel = "Catalogue";
[[nodiscard]] std::array<std::uint8_t, 32> claim_key(std::string_view token_hex);

// What an owner's session can use beyond the operations (serve_caps "features").
inline constexpr const char* kFeaturePassphrase = "passphrase";
// The service can wait at the person's own relay (docs/plans/nas-relay.md).
inline constexpr const char* kFeatureRelay = "relay";
// The relay server's key, in the service's secret store (never serve.json).
inline constexpr const char* kRelayKeyName = "relay_key";
// Waiters at the relay, one per paired device: the relay's own per-source cap.
inline constexpr int kMaxRelayWaiters = 4;

// How often the paired list is read again, for revocations.
inline constexpr int kDevicesRecheckMs = 2000;

struct counters {
  std::uint64_t accepted = 0;
  std::uint64_t over_limit = 0;     // dropped by the per-address or handshake limit
  std::uint64_t bad_hello = 0;      // not a library hello
  std::uint64_t no_key = 0;         // did not open under the access key
  std::uint64_t replayed = 0;       // opened, but a first message seen before
  std::uint64_t not_paired = 0;     // held the key, device not paired or revoked
  std::uint64_t paired = 0;         // devices added by a pairing code
  std::uint64_t bad_codes = 0;      // pairing codes that were wrong, spent or expired
  std::uint64_t sessions = 0;       // admitted
  std::uint64_t evicted = 0;        // a waiting hello closed to make room for a newer connection
  std::uint64_t wrong_passphrase = 0;  // owner or claim answers that did not verify
  std::uint64_t unlocks = 0;           // owner sessions unlocked by the passphrase
  std::uint64_t expired = 0;           // limited devices refused or cut because their time was up
  std::uint64_t relayed = 0;           // connections that came through the relay
  std::uint64_t punched = 0;           // direct paths a relayed phone moved to (nas-relay.md §9)
};

// The fuzz harness: one session's handling of what an admitted client sends,
// with no network and nothing sent. `input` is repeated: one type byte (msg),
// a little-endian u16 length, the body.
void drive_requests(const config& cfg, std::uint32_t ops, std::span<const std::uint8_t> input);

class server {
 public:
  struct options {
    config cfg;
    secrets::store* store = nullptr;  // borrowed; outlives the server
    bool loopback_only = false;       // the tests and the test build
    int max_sessions = 8;
    int max_pending = 16;             // handshakes in progress; when full, the oldest hello still waiting goes
    int per_minute = 30;              // connections per address (a /64 for IPv6); a /48 gets four times this
    int per_source_pending = 2;       // handshakes in progress per address (a /64 for IPv6)
    int per_block_pending = 4;        // and per IPv6 /48
    // The tests: a listener of their own instead of the configured port.
    std::function<std::unique_ptr<net::listener>()> make_listener;
    // Who may do what (docs/design/28 "Claiming and pairing").
    bool claimable = false;   // an owner may be taken on (the service, not the desktop's nearby one)
    bool lan_only = false;    // anyone not on own_network is closed at once (the nearby one)
    // Whether an address is on this service's own network. Empty: net::on_link.
    std::function<bool(const net::address&)> own_network;
    // A change of the named folders, written before it takes effect.
    std::function<bool(const config&)> save_config;
    // Taken around every change of the paired list ("": none), against `serve add`.
    std::string devices_lock;
    // The owner's passphrase (owner_pass.h). Unix seconds now: the tests move
    // it. Empty: the system clock.
    std::function<std::int64_t()> now_unix;
    // How long an unlock and a challenge have lasted (a steady clock, so a
    // clock set back never stretches them). Empty: steady_clock.
    std::function<std::chrono::steady_clock::time_point()> now_steady;
    // Answers to a passphrase challenge, per counter, at least this far apart:
    // a guesser waits, and the service only ever verifies a signature.
    int min_answer_gap_ms = 1000;
    // A connection to the relay server ("host", port). Empty: net::resolve
    // and net::connect. The tests: their loopback relay.
    std::function<result<std::unique_ptr<link>>(const std::string& host, std::uint16_t port, int timeout_ms)> relay_connect;
    // The tests: runs before a direct path's candidates are gathered (a slow relay).
    std::function<void()> before_gather;
  };

  // Listens and returns. status::not_found when the store has no usable
  // access key or identity (set_up it first); status::io when the port
  // cannot be bound.
  [[nodiscard]] static result<std::unique_ptr<server>> start(const options& o);
  ~server();
  server(const server&) = delete;
  server& operator=(const server&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] counters stats() const noexcept;
  [[nodiscard]] std::vector<root_config> roots() const;
  // Reads the paired list now and cuts sessions no longer on it (it is also
  // read on a timer and for every new connection).
  void recheck_devices() noexcept;
  void stop() noexcept;

 private:
  server() = default;
  struct state;
  std::unique_ptr<state> st_;
};

// Makes a pairing code that works once, for `seconds`, and keeps only its hash
// (the oldest goes past kPairCodesKept). Empty when it could not be kept.
[[nodiscard]] std::string issue_pair_code(secrets::store& store, std::int64_t seconds, std::int64_t now_unix);
// Spends `code` if it is good. False for a wrong, spent or expired one, or a
// store that cannot be rewritten (a code that could be spent twice is no use).
[[nodiscard]] bool redeem_pair_code(secrets::store& store, std::string_view code, std::int64_t now_unix);

// First-run setup: makes this service's identity and library access key in
// `store` if they are not there. `made` true when the key was new: the only
// time it is shown (`driftwire serve init`). false: could not be kept, or a
// value is there that cannot be read, which is never replaced.
[[nodiscard]] bool set_up(secrets::store& store, std::array<std::uint8_t, 32>& identity_public,
                          std::array<std::uint8_t, 32>& access_key, bool& made);

}  // namespace mv::driftwire::serve
