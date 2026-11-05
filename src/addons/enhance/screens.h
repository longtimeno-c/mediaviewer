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
//
// docs/plans/edit-refresh.md S5 (owner call B): the third screen, "tool",
// docked in the Edit workspace's Enhance tab (place "edit") under the tool the
// person chose: the sheet's knobs for that one tool (no photo choice), the
// preview, the glowing progress, the time left, Cancel, and the result row.
// Its root is "tool.<remove|restyle|detail|extend>", so the host knows which
// card it belongs under; the result row is "result" with "result.thumb" (a
// thumbnail of the new file, its path the host's for Show), "result.show",
// "result.keep" (the host's: navigating is the viewer's) and
// "result.discard" (Enhance's: the host table's recycle_file, to the Bin).
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
#include "addons/enhance/tier.h"

namespace mv::enhance {

inline constexpr const char* kSheetScreen = "enhance";
inline constexpr const char* kSettingsScreen = "settings";
// S5: the docked tool panel (manifest place "edit", contribution API 12).
inline constexpr const char* kToolScreen = "tool";
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
// The performance level's row: what is chosen ("auto" or a tier name) and
// this computer's recommendation.
struct tier_view {
  std::string chosen = "auto";
  tier recommended = tier::standard;
};
[[nodiscard]] std::string settings_tree(std::span<const model_card> cards, const tier_view& tiers = {});

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
  std::string edit;          // edit_text: the Video Editor's cut, or ""
  item_kind item = item_kind::unknown;
  bool has_picture = false;  // media: what the clip holds
  bool has_sound = false;
  clip_target target = clip_target::picture;
  // A photo with Restyle shipped: the choice between more detail and a
  // typed restyle (slice 5b).
  bool offers_restyle = false;
  bool offers_remove = false;  // slice 7: something is brushed and Remove has a model
  bool offers_extend = false;  // Remove's model, extending the picture's edges
  photo_target photo = photo_target::detail;
  std::vector<sheet_section> sections;
  std::string note;          // why Enhance cannot run on this item, or ""
  bool can_run = false;
  // A run in progress or just finished.
  bool running = false;
  double progress = -1;      // -1 indeterminate
  std::string eta;           // "About 12 minutes left", or "" while it cannot tell
  bool preview = false;      // a picture run has posted its first preview
  std::string status;        // "Saved as IMG_0001-enhance.m4a", or why it stopped
  // S5, the docked panel: the file the last run wrote (absolute, or the
  // "photos:<id>" key of a new Photos-library photo), "" when there is none
  // or it was discarded; its name as shown; Remove with nothing brushed yet.
  std::string result_path;
  std::string result_name;
  std::string result_note;  // why Discard kept the file, under the row
  bool brush_needed = false;
  bool library = false;  // the item is a Photos-library photo (docs/design/26)
};
[[nodiscard]] std::string sheet_tree(const sheet_view& v);
// S5: the docked panel for v.photo's tool (no photo choice, no Close: the
// card's own close button closes it).
[[nodiscard]] std::string tool_tree(const sheet_view& v);

// The Video Editor's cut, in the sheet's words ("The edit from the Video
// Editor: 3 pieces, 1:24 in all. …"); "" for the whole clip.
[[nodiscard]] std::string edit_text(std::span<const std::int64_t> keep_ns);

// The targets a clip offers, from what it holds.
[[nodiscard]] std::vector<clip_target> targets_for(bool has_picture, bool has_sound);

// "About 12 minutes left" from how far a run is and how long it has taken;
// "" until a few percent are done (an early guess is a wrong one).
[[nodiscard]] std::string time_left(double fraction, double elapsed_s);

}  // namespace mv::enhance
