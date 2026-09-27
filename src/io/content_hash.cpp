// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/content_hash.h"

#include <blake3.h>

namespace mv::io {

struct hasher::state {
  blake3_hasher h;
};

hasher::hasher() : state_(std::make_unique<state>()) { blake3_hasher_init(&state_->h); }
hasher::~hasher() = default;

void hasher::update(std::span<const std::uint8_t> bytes) noexcept {
  if (!bytes.empty()) blake3_hasher_update(&state_->h, bytes.data(), bytes.size());
}

content_hash hasher::finish() noexcept {
  content_hash out;
  blake3_hasher_finalize(&state_->h, out.bytes.data(), out.bytes.size());
  blake3_hasher_init(&state_->h);
  return out;
}

content_hash hash_bytes(std::span<const std::uint8_t> bytes) {
  hasher h;
  h.update(bytes);
  return h.finish();
}

std::string content_hash::hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

bool content_hash::from_hex(std::string_view hex, content_hash& out) noexcept {
  if (hex.size() != 64) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  content_hash parsed;
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    parsed.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  out = parsed;
  return true;
}

}  // namespace mv::io
