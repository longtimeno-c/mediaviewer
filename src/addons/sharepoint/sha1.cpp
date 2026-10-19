// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/sha1.h"

#include <cstring>

namespace mv::sharepoint {

namespace {

std::uint32_t rol(std::uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

void block(std::uint32_t h[5], const std::uint8_t* p) noexcept {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = static_cast<std::uint32_t>(p[4 * i]) << 24 | static_cast<std::uint32_t>(p[4 * i + 1]) << 16 |
           static_cast<std::uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
  }
  for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::size_t i = 0;
  for (; i + 64 <= data.size(); i += 64) block(h, data.data() + i);
  std::uint8_t tail[128] = {};
  const std::size_t rest = data.size() - i;
  if (rest) std::memcpy(tail, data.data() + i, rest);
  tail[rest] = 0x80;
  const std::size_t len = rest < 56 ? 64 : 128;
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
  for (int b = 0; b < 8; ++b) tail[len - 1 - b] = static_cast<std::uint8_t>(bits >> (8 * b));
  block(h, tail);
  if (len == 128) block(h, tail + 64);
  std::array<std::uint8_t, 20> out;
  for (int k = 0; k < 5; ++k) {
    out[4 * k] = static_cast<std::uint8_t>(h[k] >> 24);
    out[4 * k + 1] = static_cast<std::uint8_t>(h[k] >> 16);
    out[4 * k + 2] = static_cast<std::uint8_t>(h[k] >> 8);
    out[4 * k + 3] = static_cast<std::uint8_t>(h[k]);
  }
  return out;
}

}  // namespace mv::sharepoint
