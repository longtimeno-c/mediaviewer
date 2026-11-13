// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Timed and scoped phone access on `driftwire serve`
// (docs/plans/nas-owner-and-guests.md §5, §6.3, §6.4).
//
// A device with any limit (an expiry, a set of named folders, or phone backup
// turned off: docs/plans/mobile.md §10b.1, B2) is kept
// here and NEVER in the paired list (known.h's `devices`): a service binary
// from before this reads only that list, so after a rollback it refuses every
// guest instead of letting one in for ever. Unlimited devices stay in
// `devices`, whose format does not change. Pairing codes that carry a grant
// (from `driftwire serve pair-code --for`) are kept in their own value for the
// same reason: the old one's fixed rows would be redeemed as unlimited.
//
// Times are the service's own clock, in unix seconds. Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/secrets.h"

namespace mv::driftwire::serve::grants {

inline constexpr const char* kGrantsName = "library_grants";
inline constexpr const char* kGuestCodesName = "library_guest_codes";
// An expired grant stays listed this long, for Extend on the card (call C8).
inline constexpr std::int64_t kExpiredKeptSeconds = 7 * 24 * 3600;
inline constexpr std::int64_t kMaxSeconds = 365 * 24 * 3600;
inline constexpr std::size_t kMaxGrants = 64;  // incl. expired ones still listed
inline constexpr std::size_t kMaxGuestCodes = 16;
inline constexpr std::size_t kMaxRootIds = 32;

using key32 = std::array<std::uint8_t, 32>;

struct limit {
  std::int64_t for_s = 0;  // 0: no expiry
  // Named folder ids it may see; nullopt: all of them. The Catalogue is
  // always allowed. An empty list is no folders, never all.
  std::optional<std::vector<std::string>> roots;
  // May create files in the backup folders (phone backup, 1.1). Off is a limit.
  bool backup = true;
  [[nodiscard]] bool any() const noexcept { return for_s != 0 || roots.has_value() || !backup; }
};

struct grant {
  std::string name;
  key32 key{};
  std::int64_t expires = 0;  // 0: never
  std::optional<std::vector<std::string>> roots;
  bool guest = false;        // paired by a guest code
  std::int64_t created = 0;
  bool backup = true;        // may create files in the backup folders (B2); a guest code gives it off
  [[nodiscard]] bool expired(std::int64_t now) const noexcept { return expires != 0 && now >= expires; }
  // Whether a named folder may be read; `catalogue` true for the Catalogue root.
  [[nodiscard]] bool allows(std::string_view root_id, bool catalogue) const;
};

[[nodiscard]] std::string encode(const std::vector<grant>& list);
[[nodiscard]] std::optional<std::vector<grant>> decode(std::string_view json);

// false: there and unreadable (never replaced, and nobody it lists gets in).
// Missing: an empty list, true.
[[nodiscard]] bool load(secrets::store& store, std::vector<grant>& out);
[[nodiscard]] bool save(secrets::store& store, const std::vector<grant>& list);
// Drops grants expired more than kExpiredKeptSeconds ago. True when any went.
bool prune(std::vector<grant>& list, std::int64_t now);
// Room for one more live device beside `unlimited` paired ones (known's
// kMaxDevices in all): drops the oldest expired grant first if that makes
// room. False: full of live devices.
[[nodiscard]] bool make_room(std::vector<grant>& list, std::size_t unlimited, std::int64_t now);
[[nodiscard]] std::size_t live(const std::vector<grant>& list, std::int64_t now);

// A duration as the command line takes it: "90m", "24h", "7d", or seconds.
// nullopt: not one, or not in (0, kMaxSeconds].
[[nodiscard]] std::optional<std::int64_t> parse_duration(std::string_view text);

// ---- codes with a grant, made on the NAS (`driftwire serve pair-code --for`) -------------------
// Single use; only a hash is kept, with its expiry and the grant it gives.
[[nodiscard]] std::string issue_guest_code(secrets::store& store, std::int64_t valid_s, const limit& gives,
                                           std::int64_t now);
// Spends `code` if it is a good one; the grant it gives. nullopt for a wrong,
// spent or expired one, or a store that cannot be rewritten.
[[nodiscard]] std::optional<limit> redeem_guest_code(secrets::store& store, std::string_view code, std::int64_t now);

}  // namespace mv::driftwire::serve::grants
