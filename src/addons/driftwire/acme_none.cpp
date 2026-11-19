// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A build without BoringSSL (acme.h): no certificate from Let's Encrypt; the
// relay's web-site look (if any) keeps a self-signed one.
#include "addons/driftwire/acme.h"

#include <sodium.h>

namespace mv::driftwire::acme {

bool available() noexcept { return false; }

const char* failure_words(failure f) noexcept {
  return f == failure::none ? "issued" : "this build cannot get one";
}

std::string b64url(std::string_view bytes) {
  if (bytes.empty()) return {};
  std::string out(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::string dns01_value(std::string_view, std::string_view) { return {}; }

outcome order(const order_options& o) {
  outcome out;
  out.why = failure::unsupported;
  out.account_key_pem = o.account_key_pem;
  return out;
}

std::optional<cert_info> inspect(std::string_view, std::string_view) { return std::nullopt; }
bool key_matches(std::string_view, std::string_view) { return false; }

}  // namespace mv::driftwire::acme
