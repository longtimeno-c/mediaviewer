// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/presence_keys.h"

#include <sodium.h>

#include <cstring>

namespace mv::driftwire::known {

namespace {

constexpr char kContext[crypto_kdf_CONTEXTBYTES + 1] = "MVSPpres";  // 8 bytes

}  // namespace

dht::keys presence_keys(const device& d, std::int64_t hour, const std::array<std::uint8_t, 32>& receiver) {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  // The direction's key: the pairing secret keyed over the receiver's
  // identity. Then four subkeys of it per hour: the item key's seed, the
  // box, the salt base and CPace's password (as dht::derive's four parts).
  std::uint8_t direction[crypto_kdf_KEYBYTES];
  crypto_generichash(direction, sizeof direction, receiver.data(), receiver.size(), d.secret.data(), d.secret.size());
  std::uint8_t part[4][32];
  const auto base = static_cast<std::uint64_t>(hour) * 4;
  for (std::uint64_t i = 0; i < 4; ++i) {
    crypto_kdf_derive_from_key(part[i], sizeof part[i], base + i, kContext, direction);
  }
  sodium_memzero(direction, sizeof direction);
  dht::keys k;
  crypto_sign_seed_keypair(k.public_key.data(), k.secret_key.data(), part[0]);
  std::memcpy(k.box.data(), part[1], 32);
  std::memcpy(k.salt_base.data(), part[2], 32);
  std::memcpy(k.password.data(), part[3], 32);
  sodium_memzero(part, sizeof part);
  return k;
}

}  // namespace mv::driftwire::known
