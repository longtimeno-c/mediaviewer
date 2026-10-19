// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/wire.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/json.h"

namespace mv::driftwire {

namespace {

constexpr std::string_view kBeaconMagic = "MVSP1";

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// A name Windows opens as a device in every folder (CON, NUL, COM1 ...), with
// or without an extension.
bool windows_device(std::string_view part) noexcept {
  std::string_view base = part.substr(0, part.find('.'));
  while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
  for (std::string_view d : {"con", "prn", "aux", "nul", "conin$", "conout$"}) {
    if (iequals(base, d)) return true;
  }
  if (base.size() < 4 || !(iequals(base.substr(0, 3), "com") || iequals(base.substr(0, 3), "lpt"))) return false;
  const std::string_view n = base.substr(3);
  return (n.size() == 1 && n[0] >= '0' && n[0] <= '9') || n == "\xC2\xB9" || n == "\xC2\xB2" || n == "\xC2\xB3";
}

// Cut at most `max` bytes without splitting a UTF-8 sequence.
std::string_view cut_utf8(std::string_view s, std::size_t max) noexcept {
  if (s.size() <= max) return s;
  std::size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

}  // namespace

const char* transport_name(transport t) noexcept {
  switch (t) {
    case transport::bluetooth: return "Bluetooth";
    case transport::internet: return "the internet";
    case transport::wifi: break;
  }
  return "Wi-Fi";
}

namespace {
std::string code_of(crypto_generichash_state& st) {
  std::uint8_t out[16];
  crypto_generichash_final(&st, out, sizeof out);
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | out[i];
  const unsigned n = static_cast<unsigned>(v % 1000000u);
  char text[8];
  std::snprintf(text, sizeof text, "%03u %03u", n / 1000, n % 1000);
  return text;
}
}  // namespace

std::string pairing_code(std::span<const std::uint8_t, 32> client_pk, std::span<const std::uint8_t, 32> server_pk) {
  static constexpr char kContext[] = "MediaViewer Driftwire code v1";
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 16);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  crypto_generichash_update(&st, client_pk.data(), client_pk.size());
  crypto_generichash_update(&st, server_pk.data(), server_pk.size());
  return code_of(st);
}

std::string remember_check(std::span<const std::uint8_t, 32> session_secret) {
  static constexpr char kContext[] = "MediaViewer Driftwire remember check v1";
  crypto_generichash_state st;
  crypto_generichash_init(&st, session_secret.data(), session_secret.size(), 16);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  return code_of(st);
}

std::string pairing_code_v2(std::span<const std::uint8_t> handshake_hash, std::span<const std::uint8_t, 32> client_nonce) {
  static constexpr char kContext[] = "MediaViewer Driftwire code v2";
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 16);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  crypto_generichash_update(&st, handshake_hash.data(), handshake_hash.size());
  crypto_generichash_update(&st, client_nonce.data(), client_nonce.size());
  return code_of(st);
}

std::string pairing_code_v3(std::span<const std::uint8_t> handshake_hash, std::span<const std::uint8_t, 32> client_nonce,
                            std::span<const std::uint8_t, 32> server_nonce) {
  static constexpr char kContext[] = "MediaViewer Driftwire code v3";
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 16);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  crypto_generichash_update(&st, handshake_hash.data(), handshake_hash.size());
  crypto_generichash_update(&st, client_nonce.data(), client_nonce.size());
  crypto_generichash_update(&st, server_nonce.data(), server_nonce.size());
  return code_of(st);
}

bool safe_relative(std::string_view rel) noexcept {
  if (rel.empty() || rel.size() > kMaxRelativeBytes || !json::detail::valid_utf8(rel)) return false;
  if (rel.front() == '/') return false;
  // '/' only, on every OS; no drive letter, no stream name, no wildcard.
  if (rel.find_first_of("\\:<>\"|?*") != std::string_view::npos) return false;
  for (char c : rel) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
  }
  std::string_view rest = rel;
  int depth = 0;
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    if (part.empty() || part == "." || part == ".." || part.size() > kMaxPartBytes) return false;
    // Windows drops a trailing dot or space ("a." is "a"), and a device name
    // is a device in every folder.
    if (part.back() == '.' || part.back() == ' ' || windows_device(part)) return false;
    if (++depth > 32) return false;
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
    if (rest.empty()) return false;  // a trailing '/'
  }
  return true;
}

std::string_view leaf_name(std::string_view path) noexcept {
  const auto cut = path.find_last_of("/\\");
  return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

bool valid_id(std::string_view id) noexcept {
  return id.size() == 16 && std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string clean_name(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out.push_back(c);
  }
  if (!json::detail::valid_utf8(out)) out.clear();
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  out = std::string(cut_utf8(out, kMaxNameBytes));
  return out.empty() ? std::string("MediaViewer") : out;
}

std::string encode_beacon(const beacon& b) {
  json::writer w;
  w.begin_object()
      .key("v").integer(kBeaconVersion)
      .key("p").integer(b.protocol)
      .key("id").string(b.id)
      .key("name").string(clean_name(b.name))
      .key("port").integer(b.port)
      .end_object();
  return std::string(kBeaconMagic) + w.take();
}

std::uint32_t protocol_of(std::optional<std::int64_t> p) noexcept {
  if (!p || *p <= static_cast<std::int64_t>(kProtocol1)) return kProtocol1;
  // A newer one speaks ours too: the client asks for the highest both have.
  return *p >= static_cast<std::int64_t>(kProtocolVersion) ? kProtocolVersion : static_cast<std::uint32_t>(*p);
}

std::optional<beacon> decode_beacon(std::span<const std::uint8_t> datagram) {
  if (datagram.size() <= kBeaconMagic.size() || datagram.size() > kMaxBeaconBytes) return std::nullopt;
  const std::string_view text(reinterpret_cast<const char*>(datagram.data()), datagram.size());
  if (text.substr(0, kBeaconMagic.size()) != kBeaconMagic) return std::nullopt;
  const auto doc = json::parse(text.substr(kBeaconMagic.size()), 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const auto v = doc->integer("v");
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const auto port = doc->integer("port");
  // A newer beacon is not ours to answer ("p" is how a newer protocol says so).
  if (!v || *v != static_cast<std::int64_t>(kBeaconVersion) || !id || !valid_id(*id) || !name || !port ||
      *port <= 0 || *port > 65535) {
    return std::nullopt;
  }
  beacon b;
  b.id = *id;
  b.name = clean_name(*name);
  b.port = static_cast<std::uint16_t>(*port);
  b.protocol = protocol_of(doc->integer("p"));
  return b;
}

std::string size_text(std::uint64_t bytes) {
  char buf[32];
  if (bytes >= 1000ull * 1000 * 1000) {
    std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1e9);
  } else if (bytes >= 1000ull * 1000) {
    std::snprintf(buf, sizeof buf, "%.0f MB", static_cast<double>(bytes) / 1e6);
  } else if (bytes >= 1000) {
    std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(bytes) / 1e3);
  } else {
    std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

}  // namespace mv::driftwire
