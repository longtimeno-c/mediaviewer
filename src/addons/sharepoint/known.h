// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Known devices (issue #472 PR 4): this machine's identity, and the devices
// it remembers, so a later send needs no code.
//
//   - Identity: an X25519 key, made once and kept in the OS's secret store
//     (secrets.h). Protocol 2's XX handshakes carry it, so a peer sees the
//     same key every time. Without a store (or when it fails) each run makes
//     its own, as before PR 4, and nothing can be remembered.
//   - Remembering: after a transfer over protocol 2, either person may tick
//     "Remember this device". The record is the peer's name and key, and a
//     secret only the two ends of that session hold (channel::pairing_secret).
//     It is mutual by construction: a device is found again only when both
//     remembered each other, since the meeting is keyed by the shared secret.
//   - Presence: each hour (UTC) the secret gives the pair a fresh set of DHT
//     keys per direction (the four-word code's set, dht_item.h): the one
//     remembered waits as the receiver under the set toward its own key, the
//     one sending looks up that set as the sender. An observer who does not
//     hold the secret sees unrelated targets each hour.
//   - On every meeting the peer's key must be the one remembered (trust on
//     first use, the pairing nearby); another key ends it, loudly.
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "addons/sharepoint/dht_item.h"
#include "addons/sharepoint/secrets.h"

namespace mv::sharepoint::known {

inline constexpr std::size_t kMaxDevices = 16;
inline constexpr std::size_t kMaxName = 64;  // bytes of UTF-8 kept
inline constexpr const char* kIdentityName = "identity";
inline constexpr const char* kDevicesName = "devices";
inline constexpr std::int64_t kHourMs = 60 * 60 * 1000;

struct device {
  std::string name;
  std::array<std::uint8_t, 32> key{};     // its X25519 identity
  std::array<std::uint8_t, 32> secret{};  // the pairing secret
  // #472 relays: "This is my computer": with "Use my other computers to
  // relay" on, each relays for the other (relay.h). Off when remembered.
  bool own = false;
};

// This machine's identity: kept in `store`, made and kept there when it has
// none. nullptr, or a store that cannot keep it: a key for this run only,
// and `kept` false.
[[nodiscard]] std::array<std::uint8_t, 32> identity(secrets::store* store, bool& kept);

// The remembered devices, in one value of the store (at most kMaxDevices).
[[nodiscard]] std::vector<std::uint8_t> encode(const std::vector<device>& devices);
[[nodiscard]] std::optional<std::vector<device>> decode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<device> load(secrets::store& store);
[[nodiscard]] bool save(secrets::store& store, const std::vector<device>& devices);

// The hour (UTC) a unix time is in, and the pair's DHT keys for it in one
// direction: toward `receiver`, the identity key of the one that waits (its
// own, while it waits; the remembered device's, to send to it). Each side's
// wait and send are apart, so a device never meets its own item.
[[nodiscard]] std::int64_t hour_of(std::int64_t unix_ms) noexcept;
[[nodiscard]] dht::keys presence_keys(const device& d, std::int64_t hour,
                                      const std::array<std::uint8_t, 32>& receiver);

}  // namespace mv::sharepoint::known
