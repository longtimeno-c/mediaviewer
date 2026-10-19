// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/cpace.h"

#include <sodium.h>

#include <cstring>

namespace mv::sharepoint::cpace {

namespace {

constexpr char kDsi[] = "CPaceRistretto255";
constexpr char kDsiIsk[] = "CPaceRistretto255_ISK";
constexpr std::size_t kHashBlock = 128;  // SHA-512's input block (H.s_in_bytes)

// prepend_len: the length as LEB128, then the bytes.
void put_len(std::vector<std::uint8_t>& out, std::size_t n) {
  do {
    std::uint8_t b = static_cast<std::uint8_t>(n & 0x7f);
    n >>= 7;
    if (n) b |= 0x80;
    out.push_back(b);
  } while (n);
}

void lv(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> v) {
  put_len(out, v.size());
  out.insert(out.end(), v.begin(), v.end());
}

std::size_t len_of_len(std::size_t n) {
  std::size_t k = 1;
  while (n >>= 7) ++k;
  return k;
}

std::span<const std::uint8_t> bytes(const char* s) {
  return {reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)};
}

}  // namespace

std::vector<std::uint8_t> generator_string(std::span<const std::uint8_t> prs, std::span<const std::uint8_t> ci,
                                           std::span<const std::uint8_t> sid) {
  // §7.1: the zero padding makes DSI and PRS fill the hash's first block.
  const std::size_t dsi = std::strlen(kDsi);
  const std::size_t used = len_of_len(dsi) + dsi + len_of_len(prs.size()) + prs.size() + 1;
  const std::size_t zpad = used < kHashBlock ? kHashBlock - used : 0;
  std::vector<std::uint8_t> out;
  lv(out, bytes(kDsi));
  lv(out, prs);
  const std::vector<std::uint8_t> zeros(zpad, 0);
  lv(out, zeros);
  lv(out, ci);
  lv(out, sid);
  return out;
}

party::party(std::span<const std::uint8_t> prs, std::span<const std::uint8_t> ci, std::span<const std::uint8_t> sid,
             std::optional<std::array<std::uint8_t, 32>> fixed_scalar)
    : sid_(sid.begin(), sid.end()) {
  (void)sodium_init();
  std::vector<std::uint8_t> gs = generator_string(prs, ci, sid);
  std::uint8_t h[crypto_hash_sha512_BYTES];
  crypto_hash_sha512(h, gs.data(), gs.size());
  sodium_memzero(gs.data(), gs.size());
  std::uint8_t g[crypto_core_ristretto255_BYTES];
  const int mapped = crypto_core_ristretto255_from_hash(g, h);
  sodium_memzero(h, sizeof h);
  if (mapped != 0) return;
  if (fixed_scalar) {
    y_ = *fixed_scalar;
    sodium_memzero(fixed_scalar->data(), 32);
  } else {
    crypto_core_ristretto255_scalar_random(y_.data());
  }
  // The generator is secret (it is the code): wiped as soon as it is used.
  ok_ = crypto_scalarmult_ristretto255(y_pub_.data(), y_.data(), g) == 0;
  sodium_memzero(g, sizeof g);
}

party::~party() { sodium_memzero(y_.data(), y_.size()); }

result<std::array<std::uint8_t, kIskBytes>> party::finish(std::span<const std::uint8_t> peer_message, bool initiator,
                                                          std::span<const std::uint8_t> ad_initiator,
                                                          std::span<const std::uint8_t> ad_responder) {
  if (!ok_ || peer_message.size() != kPointBytes) {
    sodium_memzero(y_.data(), y_.size());
    return err(status::corrupt);
  }
  std::uint8_t k[crypto_scalarmult_ristretto255_BYTES];
  // scalar_mult_vfy: an encoding that is not a point, or a product that is
  // the identity, fails here (libsodium checks both).
  const int mult = crypto_scalarmult_ristretto255(k, y_.data(), peer_message.data());
  sodium_memzero(y_.data(), y_.size());
  ok_ = false;
  if (mult != 0) return err(status::corrupt);
  // ISK = H(lv_cat(DSI_ISK, sid, K) || transcript_ir(Ya, ADa, Yb, ADb)).
  const std::span<const std::uint8_t> ya = initiator ? std::span<const std::uint8_t>(y_pub_) : peer_message;
  const std::span<const std::uint8_t> yb = initiator ? peer_message : std::span<const std::uint8_t>(y_pub_);
  std::vector<std::uint8_t> in;
  lv(in, bytes(kDsiIsk));
  lv(in, sid_);
  lv(in, {k, sizeof k});
  sodium_memzero(k, sizeof k);
  lv(in, ya);
  lv(in, ad_initiator);
  lv(in, yb);
  lv(in, ad_responder);
  std::array<std::uint8_t, kIskBytes> isk;
  crypto_hash_sha512(isk.data(), in.data(), in.size());
  sodium_memzero(in.data(), in.size());
  return isk;
}

}  // namespace mv::sharepoint::cpace
