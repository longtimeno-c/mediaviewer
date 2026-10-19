// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-1 (FIPS 180-4), for the BitTorrent DHT only (#472 PR 3): node ids and
// BEP 44's targets are SHA-1 hashes, and libsodium has none. It is never used
// for anything that needs collision resistance here: a target is found, and
// what is found is checked by its Ed25519 signature and its AEAD. Portable.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace mv::sharepoint {

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) noexcept;

}  // namespace mv::sharepoint
