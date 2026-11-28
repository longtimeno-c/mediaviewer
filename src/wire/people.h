// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The people a server's devices belong to (self-host-users-smb.md §13, owner's call
// 2026-11-28): "link a user to a device, then set that user's permission". A person
// has a name and a role ("view" or "change"). Each paired device belongs to at most
// one person, and a device that belongs to someone has that person's role: in the
// app (the service's grants) and, for a computer, on the LAN share (its own account,
// device_shares.h). A device that belongs to nobody keeps a role of its own.
//
// No sign-in and no password: a person is a name on this server's lists, not an OS
// account (people who need a password of their own are share_users.h's). Kept in
// the service's store (`people`); names are shown on the owner's screens only,
// never logged (rule 6). Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/secrets.h"

namespace mv::driftwire::serve::people {

inline constexpr const char* kName = "people";
inline constexpr std::size_t kMaxPeople = 32;
inline constexpr std::size_t kMaxNameBytes = 40;
inline constexpr std::size_t kMaxDevices = 64;

using key32 = std::array<std::uint8_t, 32>;

struct person {
  std::string name;
  std::string role = "view";  // "view" or "change"
  std::vector<key32> devices;
};

// 1 to kMaxNameBytes after trimming spaces, no control characters. The trimmed name.
[[nodiscard]] std::optional<std::string> clean_name(std::string_view name);
[[nodiscard]] bool valid_role(std::string_view role) noexcept;

[[nodiscard]] std::string encode(const std::vector<person>& list);
// nullopt: not that shape, a bad name or role, a name twice (ignoring case), a device on two people.
[[nodiscard]] std::optional<std::vector<person>> decode(std::string_view json);
// false: there and unreadable (never replaced). Missing: empty, true.
[[nodiscard]] bool load(secrets::store& store, std::vector<person>& out);
[[nodiscard]] bool save(secrets::store& store, const std::vector<person>& list);

// The person a device belongs to, or nullptr.
[[nodiscard]] const person* owner_of(const std::vector<person>& list, const key32& device);
[[nodiscard]] person* find(std::vector<person>& list, std::string_view name);  // ignoring case

// Adds a person or changes one's role (by name, ignoring case). "" when done, else why:
// "bad-name", "bad-role", "too-many".
[[nodiscard]] std::string set(std::vector<person>& list, std::string_view name, std::string_view role);
// Removes a person; their devices belong to nobody (and keep the role they had). True when one went.
bool remove(std::vector<person>& list, std::string_view name);
// Links a device to `name` (taken from anyone else), or to nobody with "". "" when done,
// else "not-found" (no such person) or "too-many".
[[nodiscard]] std::string link(std::vector<person>& list, const key32& device, std::string_view name);
// A device unpaired: it belongs to nobody. True when it belonged to someone.
bool forget_device(std::vector<person>& list, const key32& device);

}  // namespace mv::driftwire::serve::people
