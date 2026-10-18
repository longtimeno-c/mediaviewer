// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/sha256.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace mv::ai {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

}  // namespace

sha256::sha256() noexcept
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void sha256::block(const std::uint8_t* p) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t{p[i * 4]} << 24) | (std::uint32_t{p[i * 4 + 1]} << 16) | (std::uint32_t{p[i * 4 + 2]} << 8) |
           std::uint32_t{p[i * 4 + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void sha256::update(std::span<const std::uint8_t> bytes) noexcept {
  total_ += bytes.size();
  std::size_t i = 0;
  if (used_ > 0) {
    while (i < bytes.size() && used_ < 64) buf_[used_++] = bytes[i++];
    if (used_ < 64) return;
    block(buf_.data());
    used_ = 0;
  }
  for (; i + 64 <= bytes.size(); i += 64) block(bytes.data() + i);
  while (i < bytes.size()) buf_[used_++] = bytes[i++];
}

std::array<std::uint8_t, 32> sha256::finish() noexcept {
  const std::uint64_t bits = total_ * 8;
  const std::uint8_t one = 0x80;
  update(std::span<const std::uint8_t>(&one, 1));
  const std::uint8_t zero = 0;
  while (used_ != 56) update(std::span<const std::uint8_t>(&zero, 1));
  std::uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
  update(len);
  std::array<std::uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(h_[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i]);
  }
  return out;
}

std::string sha256::finish_hex() {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto d = finish();
  std::string s(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    s[i * 2] = kHex[d[i] >> 4];
    s[i * 2 + 1] = kHex[d[i] & 15];
  }
  return s;
}

std::optional<std::string> sha256_file_hex(const std::string& path) {
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!in) return std::nullopt;
  sha256 h;
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got) h.update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(buf.data()), got));
  }
  if (in.bad()) return std::nullopt;
  return h.finish_hex();
}

}  // namespace mv::ai
