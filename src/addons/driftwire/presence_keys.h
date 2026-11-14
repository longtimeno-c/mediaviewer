// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The pair's DHT keys for an hour (docs/design/28 "Remembered devices"):
// the half of a remembered device's record that only Driftwire's internet
// path uses, kept out of the protocol (wire/known.h) so the app's phone
// feature links no DHT. Portable (D9).
#pragma once

#include <array>
#include <cstdint>

#include "addons/driftwire/dht_item.h"
#include "wire/known.h"

namespace mv::driftwire::known {

// The pair's DHT keys for `hour` in one direction: toward `receiver`, the
// identity key of the one that waits (its own, while it waits; the
// remembered device's, to send to it). Each side's wait and send are apart,
// so a device never meets its own item.
[[nodiscard]] dht::keys presence_keys(const device& d, std::int64_t hour,
                                      const std::array<std::uint8_t, 32>& receiver);

}  // namespace mv::driftwire::known
