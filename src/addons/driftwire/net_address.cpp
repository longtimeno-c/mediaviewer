// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net.h's addresses (#472 PR 2): text to bytes and back, and which are public.
// Written here rather than with inet_pton/inet_ntop so both hosts parse and
// print the same, and the fuzzer reaches it. Portable (D9). Also the parsers
// of Linux's text sources (the Downloads folder, the machine's name, the
// default route), so every OS's tests run them.
#include <algorithm>
#include <charconv>
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

bool is_private_v4(const address& a) noexcept {
  const std::uint8_t* b = a.bytes.data();
  return !a.v6 && (b[0] == 10 || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168));
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
    if (b[0] == 192 && b[1] == 88 && b[2] == 99) return false;                // 6to4 relay anycast (#500)
    if (b[0] == 198 && (b[1] & 0xfe) == 18) return false;                     // 198.18/15 benchmarking
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return false;               // TEST-NET-2
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return false;                // TEST-NET-3
    return true;
  }
  // Global unicast is 2000::/3; everything else here is not public.
  if ((b[0] & 0xe0) != 0x20) return false;
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // 2001:db8::/32 documentation
  if (b[0] == 0x20 && b[1] == 0x02) return false;                                // 6to4: a relay, not a peer
  // #500: 2001::/32 Teredo (a relay, not a peer), 2001:2::/48 benchmarking,
  // 2001:10::/28 ORCHID and 2001:20::/28 ORCHIDv2 (identifiers, not routed).
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00) {
    if (b[3] == 0x00) return false;
    if (b[3] == 0x02 && b[4] == 0x00 && b[5] == 0x00) return false;
    if ((b[3] & 0xf0) == 0x10 || (b[3] & 0xf0) == 0x20) return false;
  }
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

// ---- Linux's text sources -------------------------------------------------------------------

namespace {

bool blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}

// Each line of `text`, without its newline.
template <class F>
void each_line(std::string_view text, F&& f) {
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    f(text.substr(0, nl));
    if (nl == std::string_view::npos) break;
    text.remove_prefix(nl + 1);
  }
}

// The next field of a line split on blanks; empty at its end.
std::string_view next_field(std::string_view& line) noexcept {
  while (!line.empty() && blank(line.front())) line.remove_prefix(1);
  std::size_t n = 0;
  while (n < line.size() && !blank(line[n])) ++n;
  const std::string_view f = line.substr(0, n);
  line.remove_prefix(n);
  return f;
}

template <class T>
bool whole_number(std::string_view s, T& out, int base) noexcept {
  if (s.empty()) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out, base);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

constexpr std::uint32_t kRouteUp = 0x0001, kRouteGateway = 0x0002;  // RTF_UP, RTF_GATEWAY (linux/route.h)

}  // namespace

std::string parse_user_dirs_download(std::string_view user_dirs, std::string_view home) {
  std::string found;
  each_line(user_dirs, [&](std::string_view line) {
    constexpr std::string_view kKey = "XDG_DOWNLOAD_DIR";
    line = trim(line);
    if (line.substr(0, kKey.size()) != kKey) return;
    line = trim(line.substr(kKey.size()));
    if (line.empty() || line.front() != '=') return;
    line = trim(line.substr(1));
    if (line.empty() || line.front() != '"') return;
    line.remove_prefix(1);
    std::string out;
    if (line.substr(0, 6) == "$HOME/") {
      if (home.empty()) return;
      line.remove_prefix(6);
      out.assign(home);
      out.push_back('/');
    } else if (line.empty() || line.front() != '/') {
      return;  // "No other format is supported" (user-dirs.dirs(5))
    }
    bool closed = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
      char c = line[i];
      if (c == '"') {
        closed = true;
        break;
      }
      if (c == '\\' && i + 1 < line.size()) c = line[++i];
      out.push_back(c);
    }
    if (!closed) return;
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    found = std::move(out);
  });
  return found;
}

std::string parse_pretty_hostname(std::string_view machine_info) {
  std::string found;
  each_line(machine_info, [&](std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') return;
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos || trim(line.substr(0, eq)) != "PRETTY_HOSTNAME") return;
    const std::string_view v = trim(line.substr(eq + 1));
    std::string out;
    if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
      // Double quotes take \" \\ \$ \` as the character (a backslash before
      // anything else stays); single quotes take nothing. Past the closing
      // quote, nothing (concatenation is not supported).
      const char quote = v.front();
      bool closed = false;
      for (std::size_t i = 1; i < v.size(); ++i) {
        char c = v[i];
        if (c == quote) {
          closed = true;
          break;
        }
        if (quote == '"' && c == '\\' && i + 1 < v.size() &&
            std::string_view("\"\\$`").find(v[i + 1]) != std::string_view::npos) {
          c = v[++i];
        }
        out.push_back(c);
      }
      if (!closed) return;
    } else {
      for (std::size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (c == '\\' && i + 1 < v.size()) c = v[++i];
        out.push_back(c);
      }
    }
    found = std::move(out);
  });
  return found;
}

std::optional<address> parse_default_route(std::string_view proc_net_route) {
  std::optional<address> best;
  std::uint64_t best_metric = 0;
  // Iface Destination Gateway Flags RefCnt Use Metric Mask ...; the header
  // line's words are not numbers, so it is passed over like any bad line.
  each_line(proc_net_route, [&](std::string_view line) {
    std::string_view f[8];
    for (auto& x : f) x = next_field(line);
    std::uint32_t dest = 0, gw = 0, flags = 0, mask = 0;
    std::uint64_t metric = 0;
    if (f[1].size() != 8 || f[2].size() != 8 || f[7].size() != 8 || !whole_number(f[1], dest, 16) ||
        !whole_number(f[2], gw, 16) || !whole_number(f[3], flags, 16) || !whole_number(f[6], metric, 10) ||
        !whole_number(f[7], mask, 16)) {
      return;
    }
    if (dest != 0 || mask != 0 || gw == 0 || (flags & (kRouteUp | kRouteGateway)) != (kRouteUp | kRouteGateway)) return;
    if (best && metric >= best_metric) return;
    address a;
    std::memcpy(a.bytes.data(), &gw, 4);  // the native word's bytes are the address's, in order
    best = a;
    best_metric = metric;
  });
  return best;
}

}  // namespace mv::driftwire::net
