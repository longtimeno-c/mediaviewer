// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link_payload.h"

#include <sodium.h>

#include <vector>

#include "addons/ai/catalog.h"
#include "addons/driftwire/transfer.h"
#include "core/json.h"
#include "mobile/link.h"

namespace mv::mobile {

namespace {

constexpr std::string_view kPrefix = "mediaviewer://pair#";
constexpr int kVersion = 1;
constexpr std::size_t kMaxText = 256;

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
  if (!id || id->empty() || id->size() > kMaxText || !name || name->empty() || name->size() > kMaxText || !code ||
      code->empty() || code->size() > kMaxText || !expires || !service) {
    return err(status::invalid_arg);
  }
  const std::string* local = service->str("local");
  const std::string* pub = service->str("public");
  if (!local || !pub || !address_ok(*local) || !address_ok(*pub) || (local->empty() && pub->empty()) ||
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
  p.fingerprint = ai::catalog::fingerprint(p.publisher_key);
  return p;
}

}  // namespace mv::mobile
