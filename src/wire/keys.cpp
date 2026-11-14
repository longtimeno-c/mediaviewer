// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/keys.h"

#include <sodium.h>

namespace mv::driftwire {

std::string key_hex(const std::array<std::uint8_t, 32>& key) {
  char hex[65];
  sodium_bin2hex(hex, sizeof hex, key.data(), key.size());
  return std::string(hex, 64);
}

std::optional<std::array<std::uint8_t, 32>> parse_key_hex(std::string_view hex) {
  if (hex.size() != 64) return std::nullopt;
  std::array<std::uint8_t, 32> key{};
  std::size_t n = 0;
  if (sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(), nullptr, &n, nullptr) != 0 || n != key.size()) {
    return std::nullopt;
  }
  return key;
}

}  // namespace mv::driftwire
