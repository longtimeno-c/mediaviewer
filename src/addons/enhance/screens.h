// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's two screens, as view trees both hosts render with their own
// controls (docs/design/25 §10): its page in Settings -> Add-ons, where each
// model is installed, resumed or removed (slice 1), and the sheet over the
// viewer, built from the installed models' knobs for the item on screen
// (slice 2). Pure functions of a view struct, so tests read the trees whole.
//
// Node ids: "install.<kind>", "stop.<kind>", "remove.<kind>" for the kind's
// chosen model, "remove.<kind>.<id>" / "stop.<kind>.<id>" for one of its others,
// and "model.<kind>" (the choice of a kind's model, when it has more than one)
// on the page; "tier" (the level's choice) and "tierlevel" (the level in
// effect, "Default level": the text the Edit pane's Enhance tab shows in its
// chip); "target", "photo", "detail" (S7: Enhance image's model),
// "model.<kind>", "k.<kind>.<knob>", "install.<kind>", "stop.<kind>", "run",
// "cancel", "close" on the sheet.
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
// docs/design/30: the sheet's options drawn inside another add-on's window.
inline constexpr const char* kPanelScreen = "panel";
// S5: the docked tool panel (manifest place "edit", contribution API 13).
inline constexpr const char* kToolScreen = "tool";
// The preview surface's height, in points (its width is the sheet's).
inline constexpr std::uint32_t kPreviewHeight = 220;

// "63 MB", "4.2 GB".
[[nodiscard]] std::string size_text(std::uint64_t bytes);

// Enhance image's choices as the sheet names them ("Sharpen & restore", …).
[[nodiscard]] const char* detail_text(model_kind k) noexcept;

// One model of a kind, and where its download stands.
struct model_choice {
  const model_spec* spec = nullptr;
  model_files files;
  dl_status st;
};

struct model_card {
  model_kind kind = model_kind::still;
  std::vector<model_choice> models;  // empty: this build of Enhance has none of that kind
  std::size_t chosen = 0;            // the one the kind uses
};
// The performance level's row: what is chosen ("auto" or a tier name) and
// this computer's recommendation.
struct tier_view {
  std::string chosen = "auto";
  tier recommended = tier::standard;
  // What a run uses: the chosen level, or the recommendation for "auto".
  [[nodiscard]] tier effective() const noexcept {
    tier t = recommended;
    if (chosen != "auto") (void)tier_from(chosen, t);
    return t;
  }
};
[[nodiscard]] std::string settings_tree(std::span<const model_card> cards, const tier_view& tiers = {});

