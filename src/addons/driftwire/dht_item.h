// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A four-word code and the DHT items it finds (issue #472 PR 3, opt-in): when
// the receiver has no public address of its own, both sides meet on the
// public BitTorrent DHT under targets only the code's holders can compute.
//
//   1. The code is four words of the BIP-39 list (44 bits). Argon2id
//      (libsodium's crypto_pwhash, moderate) turns it into an Ed25519 seed,
//      a key for the items' contents, a salt base and CPace's password
//      (cpace.h; the code is the password of the handshake too). Its salt
//      holds the hour (UTC) the code was made in (#499: a fixed salt let one
//      table of all 2^44 codes break every code; now it serves one hour), so
//      a sender tries the hour now and, early in an hour, the one before.
//   2. Each side publishes a BEP 44 mutable item under kTargets salts of its
//      role (receiver, sender): its candidate addresses, sealed with
//      XChaCha20-Poly1305, padded to 512 bytes, its timestamp inside. The
//      target is SHA-1(public key || salt): without the code, 20 random bytes
//      nobody can find, read or forge.
//   3. After the link is up each side overwrites its items with an empty one.
//
// Fake DHT nodes can delay a lookup, never read or forge an item: every item
// found is checked by its signature, its AEAD and its timestamp here.
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/net.h"
#include "core/result.h"

namespace mv::driftwire::dht {

inline constexpr std::size_t kWords = 4;
inline constexpr int kTargets = 4;             // salts per role, looked up apart
inline constexpr std::size_t kPlainBytes = 512;  // every item's contents, padded
inline constexpr std::size_t kMaxCandidates = 16;
inline constexpr std::int64_t kItemMaxAgeMs = 10 * 60 * 1000;  // a code's life
inline constexpr std::int64_t kItemSkewMs = 60 * 1000;         // two clocks, at most this apart

using word_code = std::array<std::uint16_t, kWords>;
[[nodiscard]] word_code random_code();
[[nodiscard]] std::string words_text(const word_code& c);
// Whole words or their first four letters (unique in the list), any case,
// separated by spaces, dashes, dots or commas.
[[nodiscard]] std::optional<word_code> parse_words(std::string_view text);

enum class role : std::uint8_t { receiver = 0, sender = 1 };

struct keys {
  std::array<std::uint8_t, 32> public_key{};   // Ed25519
  std::array<std::uint8_t, 64> secret_key{};   // Ed25519 (seed and public key)
  std::array<std::uint8_t, 32> box{};          // XChaCha20-Poly1305
  std::array<std::uint8_t, 32> salt_base{};
  std::array<std::uint8_t, 32> password{};     // CPace's PRS
  ~keys();
};
// The hour (UTC, since the epoch) a code made at `unix_ms` belongs to.
[[nodiscard]] std::int64_t code_hour(std::int64_t unix_ms) noexcept;
// Argon2id over the code and its hour: the slow part (moderate: ~0.5-1 s,
// 256 MiB). `fast` (the tests only): the minimum cost. `hour` < 0: now's.
[[nodiscard]] result<keys> derive(const word_code& c, bool fast = false, std::int64_t hour = -1);

// BEP 44's salt for (role, i), at most 64 bytes, and its target.
[[nodiscard]] std::string salt_of(const keys& k, role r, int i);
[[nodiscard]] std::array<std::uint8_t, 20> target_of(const keys& k, std::string_view salt);

// What an item says: where its side can be reached (empty: "gone", the link
// is up), and when it was written.
struct contents {
  std::vector<net::endpoint> candidates;
  std::int64_t written_ms = 0;  // unix time
  bool gone = false;
  // #472: the DHT saw this side on more than one port (a NAT that maps each
  // destination apart): the other side birthday-punches it.
  bool hard = false;
  // #472: the TCP port this side's simultaneous open uses, at every
  // candidate's address (0: none).
  std::uint16_t tcp_port = 0;
};
// The item's v (bencoded string's bytes): nonce || sealed padded contents.
[[nodiscard]] std::string seal(const keys& k, std::string_view salt, const contents& c);
[[nodiscard]] std::optional<contents> open(const keys& k, std::string_view salt, std::string_view v,
                                           std::int64_t now_ms);
// The padded plaintext inside (open's second half; the fuzzer's way in).
[[nodiscard]] std::optional<contents> parse_plain(std::span<const std::uint8_t> plain, std::int64_t now_ms);

// BEP 44's signature over (salt, seq, v): "4:salt<n>:<salt>3:seqi<seq>e1:v<bencoded v>".
[[nodiscard]] std::string signed_bytes(std::string_view salt, std::int64_t seq, std::string_view bencoded_v);
[[nodiscard]] std::array<std::uint8_t, 64> sign(const keys& k, std::string_view salt, std::int64_t seq,
                                                std::string_view bencoded_v);
[[nodiscard]] bool verify(std::span<const std::uint8_t, 32> public_key, std::string_view salt, std::int64_t seq,
                          std::string_view bencoded_v, std::span<const std::uint8_t> signature);

}  // namespace mv::driftwire::dht
