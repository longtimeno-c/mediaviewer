// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link_payload.h"

#include <sodium.h>

#include <vector>

#include "wire/pair_check.h"
#include "wire/keys.h"
#include "core/json.h"
#include "mobile/link.h"

namespace mv::mobile {

namespace {

constexpr std::string_view kPrefix = "mediaviewer://pair#";
constexpr int kVersion = 1;
// One less than the ABI's fields (mv_pair_info), so nothing is ever cut short.
constexpr std::size_t kMaxId = 127, kMaxName = 255, kMaxAddress = 255, kMaxCode = 255;

// Text a person reads or a path is built from: no control characters (NUL
// included), no direction overrides that could make one name look like another.
bool clean_text(const std::string& s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7f) return false;
    if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
        static_cast<unsigned char>(s[i + 1]) <= 0x9f) {
      return false;  // C1 controls
    }
    if (c == 0xD8 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x9C) return false;  // U+061C
    if (c == 0xE2 && i + 2 < s.size()) {
      const auto b1 = static_cast<unsigned char>(s[i + 1]), b2 = static_cast<unsigned char>(s[i + 2]);
      // U+200B..U+200F, U+202A..U+202E, U+2066..U+2069
      if (b1 == 0x80 && ((b2 >= 0x8B && b2 <= 0x8F) || (b2 >= 0xAA && b2 <= 0xAE))) return false;
      if (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9) return false;
    }
  }
  return true;
}

bool text_ok(const std::string* s, std::size_t max, bool may_be_empty = false) {
  return s && (may_be_empty || !s->empty()) && s->size() <= max && clean_text(*s);
}

bool key_of(const std::string* hex, std::array<std::uint8_t, 32>& out) {
  if (!hex) return false;
  const auto parsed = driftwire::parse_key_hex(*hex);
  if (!parsed) return false;
  out = *parsed;
  return true;
}

bool address_ok(const std::string& text) {
  if (text.empty()) return true;
  std::string host;
  std::uint16_t port = 0;
  return split_host_port(text, host, port);
}

}  // namespace

result<pair_payload> parse_pair_payload(std::string_view text) {
  if (text.size() > kMaxPayloadBytes || !text.starts_with(kPrefix)) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);
  const std::string_view coded = text.substr(kPrefix.size());
  std::vector<unsigned char> raw(coded.size() + 1);
  std::size_t raw_len = 0;
  const char* end = nullptr;
  // base64url, padded or not.
  if (sodium_base642bin(raw.data(), raw.size(), coded.data(), coded.size(), "=", &raw_len, &end,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
      end != coded.data() + coded.size()) {
    return err(status::invalid_arg);
  }
  const auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(raw.data()), raw_len), 4);
  if (!doc || doc->integer("v") != kVersion) return err(status::invalid_arg);
  pair_payload p;
  const std::string* id = doc->str("library_id");
  const std::string* name = doc->str("library_name");
  const std::string* code = doc->str("code");
  const auto expires = doc->integer("expires");
  const json::value* service = doc->find("service");
  if (!text_ok(id, kMaxId) || !text_ok(name, kMaxName) || !text_ok(code, kMaxCode) || !expires || !service) {
    return err(status::invalid_arg);
  }
  const std::string* local = service->str("local");
  const std::string* pub = service->str("public");
  if (!text_ok(local, kMaxAddress, true) || !text_ok(pub, kMaxAddress, true) || !address_ok(*local) ||
      !address_ok(*pub) || (local->empty() && pub->empty()) ||
      !key_of(service->str("identity"), p.identity) || !key_of(doc->str("publisher_key"), p.publisher_key) ||
      !key_of(doc->str("access_key"), p.access_key)) {
    return err(status::invalid_arg);
  }
  p.library_id = *id;
  p.library_name = *name;
  p.local = *local;
  p.public_name = *pub;
  p.code = *code;
  p.expires = *expires;
  if (const json::value* g = doc->find("grant"); g && g->k == json::kind::object) {
    const auto for_s = g->integer("for_s");
    if (for_s && *for_s > 0 && *for_s <= 365 * 24 * 3600) p.grant_for_s = *for_s;
    p.grant_scoped = g->boolean("scoped") == true;
  }
  // An address that does not parse is left out, never a reason to refuse the
  // code: the direct routes still work.
  if (const json::value* r = doc->find("relay"); r && r->k == json::kind::object) {
    const std::string* address = r->str("address");
    if (text_ok(address, kMaxAddress, false) && !address->empty() && address_ok(*address)) p.relay = *address;
  }
  p.fingerprint = driftwire::pairing_check(p.library_id, p.publisher_key, p.identity, p.access_key);
  return p;
}

}  // namespace mv::mobile
