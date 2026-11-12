// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/pair_check.h"

#include <sodium.h>

#include <vector>

namespace mv::driftwire {

namespace {
constexpr std::string_view kDomain = "MediaViewer pairing check v1";
constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void put_field(crypto_generichash_state& st, std::span<const std::uint8_t> bytes) {
  const auto n = static_cast<std::uint32_t>(bytes.size());
  const std::uint8_t len[4] = {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                               static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 24)};
  crypto_generichash_update(&st, len, sizeof len);
  crypto_generichash_update(&st, bytes.data(), bytes.size());
}
}  // namespace

std::string pairing_check(std::string_view library_id, std::span<const std::uint8_t, 32> publisher_key,
                          std::span<const std::uint8_t, 32> service_identity, std::span<const std::uint8_t, 32> access_key) {
  if (sodium_init() < 0) return {};
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 32);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kDomain.data()), kDomain.size());
  put_field(st, std::span(reinterpret_cast<const std::uint8_t*>(library_id.data()), library_id.size()));
  put_field(st, publisher_key);
  put_field(st, service_identity);
  put_field(st, access_key);
  std::uint8_t h[32];
  crypto_generichash_final(&st, h, sizeof h);
  // 80 bits, five at a time, most significant first.
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (int i = 0; i < 10; ++i) {
    acc = (acc << 8) | h[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      if (!out.empty() && (out.size() + 1) % 5 == 0) out.push_back('-');
      out.push_back(kCrockford[(acc >> bits) & 31]);
    }
  }
  sodium_memzero(h, sizeof h);
  return out;
}

}  // namespace mv::driftwire
