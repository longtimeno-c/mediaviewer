// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's half of mv_mobile_vpn.h: the pasted VPN code and the knock key. Apart from
// vpn_abi.cpp so the app does not link the tunnel's stack.
#include <sodium.h>

#include <cstdlib>
#include <cstring>
#include <optional>
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

extern "C" mv_vpn_status mv_vpn_parse_code(const char* text, char** relay, char** key, int32_t* tls,
                                           uint16_t* tls_port) {
  if (!text || !relay || !key || !tls || !tls_port) return MV_VPN_INVALID;
  *relay = nullptr;
  *key = nullptr;
  *tls = 0;
  *tls_port = 0;
  std::optional<std::string> line;  // holds the key: zeroed on every way out
  try {
    bool look = false;
    line = mv::driftwire::serve::parse_relay_connect_code(text, &look);
    if (!line) return MV_VPN_INVALID;
    const auto hash = line->find('#');  // the parser allows exactly one
    // The HTTPS look is on the code's own port, as the desktop's Tunnel takes it. The parser has
    // read this address already, so it can't fail here; the port is all that is taken from it.
    std::string host;
    std::uint16_t port = 0;
    if (!mv::driftwire::serve::parse_relay_address(std::string_view(*line).substr(0, hash), host, port)) {
      sodium_memzero(line->data(), line->size());
      return MV_VPN_INTERNAL;
    }
    *tls = look ? 1 : 0;
    *tls_port = look ? port : 0;
    *relay = copy_out(std::string_view(*line).substr(0, hash));
    *key = copy_out(std::string_view(*line).substr(hash + 1));
    sodium_memzero(line->data(), line->size());
    if (!*relay || !*key) {
      mv_vpn_free(*relay);
      mv_vpn_free(*key);
      *relay = *key = nullptr;
      *tls = 0;
      *tls_port = 0;
      return MV_VPN_INTERNAL;
    }
    return MV_VPN_OK;
  } catch (...) {
    if (line) sodium_memzero(line->data(), line->size());
    mv_vpn_free(*relay);
    mv_vpn_free(*key);
    *relay = *key = nullptr;
    *tls = 0;
    *tls_port = 0;
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
