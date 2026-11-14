// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A remote item's key (docs/plans/desktop-client.md §3 "Keys"): what the viewer
// lists, thumbs, marks and opens in place of a path for a file on a paired
// `driftwire serve` (a self-hosted computer or a Synology), the way the Photos
// library's items are "photos:<id>" (docs/design/26 "Virtual items").
//
//   remote:<server>/                the server's top: its named folders
//   remote:<server>/<root>/         a named folder
//   remote:<server>/<root>/<rel>/   a folder inside it (always ends in '/')
//   remote:<server>/<root>/<rel>    a file
//
// <server> and <root> are 16 lowercase hex digits; <rel> is the service's own
// relative path, '/'-separated. Pure, no I/O, portable (D9).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace mv::remote {

inline constexpr std::string_view kKeyPrefix = "remote:";

[[nodiscard]] constexpr bool is_key(std::string_view text) noexcept {
  return text.size() >= kKeyPrefix.size() && text.substr(0, kKeyPrefix.size()) == kKeyPrefix;
}

struct key_parts {
  std::string server;  // 16 hex
  std::string root;    // 16 hex, empty for the server's top
  std::string rel;     // no leading or trailing '/'; empty for a named folder itself
  bool folder = true;  // false: a file
};

// nullopt: not a key of ours, or a part that is not well-formed (".." and
// empty segments are refused, as the service refuses them).
[[nodiscard]] std::optional<key_parts> parse_key(std::string_view text);
[[nodiscard]] std::string make_key(const key_parts& parts);
[[nodiscard]] std::string server_key(std::string_view server);
// The folder a key is in ("" for the server's top).
[[nodiscard]] std::string parent_key(std::string_view key);
// The last segment of <rel> ("" for a server's top or a named folder).
[[nodiscard]] std::string leaf_name(std::string_view key);
[[nodiscard]] bool is_hex16(std::string_view s) noexcept;

}  // namespace mv::remote
