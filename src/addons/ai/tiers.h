// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Model tiers (docs/plans/model-tiers.md, #482): which model folder fills each
// role. tools/package/ai-models.py writes the profiles to the Core pack's
// models/tiers.json; the pack (pack.cpp) asks the profile for every folder and
// names no model itself, so a different tower is a manifest change.
//
// A folder is relative to its role's piece: pictures in the Core pack (`ai`),
// speech and sounds in `ai-audio`, faces in `ai-faces`, documents in `ai-docs`.
// Pictures and speech have a high and a fast slot (mv_ai_quality 2 and 1);
// either may be empty (a tier with one tower), never both.
#pragma once

#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::ai {

struct tier_profile {
  std::string id;    // "default"
  std::string name;  // "Default"
  std::string picture_high, picture_fast;  // "models/clip-l14"
  std::string speech_high, speech_fast;
  std::string sounds;
  std::string faces;
  std::string documents;
};

// The profile `tier` of a tiers.json text; "" means the file's default.
// status::corrupt for a malformed file or a folder that is not models/<name>;
// status::not_found for a tier the file does not define.
[[nodiscard]] result<tier_profile> parse_tier_profile(std::string_view json_text, std::string_view tier);

// The folders packs staged before tiers.json existed carry (today's Default).
[[nodiscard]] tier_profile legacy_profile();

// <core_dir>/models/tiers.json's profile `tier`. A pack without the file is
// one staged before it: legacy_profile(). A file that is there but unreadable
// or lacks the tier also falls back, so the pack still opens what it has.
[[nodiscard]] tier_profile read_tier_profile(const std::string& core_dir_utf8, std::string_view tier);

}  // namespace mv::ai
