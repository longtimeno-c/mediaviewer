// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An identity key as the protocol's JSON carries it ("key": 64 hex digits),
// and back. Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mv::driftwire {

[[nodiscard]] std::string key_hex(const std::array<std::uint8_t, 32>& key);
// nullopt for anything but 64 hex digits.
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> parse_key_hex(std::string_view hex);

// The longest a cancel waits to be said before the link closes (#511): for
// the frame being written, then for the peer to acknowledge it.
inline constexpr int kCancelWaitMs = 250;

}  // namespace mv::driftwire
