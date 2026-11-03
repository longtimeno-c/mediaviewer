// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): what the add-on remembers, in its data folder
// (enhance.json): the last-used knob values per kind of target, a clip's
// last target (picture, sound or both), and which models the person chose to
// install, so a download a restart cut off resumes without a second click.
// Nothing about a file is kept (rule 6).
#pragma once

#include <array>
#include <map>
#include <string>

#include "addons/enhance/spec.h"
#include "core/json.h"

namespace mv::enhance {

// What Enhance does to a clip.
enum class clip_target : std::uint8_t { picture = 0, sound = 1, both = 2 };
[[nodiscard]] const char* target_name(clip_target t) noexcept;  // "picture" | "sound" | "both"

// What Enhance does to a photo (slice 5b): more detail (the still model) or
// a typed restyle.
// remove (slice 7): fill in what was brushed in Edit, offered while it is.
enum class photo_target : std::uint8_t { detail = 0, restyle = 1, remove = 2 };
inline constexpr photo_target kPhotoTargets[] = {photo_target::detail, photo_target::restyle, photo_target::remove};
[[nodiscard]] const char* photo_target_name(photo_target t) noexcept;  // "detail" | "restyle" | "remove"

struct prefs {
  // values[kind][knob id], for a target of that kind.
  std::array<std::map<std::string, json::value>, kKindCount> values;
  clip_target target = clip_target::picture;
  photo_target photo = photo_target::detail;
  // The person clicked Install for that kind's model (and did not Remove it).
  std::array<bool, kKindCount> wanted{};

  [[nodiscard]] std::map<std::string, json::value>& of(model_kind k) { return values[static_cast<int>(k)]; }
  [[nodiscard]] const std::map<std::string, json::value>& of(model_kind k) const {
    return values[static_cast<int>(k)];
  }
  [[nodiscard]] bool& want(model_kind k) { return wanted[static_cast<int>(k)]; }
  [[nodiscard]] bool want(model_kind k) const { return wanted[static_cast<int>(k)]; }
};

[[nodiscard]] std::string prefs_json(const prefs& p);
// Whatever of `json_text` is well formed; the rest at its default.
[[nodiscard]] prefs prefs_from(std::string_view json_text);

// The file: read (defaults when absent or unreadable) and written whole,
// through a temporary renamed into place.
[[nodiscard]] prefs load_prefs(const std::string& path_utf8);
bool save_prefs(const std::string& path_utf8, const prefs& p);

}  // namespace mv::enhance