struct sheet_section {
  model_kind kind = model_kind::still;
  const model_spec* spec = nullptr;  // the model that serves it, when installed and verified
  // Why there is none: the model is not installed, still downloading, or
  // this build has none of that kind.
  std::string missing;
  // The kind's models to choose from, when it has more than one (id, name),
  // and the chosen one's id.
  std::vector<knob_option> models;
  std::string model;
  // The chosen model is not installed: its Download button ("Download (64
  // MB)", "Resume the download", "Try again"; "install.<kind>", S7: fetched
  // from the sheet itself), or its progress while it downloads (0-1, -1
  // unknown; "stop.<kind>" stops it). Empty and unset: no button.
  std::string install;
  std::optional<double> downloading;
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
  bool offers_faces = false;   // S10: Restore faces has a model
  // edit-refresh S6: the host holds a frame dragged past this photo's edges
  // (still_extend); Extend uses it, so the preset choice is not shown.
  bool framed = false;
  photo_target photo = photo_target::detail;
  // The performance level in effect: Restyle's qualities above what it allows
  // are shown disabled with the level they need (docs/plans/edit-refresh.md S9).
  tier level = tier::standard;
  // S7: Enhance image's models this build offers (kStillKinds with a spec and
  // pins), and the chosen one; the "detail" choice when more than one.
  std::vector<model_kind> details;
  model_kind detail = model_kind::still;
  std::vector<sheet_section> sections;
  std::string note;          // why Enhance cannot run on this item, or ""
  bool can_run = false;
  // A run in progress or just finished.
  bool running = false;
  double progress = -1;      // -1 indeterminate
  std::string eta;           // "About 12 minutes left", or "" while it cannot tell
  bool preview = false;      // a picture run has posted its first preview
  bool preview_split = false;      // ...split before / after (else a photo being redrawn)
  std::uint32_t preview_height = kPreviewHeight;
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

// ---- One-click (owner 2026-11-05) ------------------------------------------------
// The "tool" screen (place "edit") is the Edit pane's Enhance tab: no panel
// and no sheet. Its tree's root is "cards"; each tool the add-on has is a
// stack "card.<detail|restyle|faces|remove|extend>" the host draws inside its
// own card (the compact choices, the time it will take, the one button
// "go.<word>": "Enhance", or "Download (161 MB) and enhance"), and one hidden
// stack "run" says what the run is doing for the host's glow over the photo,
// its tool bar line and opening the result (docs/design/29 "One click").
//
//   run (hidden; label = idle | running | downloading | done | failed | cancelled)
//     run.progress   progress, value = the fraction (-1 while it cannot say)
//     run.line       "Enhancing… 40 % · 1 min left", "Saved as IMG-enhance.jpg", the reason
//     run.id         the run's number (a host opens a result once per run)
//     run.tool       detail | restyle | faces | remove | extend
//     run.result     thumbnail, the new file's path (or a library key): done only
//     cancel, result.discard ("Move to Bin"), run.dismiss
inline constexpr const char* kCardsRoot = "cards";

struct card_view {
  photo_target tool = photo_target::detail;
  // The tool's model as this Enhance ships it (its knobs show before it is
  // downloaded); null when it has none.
  const model_spec* spec = nullptr;
  // Its download: running (with a fraction, -1 unknown), and what the one
  // button says when it is not here ("Download (161 MB)", "Resume the
  // download", "Try again"; "" when it is ready or cannot be fetched).
  bool downloading = false;
  double download_fraction = -1;
  std::string download_text;  // "48 MB of 161 MB"
  std::string install;
  // That tool's view: its sections (knobs, the model's download state), the
  // note when it cannot run, can_run.
  sheet_view v;
  std::string eta;  // "About 2 min", "Estimating…", "About 2 seconds a face", or ""
};

enum class run_state : std::uint8_t { idle, running, done, failed, cancelled };
[[nodiscard]] const char* run_state_name(run_state s) noexcept;

struct run_view {
  run_state state = run_state::idle;
  std::uint64_t id = 0;
  photo_target tool = photo_target::detail;
  double fraction = -1;
  std::string eta;          // "1 min left", or "" while it cannot tell
  bool downloading = false;  // the run waits for its model (download, then run)
  std::string model_name;   // "Sharpen & restore" while downloading
  std::string result_path;  // done: the new file (absolute) or a library key
  std::string result_name;  // done: its name as shown ("" for a library photo)
  std::string message;      // failed / cancelled: why; done: a restorer's note
};

// "Enhancing…", "Restyling…", "Restoring faces…", "Removing…", "Extending…".
[[nodiscard]] const char* run_verb(photo_target t) noexcept;
// The tool bar's line for the run.
[[nodiscard]] std::string run_line(const run_view& r);
// "1 min left", "Less than a minute left", "About 2 h left"; "" until a few
// percent are done.
[[nodiscard]] std::string time_left_short(double fraction, double elapsed_s);
[[nodiscard]] std::string cards_tree(std::span<const card_view> cards, const run_view& run);

// Restyle's quality at a performance level (quality over speed, owner
// 2026-11-04): the person's choice while the level allows it; with none
// chosen, or one above the level, the highest the level allows (Default ->
// High). `values` are a restyle section's; another kind's spec is left alone.
void level_quality(std::map<std::string, json::value>& values, const model_spec& spec, tier level);

// The Video Editor's cut, in the sheet's words ("The edit from the Video
// Editor: 3 pieces, 1:24 in all. …"); "" for the whole clip.
[[nodiscard]] std::string edit_text(std::span<const std::int64_t> keep_ns);

// The targets a clip offers, from what it holds.
[[nodiscard]] std::vector<clip_target> targets_for(bool has_picture, bool has_sound);

// "About 12 minutes left" from how far a run is and how long it has taken;
// "" until a few percent are done (an early guess is a wrong one).
[[nodiscard]] std::string time_left(double fraction, double elapsed_s);

}  // namespace mv::enhance
