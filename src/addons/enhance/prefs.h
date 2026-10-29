// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): what the add-on remembers, in its data folder
// (enhance.json): the last-used knob values per kind of target, a clip's
// last target (picture, sound or both), which model each kind uses, and which
// models the person chose to install, so a download a restart cut off resumes
// without a second click.
// Nothing about a file is kept (rule 6).
#pragma once

#include <array>
#include <map>
#include <set>
#include <string>

#include "addons/enhance/spec.h"
#include "core/json.h"

namespace mv::enhance {

// What Enhance does to a clip.
enum class clip_target : std::uint8_t { picture = 0, sound = 1, both = 2 };
[[nodiscard]] const char* target_name(clip_target t) noexcept;  // "picture" | "sound" | "both"

// A model's key, "<kind>.<id>": its download and its place in `wanted`.
[[nodiscard]] std::string model_key(model_kind k, const std::string& id);


struct prefs {
  // values[kind][knob id], for a target of that kind.
  std::array<std::map<std::string, json::value>, 3> values;
  clip_target target = clip_target::picture;
  // The model each kind uses, by id; "" until the person chooses (model.json's).
  std::array<std::string, 3> use;
  // The person clicked Install for these models (and did not Remove them),
  // by key "<kind>.<id>". An enhance.json from before models had ids names a
  // kind alone: that kind's model.json ("<kind>.model").
  std::set<std::string> wanted;

  [[nodiscard]] std::map<std::string, json::value>& of(model_kind k) { return values[static_cast<int>(k)]; }
  [[nodiscard]] const std::map<std::string, json::value>& of(model_kind k) const {
    return values[static_cast<int>(k)];
  }
  [[nodiscard]] std::string& use_of(model_kind k) { return use[static_cast<int>(k)]; }
  [[nodiscard]] const std::string& use_of(model_kind k) const { return use[static_cast<int>(k)]; }
  [[nodiscard]] bool want(const std::string& key) const { return wanted.count(key) != 0; }
};

[[nodiscard]] std::string prefs_json(const prefs& p);
// Whatever of `json_text` is well formed; the rest at its default.
[[nodiscard]] prefs prefs_from(std::string_view json_text);

// The file: read (defaults when absent or unreadable) and written whole,
// through a temporary renamed into place.
[[nodiscard]] prefs load_prefs(const std::string& path_utf8);
bool save_prefs(const std::string& path_utf8, const prefs& p);

}  // namespace mv::enhance
