// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The pairing code a desktop shows (docs/plans/mobile.md §7.4), which reaches
// the phone only through the camera or a test's injection: everything in it is
// untrusted until the person has compared the fingerprint with the desktop.
//   mediaviewer://pair#<base64url of UTF-8 JSON>
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::mobile {

struct pair_payload {
  std::string library_id;
  std::string library_name;
  std::array<std::uint8_t, 32> publisher_key{};
  std::string local;        // "host:port", may be empty
  std::string public_name;  // "name:port", may be empty
  std::array<std::uint8_t, 32> identity{};
  std::array<std::uint8_t, 32> access_key{};
  std::string code;
  std::int64_t expires = 0;
  // A guest code's grant (docs/plans/nas-owner-and-guests.md §4.4), shown before
  // confirming; the service holds the real one against the code.
  std::int64_t grant_for_s = 0;
  bool grant_scoped = false;
  std::string fingerprint;  // driftwire::pairing_check over the library, both keys and the access key
};

inline constexpr std::size_t kMaxPayloadBytes = 4096;

// status::invalid_arg: not one of ours or unreadable.
[[nodiscard]] result<pair_payload> parse_pair_payload(std::string_view text);

}  // namespace mv::mobile
