// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Each paired computer is its own user of the self-hosted computer's LAN SMB share
// (docs/plans/self-host-users-smb.md §12, owner's call 2026-11-27): a computer that
// pairs gets a sharing-only OS account of its own, named after its key, with a long
// random password nobody types. Its role and folders are its pairing's (grants.h,
// known.h): view or change, all folders or some. The account is made, changed and
// taken away by the same elevated step as the people's accounts (host_smb.h), which
// the person approves on that computer; the service hands the login to that computer
// alone, inside its own encrypted session's caps, so its Finder mounts the share with
// no sign-in.
//
// Kept in the service's store (`device_shares`, owner-only, beside the pairing keys).
// The passwords are machine-made and live here, in the elevated step's request and in
// that computer's session only: never logged, never in a listing (rule 6). Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/grants.h"
#include "wire/secrets.h"
#include "wire/serve_wire.h"

namespace mv::driftwire::serve::device_shares {

inline constexpr const char* kName = "device_shares";
inline constexpr std::size_t kMaxDesks = 64;
inline constexpr std::size_t kPasswordBytes = 24;  // 32 characters of base64url

using key32 = std::array<std::uint8_t, 32>;

struct desk {
  key32 key{};
  std::string account;   // account_for(key)
  std::string password;  // "" until the step first made the account
  // What the step last made it: "view" or "change", and its folders' labels (nullopt: all).
  std::string role;
  std::optional<std::vector<std::string>> folders;
  std::int64_t applied = 0;  // 0: never made
  bool gone = false;         // unpaired: the account is to be taken away, then the entry
  ~desk();
  desk() = default;
  desk(const desk&) = default;
  desk(desk&&) = default;
  desk& operator=(const desk&) = default;
  desk& operator=(desk&&) = default;
};

// "mv-" and the key's first 4 bytes in hex: a valid account name on every OS
// (share_users::valid_name), the same every time for the same computer.
[[nodiscard]] std::string account_for(const key32& key);

[[nodiscard]] std::string encode(const std::vector<desk>& list);
[[nodiscard]] std::optional<std::vector<desk>> decode(std::string_view json);
// false: there and unreadable (never replaced). Missing: empty, true.
[[nodiscard]] bool load(secrets::store& store, std::vector<desk>& out);
[[nodiscard]] bool save(secrets::store& store, const std::vector<desk>& list);

// A computer paired: listed (again, if it was going). True when the list changed.
bool add(std::vector<desk>& list, const key32& key);
// A computer unpaired: marked gone if its account was made, dropped if it never was.
bool remove(std::vector<desk>& list, const key32& key);
[[nodiscard]] const desk* find(const std::vector<desk>& list, const key32& key);

// Whether the step must run for it: never made, gone, or made with another role or folders.
[[nodiscard]] bool waiting(const desk& d, std::string_view role, const std::optional<std::vector<std::string>>& folders);

// What a computer's pairing lets it do, in the share's words: "change" when unlimited or its
// grant may write (backup on), else "view"; its folders' labels when its grant names some.
struct want {
  std::string role;
  std::optional<std::vector<std::string>> folders;
};
[[nodiscard]] want want_of(const grants::grant* limited, const std::vector<root_config>& roots);

// A fresh password for a new account (base64url, kPasswordBytes random bytes).
[[nodiscard]] std::string new_password();

}  // namespace mv::driftwire::serve::device_shares
