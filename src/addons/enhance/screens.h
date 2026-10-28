// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's two screens, as view trees both hosts render with their own
// controls (docs/design/25 §10): its page in Settings -> Add-ons, where each
// model is installed, resumed or removed (slice 1), and the sheet over the
// viewer, built from the installed models' knobs for the item on screen
// (slice 2). Pure functions of a view struct, so tests read the trees whole.
//
// Node ids: "install.<kind>", "stop.<kind>", "remove.<kind>" on the page;
// "target", "k.<kind>.<knob>", "run", "cancel", "close" on the sheet.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "addons/enhance/downloads.h"
#include "addons/enhance/prefs.h"
#include "addons/enhance/sniff.h"
#include "addons/enhance/spec.h"

namespace mv::enhance {

inline constexpr const char* kSheetScreen = "enhance";
inline constexpr const char* kSettingsScreen = "settings";
// The preview surface's height, in points (its width is the sheet's).
inline constexpr std::uint32_t kPreviewHeight = 220;

// "63 MB", "4.2 GB".
[[nodiscard]] std::string size_text(std::uint64_t bytes);

struct model_card {
  model_kind kind = model_kind::still;
  const model_spec* spec = nullptr;  // null: this build of Enhance has no model of that kind
  model_files files;
  dl_status st;
};
[[nodiscard]] std::string settings_tree(std::span<const model_card> cards);

struct sheet_section {
  model_kind kind = model_kind::still;
  const model_spec* spec = nullptr;  // the model that serves it, when installed and verified
  // Why there is none: the model is not installed, still downloading, or
  // this build has none of that kind.
  std::string missing;
  std::map<std::string, json::value> values;  // last-used, per kind of target
};

struct sheet_view {
  std::string file_name;     // shown, never logged (rule 6)
  item_kind item = item_kind::unknown;
  bool has_picture = false;  // media: what the clip holds
  bool has_sound = false;
  clip_target target = clip_target::picture;
  std::vector<sheet_section> sections;
  std::string note;          // why Enhance cannot run on this item, or ""
  bool can_run = false;
  // A run in progress or just finished.
  bool running = false;
  double progress = -1;      // -1 indeterminate
  std::string eta;           // "About 12 minutes left", or "" while it cannot tell
  bool preview = false;      // a picture run has posted its first preview
  std::string status;        // "Saved as IMG_0001-enhance.m4a", or why it stopped
};
[[nodiscard]] std::string sheet_tree(const sheet_view& v);

// The targets a clip offers, from what it holds.
[[nodiscard]] std::vector<clip_target> targets_for(bool has_picture, bool has_sound);

// "About 12 minutes left" from how far a run is and how long it has taken;
// "" until a few percent are done (an early guess is a wrong one).
[[nodiscard]] std::string time_left(double fraction, double elapsed_s);

}  // namespace mv::enhance
