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
//      paired devices hold a read grant on the named folders; a device whose
//      role is "change" may also add, move and delete files in them
//      (serve_wire.h "changing", serve_backup.h for backup folders); a device
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
#include "wire/serve_convert.h"
#include "wire/serve_wire.h"
#include "wire/splice.h"
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
// Phone backup (1.1): backup folders and the backup flag on a phone's grant, so the cards
// show their controls only to a service that has them.
inline constexpr const char* kFeatureBackup = "backup";
// The self-hosted computer's users (share_users.h): serve_admin users, user_set, user_remove.
inline constexpr const char* kFeatureUsers = "users";
// The service keeps where paired devices find it (serve_admin set_addresses) and tells them in
// their caps (kCapsAddresses).
inline constexpr const char* kFeatureAddresses = "addresses";
// The owner can require phones to lock the app (serve_admin set_phone_lock; kCapsLock).
inline constexpr const char* kFeaturePhoneLock = "phone_lock";
// The relay server's key, in the service's secret store (never serve.json).
inline constexpr const char* kRelayKeyName = "relay_key";
// The VPN (docs/design/28 "The NAS as a VPN host"): offered by `driftwire serve`
// (options::vpn), the key its own relay runs on, kept apart from the person's
// other relay's.
inline constexpr const char* kFeatureVpn = "vpn";
inline constexpr const char* kVpnKeyName = "vpn_relay_key";
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
  std::uint64_t udp = 0;               // sessions over a UDP stream (options::udp)
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
    // Sessions at once, in all and per paired device: a phone's copy opens several (sync's
    // lanes), and one phone must not take every slot from the others.
    int max_sessions = 32;
    int max_per_device = 8;
    int max_pending = 16;            // handshakes in progress; when full, the oldest hello still waiting goes
    int per_minute = 30;              // connections per address (a /64 for IPv6); a /48 gets four times this
    // Handshakes in progress per address (a /64 for IPv6), and per IPv6 /48: a phone's copy
    // opens its lanes together (src/mobile/sync.h lanes, 3 beside its link; 2 and 4 before).
    int per_source_pending = 4;
    int per_block_pending = 8;
    // Sessions over UDP as well (udp_hub's stream, on the TCP port's number; caps say so): on a
    // long path with some loss one stream keeps the line's speed, where TCP halves on each loss.
    // Same admission and handshake as TCP. Never for the LAN-only service. A port taken: TCP only.
    bool udp = true;
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
    // Whether a device may change files (role "change": phone backup's writes, and
    // kChangeOps's add, mkdir, move and delete) in the folders it sees. Empty: its grant
    // decides (docs/plans/mobile.md §10b.1, B2; self-host-users-smb.md §9): an unlimited
    // paired device may, a limited one only with `backup` on and only in its folders, the
    // owner's session never, and nobody in the Catalogue. The tests set it.
    std::function<bool(const std::array<std::uint8_t, 32>&)> may_write;
    // Tests: a volume's free bytes, instead of asking the OS.
    std::function<std::uint64_t(const std::string&)> free_bytes;
    // Tests: a move between two named folders always copies, checks and removes, as one
    // across volumes does (serve_wire.h "changing"), so that path runs on one disk.
    bool copy_moves = false;
    // Tests: called on the worker before each file a move copies (a revoke in the middle).
    std::function<void()> during_copy;
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
    // Self-hosting (docs/plans/self-hosted.md §6): whether a named folder's drive is there,
    // beyond the folder being a folder (an unmounted mount point, a volume whose letter now
    // names another drive). Asked wherever the folder's presence is (roots, a failed
    // request, before a write). Empty: the folder alone decides.
    std::function<bool(const root_config&)> root_present;
    // Self-hosting with combined drives (docs/plans/self-host-drives.md §4): the worst
    // set's health word ("degraded", "rebuilding", "offline", "failing"; "" when healthy
    // or none), sent to every session in serve_caps "storage" so a phone can say
    // "Your server's mirror lost a drive". Any thread; never blocks.
    std::function<std::string()> storage;
    // Clips the phone can't play (docs/plans/phone-clip-convert.md): the host's converter, or
    // none. With one, every session's caps list `convert`. set_converter changes it later.
    std::shared_ptr<convert::service> converter;
    // The NAS as a VPN host (docs/design/28): the owner may turn on a relay of
    // this service's own that forwards (the Tunnel/VPN exit, as `driftwire host
    // vpn on` does). Only `driftwire serve` offers it; a host runs its own relay.
    bool vpn = false;
    // The tests: that relay's options before it starts (loopback, a private site).
    std::function<void(relay::server::options&)> vpn_tweak;
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
  // The devices with a session up now, each once (docs/plans/self-hosted.md
  // "Tray": the host's menu names the phones connected). Their keys only; the
  // owner's own session (the desktop that claimed it) is not one of them.
  [[nodiscard]] std::vector<std::array<std::uint8_t, 32>> connected_devices() const;
  // "Tray" usage: per paired device (never the owner's own sessions), the file
  // bytes read and written by this server's sessions so far (ended ones kept),
  // whether it is connected now, since when and how. Counted with one relaxed
  // atomic add per chunk; folded here, when the host writes its state.
  enum class usage_via : std::uint8_t { home, internet, relay };  // its network; direct from away; the person's relay
  struct device_usage {
    std::array<std::uint8_t, 32> device{};
    std::uint64_t read = 0, written = 0;
    bool connected = false;
    std::int64_t since_unix = 0;  // the earliest session up now
    usage_via via = usage_via::internet;
  };
  [[nodiscard]] std::vector<device_usage> usage() const;
  [[nodiscard]] std::vector<root_config> roots() const;
  // Reads the paired list now and cuts sessions no longer on it (it is also
  // read on a timer and for every new connection).
  void recheck_devices() noexcept;
  // `driftwire host`: the named folders changed on disk (a `host share` from another
  // process, a drive's new letter). Taken as they are, not saved again; requests already
  // open keep the view they opened with. False for a config with no folders.
  bool replace_config(config next);
  // The converter turned on or off: sessions that start from now see the change in their
  // caps; one already listing `convert` is answered unsupported {"why":"off"} without one.
  void set_converter(std::shared_ptr<convert::service> c);
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
