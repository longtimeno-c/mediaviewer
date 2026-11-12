// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the person compares on the desktop and the phone before pairing (docs/plans/mobile.md
// §7.4). It covers everything a forged code could swap: the library, the key that signs its
// catalogue, the service's identity and the library access key. Both sides call this one function.
//
//   BLAKE2b-256 of: "MediaViewer pairing check v1" (no NUL), then for each of library_id,
//   publisher_key (32), service identity (32), access_key (32): a little-endian u32 length, the
//   bytes. The first 10 bytes (80 bits) as 16 Crockford base-32 characters, in four groups of
//   four joined by '-': "7F3A-92C1-0KXH-M4QZ".
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mv::driftwire {

[[nodiscard]] std::string pairing_check(std::string_view library_id, std::span<const std::uint8_t, 32> publisher_key,
                                        std::span<const std::uint8_t, 32> service_identity,
                                        std::span<const std::uint8_t, 32> access_key);

}  // namespace mv::driftwire
