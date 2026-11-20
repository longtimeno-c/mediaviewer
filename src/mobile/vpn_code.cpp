// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's half of mv_mobile_vpn.h: the pasted VPN code and the knock key. Apart from
// vpn_abi.cpp so the app does not link the tunnel's stack.
#include <sodium.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "mobile/mv_mobile_vpn.h"
#include "wire/serve_wire.h"
#include "wire/tls.h"

namespace {

char* copy_out(std::string_view s) {
  auto* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

}  // namespace

extern "C" mv_vpn_status mv_vpn_parse_code(const char* text, char** relay, char** key) {
  if (!text || !relay || !key) return MV_VPN_INVALID;
  *relay = nullptr;
  *key = nullptr;
  try {
    auto line = mv::driftwire::serve::parse_relay_connect_code(text);
    if (!line) return MV_VPN_INVALID;
    const auto hash = line->find('#');  // the parser allows exactly one
    *relay = copy_out(std::string_view(*line).substr(0, hash));
    *key = copy_out(std::string_view(*line).substr(hash + 1));
    sodium_memzero(line->data(), line->size());
    if (!*relay || !*key) {
      mv_vpn_free(*relay);
      mv_vpn_free(*key);
      *relay = *key = nullptr;
      return MV_VPN_INTERNAL;
    }
    return MV_VPN_OK;
  } catch (...) {
    return MV_VPN_INTERNAL;
  }
}

extern "C" void mv_vpn_free(char* s) {
  if (!s) return;
  sodium_memzero(s, std::strlen(s));
  std::free(s);
}

extern "C" mv_vpn_status mv_vpn_derive_knock_key(const char* key, uint8_t out[32]) {
  if (!key || !out || !*key) return MV_VPN_INVALID;
  try {
    auto k = mv::driftwire::tls::derive_knock_key(key);
    if (!k) return MV_VPN_INTERNAL;
    std::memcpy(out, k->data(), k->size());
    sodium_memzero(k->data(), k->size());
    return MV_VPN_OK;
  } catch (...) {
    return MV_VPN_INTERNAL;
  }
}
