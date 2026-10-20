// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net.h's addresses (#472 PR 2): text to bytes and back, and which are public.
// Written here rather than with inet_pton/inet_ntop so both hosts parse and
// print the same, and the fuzzer reaches it. Portable (D9).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "addons/driftwire/net.h"

namespace mv::driftwire::net {

namespace {

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Dotted IPv4 into out[0..3]; no leading zeros (they would read as octal elsewhere).
bool parse4(std::string_view t, std::uint8_t* out) noexcept {
  int part = 0;
  std::size_t i = 0;
  while (part < 4) {
    if (i >= t.size() || t[i] < '0' || t[i] > '9') return false;
    if (t[i] == '0' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9') return false;
    int v = 0;
    std::size_t digits = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
      v = v * 10 + (t[i] - '0');
      if (++digits > 3 || v > 255) return false;
      ++i;
    }
    out[part++] = static_cast<std::uint8_t>(v);
    if (part < 4) {
      if (i >= t.size() || t[i] != '.') return false;
      ++i;
    }
  }
  return i == t.size();
}

bool parse6(std::string_view t, std::uint8_t* out) noexcept {
  std::uint16_t words[8] = {};
  int count = 0;
  int gap = -1;  // where "::" stands, in words
  std::size_t i = 0;
  if (t.size() >= 2 && t[0] == ':' && t[1] == ':') {
    gap = 0;
    i = 2;
  } else if (!t.empty() && t[0] == ':') {
    return false;
  }
  while (i < t.size()) {
    if (count >= 8) return false;
    // An IPv4 tail ("::ffff:192.0.2.1"): the rest, if it has a dot.
    const std::size_t end = t.find(':', i);
    const std::string_view piece = t.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
    if (piece.find('.') != std::string_view::npos) {
      if (end != std::string_view::npos || count > 6) return false;
      std::uint8_t v4[4];
      if (!parse4(piece, v4)) return false;
      words[count++] = static_cast<std::uint16_t>(v4[0] << 8 | v4[1]);
      words[count++] = static_cast<std::uint16_t>(v4[2] << 8 | v4[3]);
      i = t.size();
      break;
    }
    if (piece.empty() || piece.size() > 4) return false;
    std::uint16_t w = 0;
    for (const char c : piece) {
      const int d = hex_digit(c);
      if (d < 0) return false;
      w = static_cast<std::uint16_t>(w << 4 | d);
    }
    words[count++] = w;
    i += piece.size();
    if (i == t.size()) break;
    ++i;  // the ':'
    if (i < t.size() && t[i] == ':') {
      if (gap >= 0) return false;  // a second "::"
      gap = count;
      ++i;
    } else if (i == t.size()) {
      return false;  // a trailing single ':'
    }
  }
  if (gap < 0 && count != 8) return false;
  if (gap >= 0 && count > 7) return false;
  std::uint16_t full[8] = {};
  if (gap < 0) {
    for (int k = 0; k < 8; ++k) full[k] = words[k];
  } else {
    for (int k = 0; k < gap; ++k) full[k] = words[k];
    const int tail = count - gap;
    for (int k = 0; k < tail; ++k) full[8 - tail + k] = words[gap + k];
  }
  for (int k = 0; k < 8; ++k) {
    out[2 * k] = static_cast<std::uint8_t>(full[k] >> 8);
    out[2 * k + 1] = static_cast<std::uint8_t>(full[k]);
  }
  return true;
}

}  // namespace

std::optional<address> parse_address(std::string_view text) noexcept {
  if (text.empty() || text.size() > 45) return std::nullopt;
  address a;
  if (text.find(':') == std::string_view::npos) {
    if (!parse4(text, a.bytes.data())) return std::nullopt;
    return a;
  }
  a.v6 = true;
  if (!parse6(text, a.bytes.data())) return std::nullopt;
  return a;
}

std::string address_text(const address& a) {
  char buf[48];
  if (!a.v6) {
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", a.bytes[0], a.bytes[1], a.bytes[2], a.bytes[3]);
    return buf;
  }
  std::uint16_t w[8];
  for (int k = 0; k < 8; ++k) w[k] = static_cast<std::uint16_t>(a.bytes[2 * k] << 8 | a.bytes[2 * k + 1]);
  // RFC 5952: the longest run of two or more zero words (the first of equals) is "::".
  int best = -1, best_len = 0;
  for (int k = 0; k < 8;) {
    if (w[k] != 0) {
      ++k;
      continue;
    }
    int j = k;
    while (j < 8 && w[j] == 0) ++j;
    if (j - k > best_len && j - k >= 2) {
      best = k;
      best_len = j - k;
    }
    k = j;
  }
  std::string out;
  for (int k = 0; k < 8; ++k) {
    if (k == best) {
      out += "::";
      k += best_len - 1;
      continue;
    }
    if (!out.empty() && out.back() != ':') out += ':';
    std::snprintf(buf, sizeof buf, "%x", w[k]);
    out += buf;
  }
  return out;
}

bool is_public(const address& a) noexcept {
  const std::uint8_t* b = a.bytes.data();
  if (!a.v6) {
    if (b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224) return false;  // this, private, loopback, multicast+
    if (b[0] == 100 && (b[1] & 0xc0) == 64) return false;                     // 100.64/10, CGNAT
    if (b[0] == 169 && b[1] == 254) return false;                             // link-local
    if (b[0] == 172 && (b[1] & 0xf0) == 16) return false;                     // 172.16/12
    if (b[0] == 192 && b[1] == 168) return false;                             // 192.168/16
    if (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) return false;   // IETF, TEST-NET-1
    if (b[0] == 198 && (b[1] & 0xfe) == 18) return false;                     // 198.18/15 benchmarking
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return false;               // TEST-NET-2
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return false;                // TEST-NET-3
    return true;
  }
  // Global unicast is 2000::/3; everything else here is not public.
  if ((b[0] & 0xe0) != 0x20) return false;
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // 2001:db8::/32 documentation
  if (b[0] == 0x20 && b[1] == 0x02) return false;                                // 6to4: a relay, not a peer
  return true;
}

bool on_link(const address& a, std::span<const subnet> nets) noexcept {
  const std::uint8_t* b = a.bytes.data();
  if (!a.v6 && (b[0] == 127 || (b[0] == 169 && b[1] == 254))) return true;  // loopback, link-local
  if (a.v6) {
    static constexpr std::uint8_t kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(b, kLoopback, 16) == 0) return true;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;  // fe80::/10
  }
  for (const auto& n : nets) {
    if (n.base.v6 != a.v6) continue;
    const int bits = std::min(n.prefix, a.v6 ? 128 : 32);
    if (bits <= 0) continue;  // a default route's "network" is everything: never on-link
    const int whole = bits / 8;
    if (std::memcmp(b, n.base.bytes.data(), static_cast<std::size_t>(whole)) != 0) continue;
    const int rest = bits % 8;
    if (rest != 0) {
      const auto mask = static_cast<std::uint8_t>(0xff << (8 - rest));
      if ((b[whole] & mask) != (n.base.bytes[whole] & mask)) continue;
    }
    return true;
  }
  return false;
}

}  // namespace mv::driftwire::net
