// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The NAS owner's passphrase (docs/plans/nas-owner-and-guests.md §3): a
// second factor beside the owner's device key, for the claim and for the
// owner's changes that add access.
//
// The passphrase is stretched with Argon2id into the seed of an Ed25519 key.
// The service keeps the salt, the cost and the PUBLIC key: neither the
// passphrase nor anything that can answer for it is ever on the NAS or on the
// wire. The owner signs a fresh nonce, bound to the session's handshake hash,
// the service's identity and the owner's device key, so a signature is no
// use on any other session.
//
// Wrong answers are counted per counter (a claim, the owner's session) in
// the store, so a restart does not reset them: five free, then a lock of 15
// minutes doubling to a day. `driftwire serve passphrase reset`, on the NAS,
// clears it all. Portable (D9); no I/O beyond the store.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::serve::owner {

inline constexpr const char* kPassphraseName = "owner_passphrase";
inline constexpr const char* kLockoutName = "owner_lockout";

// The cost every new verifier gets (call C11): crypto_pwhash's MODERATE.
inline constexpr std::uint64_t kOps = 3;
inline constexpr std::uint64_t kMem = 256ull * 1024 * 1024;
// What a stored verifier may ask of a client, at most and at least, so a
// tampered store cannot make the Mac spend a gigabyte or a minute, or
// accept a cost too cheap to mean anything.
inline constexpr std::uint64_t kMinOps = 2, kMaxOps = 10;
inline constexpr std::uint64_t kMinMem = 32ull * 1024 * 1024, kMaxMem = 1024ull * 1024 * 1024;
inline constexpr std::size_t kSaltBytes = 16;
inline constexpr std::size_t kBindingBytes = 64;  // noise::kHashBytes
inline constexpr std::size_t kMinChars = 10;      // code points, nothing else asked
inline constexpr std::size_t kMaxBytes = 1024;

inline constexpr std::int64_t kUnlockSeconds = 600;     // an unlocked session, this long (call C2)
inline constexpr std::int64_t kChallengeSeconds = 60;  // a nonce, this long
inline constexpr int kFreeMisses = 5;                  // call C10
inline constexpr std::int64_t kFirstLockSeconds = 15 * 60;
inline constexpr std::int64_t kMaxLockSeconds = 24 * 60 * 60;

using key32 = std::array<std::uint8_t, 32>;
using salt_t = std::array<std::uint8_t, kSaltBytes>;
using signature = std::array<std::uint8_t, 64>;

struct verifier {
  salt_t salt{};
  std::uint64_t ops = kOps;
  std::uint64_t mem = kMem;
  key32 pk{};
  std::int64_t set = 0;  // unix seconds
};
[[nodiscard]] std::string verifier_json(const verifier& v);
[[nodiscard]] std::optional<verifier> parse_verifier(std::string_view json);
// A cost a client will pay: within kMin/kMax.
[[nodiscard]] bool sane_cost(std::uint64_t ops, std::uint64_t mem) noexcept;

// The private half, held only by whoever typed the passphrase. Wiped when it goes.
struct signer {
  std::array<std::uint8_t, 64> sk{};
  key32 pk{};
  signer() = default;
  signer(const signer&) = default;
  signer& operator=(const signer&) = default;
  ~signer();
};

// At least kMinChars code points of valid UTF-8, at most kMaxBytes. The
// caller hands it over in NFC (the Mac normalises; a terminal types NFC).
[[nodiscard]] bool acceptable(std::string_view passphrase) noexcept;
// Argon2id over the passphrase, then the Ed25519 key from it. nullopt: the
// cost is not sane, or the memory could not be had.
[[nodiscard]] std::optional<signer> derive(std::string_view passphrase, const salt_t& salt, std::uint64_t ops,
                                           std::uint64_t mem);
// A new verifier with a fresh salt, and its signer when `out` is given.
[[nodiscard]] std::optional<verifier> make(std::string_view passphrase, std::int64_t now_unix,
                                           std::uint64_t ops = kOps, std::uint64_t mem = kMem, signer* out = nullptr);

// What is signed: a label, the session's handshake hash, the nonce, the
// service's identity and the owner's device key (both public).
[[nodiscard]] signature sign(const signer& s, std::span<const std::uint8_t, kBindingBytes> binding, const key32& nonce,
                             const key32& service, const key32& client);
[[nodiscard]] bool check(const verifier& v, const signature& sig, std::span<const std::uint8_t, kBindingBytes> binding,
                         const key32& nonce, const key32& service, const key32& client) noexcept;

// ---- in the store ---------------------------------------------------------------------------
enum class stored { none, set, unreadable };
// unreadable: there and not a verifier. Never read as none: that would let
// the next claim choose the passphrase.
[[nodiscard]] stored load(secrets::store& store, verifier& out);
[[nodiscard]] bool save(secrets::store& store, const verifier& v);

// ---- wrong answers --------------------------------------------------------------------------
struct counter {
  int misses = 0;
  std::int64_t until = 0;  // unix seconds; locked while now < until
  int level = 0;           // locks so far: the next lasts kFirstLockSeconds << level, at most a day
  [[nodiscard]] bool locked(std::int64_t now) const noexcept { return now < until; }
  void miss(std::int64_t now) noexcept;
  void success() noexcept { *this = counter{}; }
};
struct lockout {
  counter claim;  // a claimant past the write proof that is not the owner
  counter owner;  // the owner's device key (its sessions and its own re-claim)
};
[[nodiscard]] std::string lockout_json(const lockout& l);
[[nodiscard]] std::optional<lockout> parse_lockout(std::string_view json);
// Missing: no misses. There and unreadable: both locked for
// kFirstLockSeconds from `now` (a tampered or broken file never unlocks).
[[nodiscard]] lockout load_lockout(secrets::store& store, std::int64_t now);
[[nodiscard]] bool save_lockout(secrets::store& store, const lockout& l);

// `driftwire serve passphrase reset`: the verifier and both counters go.
[[nodiscard]] bool reset(secrets::store& store);

}  // namespace mv::driftwire::serve::owner
