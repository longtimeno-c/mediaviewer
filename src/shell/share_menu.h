// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Share everywhere (issue #444, docs/design/16 "Share"): one command, `share`,
// that Ctrl/Cmd+Shift+S, the gallery's share icon, the command bar's share
// button and the right-click Share ▸ submenu all run. It opens one menu:
// Sharepoint first (or its one-click install), then the system's own targets
// (Windows Share; the Mac's NSSharingServicePicker). Picking a target is the
// only thing that sends anything (rule 6); nothing is read before that.
//
// This header is the part both hosts share: which files a request acts on,
// what the system sheet gets, and how a request rides the island's one float
// argument. Pure: no I/O, no platform calls.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mv::shell {

// What a share request asks for. `menu` opens the share menu; the others are
// an entry of it picked straight from the right-click Share ▸ submenu.
enum class share_target : std::uint8_t { menu = 0, sharepoint = 1, system = 2 };

// Where it was asked from: only where the menu is anchored.
enum class share_from : std::uint8_t { key = 0, bar = 1, gallery = 2, context = 3 };

struct share_request {
  share_target target = share_target::menu;
  share_from from = share_from::key;
  std::int32_t clicked = -1;  // the right-clicked stop's index; -1 for none
};

// The island's command carries one float, exact for integers below 2^24:
// (clicked + 1) * 16 + from * 4 + target. 0 is the key's request (the menu,
// anchored at the bar, on the selection). A clicked index past 2^20 - 2 does
// not fit and is dropped (the selection is shared instead).
inline constexpr std::int32_t kShareClickedLimit = (1 << 20) - 1;

[[nodiscard]] inline float pack_share_request(const share_request& r) noexcept {
  const std::int32_t clicked = r.clicked >= 0 && r.clicked < kShareClickedLimit ? r.clicked + 1 : 0;
  const std::int32_t packed = clicked * 16 + static_cast<std::int32_t>(r.from) * 4 + static_cast<std::int32_t>(r.target);
  return static_cast<float>(packed);
}

[[nodiscard]] inline share_request unpack_share_request(float arg) noexcept {
  share_request r;
  if (!(arg >= 0.0f) || arg >= 16777216.0f) return r;  // NaN, negative, inexact
  const auto packed = static_cast<std::int32_t>(arg);
  const std::int32_t target = packed & 3;
  const std::int32_t from = (packed >> 2) & 3;
  r.target = target <= 2 ? static_cast<share_target>(target) : share_target::menu;
  r.from = static_cast<share_from>(from);
  r.clicked = (packed >> 4) - 1;
  return r;
}

// The stops a request acts on. `selection` is what copy, F7/F8 and Delete act
// on (the gallery's range while it stands, else the marks, else the current
// stop; docs/design/16 "Gallery range and share"). A right-clicked stop that is
// part of it shares all of it; any other right-clicked stop shares itself.
[[nodiscard]] inline std::vector<std::string> share_stops(std::vector<std::string> selection,
                                                          std::string_view clicked) {
  if (clicked.empty()) return selection;
  if (std::find(selection.begin(), selection.end(), clicked) != selection.end()) return selection;
  return {std::string(clicked)};
}

// Each stop followed by its pair's other half (the RAW of a RAW+JPEG, a Live
// Photo's MOV), as Sharepoint and a drag take them. `pairs` maps a primary to
// its secondary; the host fills it off the UI thread (the listing walk).
[[nodiscard]] inline std::vector<std::string> share_with_pairs(
    std::vector<std::string> stops, const std::map<std::string, std::string, std::less<>>& pairs) {
  if (pairs.empty()) return stops;
  std::vector<std::string> out;
  out.reserve(stops.size() * 2);
  for (auto& s : stops) {
    const auto it = pairs.find(s);
    out.push_back(std::move(s));
    if (it != pairs.end() && !it->second.empty()) out.push_back(it->second);
  }
  return out;
}

// A sidecar beside a camera file: metadata, never media. Sharepoint carries
// them (it takes a file's XMP with it); the system sheet does not (issue #444).
[[nodiscard]] inline bool share_is_sidecar(std::string_view path) noexcept {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  const std::string_view ext = name.substr(dot + 1);
  constexpr std::string_view kSidecars[] = {"xmp", "aae", "thm"};
  for (std::string_view s : kSidecars) {
    if (ext.size() != s.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < s.size() && same; ++i) {
      const char c = ext[i];
      same = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == s[i];
    }
    if (same) return true;
  }
  return false;
}

// What the system sheet gets: the media files in order, each once, no
// sidecars and no empty entries.
[[nodiscard]] inline std::vector<std::string> share_system_files(const std::vector<std::string>& files) {
  std::vector<std::string> out;
  out.reserve(files.size());
  std::unordered_set<std::string_view> seen;
  seen.reserve(files.size());
  for (const std::string& f : files) {
    if (f.empty() || share_is_sidecar(f) || !seen.insert(f).second) continue;
    out.push_back(f);
  }
  return out;
}

}  // namespace mv::shell
