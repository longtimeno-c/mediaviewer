// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where files go: layouts, rename templates, safe names (plan/18 "Layout",
// "Rename"). Pure functions of their inputs, identical on Windows and Mac
// (PR 18 verify: "a rename template yields identical names on Windows and Mac
// for the same card"), so nothing here asks the OS anything.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "addons/import/model.h"

namespace mv::import {

struct civil {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// Seconds since 1970 (treated as a wall clock, no zone) to calendar fields.
[[nodiscard]] civil civil_from_seconds(std::int64_t s) noexcept;
[[nodiscard]] std::string day_key(std::int64_t s);  // "YYYY-MM-DD"
// A file time (UTC seconds) as this machine's local wall clock, the frame
// capture dates are already in. The one place naming asks the OS anything.
[[nodiscard]] std::int64_t local_wall_from_utc(std::int64_t utc) noexcept;
// "YYYY-MM-DD" to days since 1970; false if malformed.
[[nodiscard]] bool parse_day(std::string_view text, std::int64_t& days) noexcept;

// A file name component that is legal on Windows, macOS and exFAT alike:
// control characters and < > : " / \ | ? * become '_', trailing dots and
// spaces are dropped, reserved DOS device names get a trailing '_'.
[[nodiscard]] std::string sanitize_component(std::string_view text);

// The name's stem as the pairing sees it ("IMG_0001" for "IMG_0001.CR3" and
// for the sidecar "IMG_0001.CR3.xmp"), and what follows it (".CR3.xmp").
[[nodiscard]] std::string_view stem_of(std::string_view name) noexcept;

struct layout_input {
  std::int64_t taken = 0;
  std::string_view camera;
  bool has_raw = false;
  file_type primary_type = file_type::other;
  std::string_view source_rel_dir;  // '/'-separated, "" at the root
};

// The destination folder for a unit, relative, '/'-separated.
[[nodiscard]] std::string layout_folder(const preset& p, const layout_input& in);

struct rename_input {
  std::int64_t taken = 0;
  std::string_view camera;
  std::uint32_t seq = 0;
  std::string_view original_stem;
};

// `{date}` YYYY-MM-DD, `{time}` HHMMSS, `{camera}`, `{seq}` (4 digits, per
// day), `{original}` the camera's stem. Unknown braces are kept literally.
// Returns the new stem, sanitized; the caller appends each member's tail.
[[nodiscard]] std::string render_stem(std::string_view tmpl, const rename_input& in);
[[nodiscard]] bool template_uses_seq(std::string_view tmpl) noexcept;

// "IMG_0001 (2).CR3": the clash suffix goes after the stem, so every member
// of a unit keeps the same stem (plan/18: pairs stay together).
// `name` with its leading `old_stem` (the unit primary's stem, which every
// member starts with, ASCII case-insensitively) replaced by `new_stem`.
[[nodiscard]] std::string with_stem(std::string_view name, std::string_view old_stem,
                                    std::string_view new_stem);
[[nodiscard]] std::string clash_stem(std::string_view stem, int n);

}  // namespace mv::import
