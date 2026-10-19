// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/invite.h"

#include <sodium.h>

#include <cstdio>
#include <cstring>

namespace mv::sharepoint::invite {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford
constexpr char kKeyContext[] = "MediaViewer Sharepoint code places v2";
constexpr char kCheckContext[] = "MediaViewer Sharepoint code check v2";
constexpr std::uint8_t kKind4 = 4, kKind6 = 6;
// #472 relays: a relay server's address, then its 16-byte token.
constexpr std::uint8_t kRelay4 = 0x24, kRelay6 = 0x26;

int value_of(char c) noexcept {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c == 'O') c = '0';
  if (c == 'I' || c == 'L') c = '1';
  for (int i = 0; i < 32; ++i) {
    if (kAlphabet[i] == c) return i;
  }
  return -1;
}

// The places' bytes, encrypted (XOR) under a key from the secret: one code,
// one key, so a fixed nonce is enough.
void cover(const std::array<std::uint8_t, kSecretBytes>& secret, std::uint8_t* data, std::size_t n) {
  std::uint8_t key[crypto_stream_xchacha20_KEYBYTES];
  crypto_generichash(key, sizeof key, secret.data(), secret.size(), reinterpret_cast<const std::uint8_t*>(kKeyContext),
                     sizeof kKeyContext - 1);
  const std::uint8_t nonce[crypto_stream_xchacha20_NONCEBYTES] = {};
  crypto_stream_xchacha20_xor(data, data, n, nonce, key);
  sodium_memzero(key, sizeof key);
}

void check(const std::uint8_t* data, std::size_t n, std::uint8_t out[2]) {
  crypto_generichash(out, 2, data, n, reinterpret_cast<const std::uint8_t*>(kCheckContext), sizeof kCheckContext - 1);
}

bool put_place(std::vector<std::uint8_t>& out, const net::endpoint& e, bool relay = false) {
  const auto ip = net::parse_address(e.address);
  if (!ip || e.port == 0) return false;
  out.push_back(relay ? (ip->v6 ? kRelay6 : kRelay4) : ip->v6 ? kKind6 : kKind4);
  out.insert(out.end(), ip->bytes.begin(), ip->bytes.begin() + (ip->v6 ? 16 : 4));
  out.push_back(static_cast<std::uint8_t>(e.port >> 8));
  out.push_back(static_cast<std::uint8_t>(e.port));
  return true;
}

}  // namespace

code make(const std::vector<net::endpoint>& places, std::optional<relay_place> relay) {
  (void)sodium_init();
  code c;
  randombytes_buf(c.secret.data(), c.secret.size());
  if (relay) {
    std::vector<std::uint8_t> probe;
    if (put_place(probe, relay->at, true)) c.relay = std::move(relay);
  }
  const std::size_t room = kMaxPlaces - (c.relay ? 1 : 0);
  for (const auto& p : places) {
    if (c.places.size() >= room) break;
    std::vector<std::uint8_t> probe;
    if (put_place(probe, p)) c.places.push_back(p);
  }
  return c;
}

std::string encode(const code& c) {
  // version, count | secret | places (covered) | check
  std::vector<std::uint8_t> b;
  b.push_back(static_cast<std::uint8_t>(kVersion << 4 | ((c.places.size() + (c.relay ? 1 : 0)) & 0x0f)));
  b.insert(b.end(), c.secret.begin(), c.secret.end());
  const std::size_t at = b.size();
  for (const auto& p : c.places) (void)put_place(b, p);
  if (c.relay && put_place(b, c.relay->at, true)) b.insert(b.end(), c.relay->token.begin(), c.relay->token.end());
  cover(c.secret, b.data() + at, b.size() - at);
  std::uint8_t sum[2];
  check(b.data(), b.size(), sum);
  b.insert(b.end(), sum, sum + 2);
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const std::uint8_t x : b) {
    acc = acc << 8 | x;
    bits += 8;
    while (bits >= 5) {
      out.push_back(kAlphabet[(acc >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  if (bits > 0) out.push_back(kAlphabet[(acc << (5 - bits)) & 31]);
  sodium_memzero(b.data(), b.size());
  std::string grouped;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (i > 0 && i % 4 == 0) grouped.push_back('-');
    grouped.push_back(out[i]);
  }
  return grouped;
}

std::string link(const code& c) {
  std::string t = encode(c);
  std::string flat;
  for (char ch : t) {
    if (ch != '-') flat.push_back(ch);
  }
  return std::string(kLinkPrefix) + flat;
}

std::optional<code> decode(std::string_view text) {
  if (text.size() > 512) return std::nullopt;  // a relay code is about 115 characters
  // A link: the part after "code=".
  if (const auto q = text.find("code="); q != std::string_view::npos) text.remove_prefix(q + 5);
  std::vector<std::uint8_t> b;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char ch : text) {
    if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
    if (ch == '&' || ch == '#') break;  // the rest of a link
    const int v = value_of(ch);
    if (v < 0) return std::nullopt;
    acc = (acc << 5 | static_cast<std::uint32_t>(v)) & 0xfff;
    bits += 5;
    if (bits >= 8) {
      b.push_back(static_cast<std::uint8_t>(acc >> (bits - 8)));
      bits -= 8;
    }
  }
  if (b.size() < 1 + kSecretBytes + 2) return std::nullopt;
  if ((b[0] >> 4) != kVersion) return std::nullopt;
  const std::size_t count = b[0] & 0x0f;
  if (count > kMaxPlaces) return std::nullopt;
  std::uint8_t sum[2];
  check(b.data(), b.size() - 2, sum);
  if (sum[0] != b[b.size() - 2] || sum[1] != b[b.size() - 1]) return std::nullopt;  // a typo
  code c;
  std::memcpy(c.secret.data(), b.data() + 1, kSecretBytes);
  const std::size_t at = 1 + kSecretBytes;
  const std::size_t end = b.size() - 2;
  cover(c.secret, b.data() + at, end - at);
  std::size_t i = at;
  for (std::size_t n = 0; n < count; ++n) {
    if (i >= end) return std::nullopt;
    const std::uint8_t kind = b[i++];
    const bool relay = kind == kRelay4 || kind == kRelay6;
    const bool v6 = kind == kKind6 || kind == kRelay6;
    const std::size_t len = v6 ? 16 : kind == kKind4 || kind == kRelay4 ? 4 : 0;
    const std::size_t extra = relay ? 16 : 0;
    // One relay, and it comes last.
    if (len == 0 || i + len + 2 + extra > end || c.relay) return std::nullopt;
    net::address ip;
    ip.v6 = v6;
    std::memcpy(ip.bytes.data(), b.data() + i, len);
    i += len;
    const std::uint16_t port = static_cast<std::uint16_t>(b[i] << 8 | b[i + 1]);
    i += 2;
    if (port == 0) return std::nullopt;
    if (relay) {
      relay_place r;
      r.at = {net::address_text(ip), port};
      std::memcpy(r.token.data(), b.data() + i, 16);
      i += 16;
      c.relay = std::move(r);
    } else {
      c.places.push_back({net::address_text(ip), port});
    }
  }
  if (i != end) return std::nullopt;
  sodium_memzero(b.data(), b.size());
  return c;
}

}  // namespace mv::sharepoint::invite
