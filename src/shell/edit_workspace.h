// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 — the Edit workspace (docs/design/20): one visible door ("Edit image" /
// "Edit video", Enter) to the edits PRs 10-14 built, as a strip of tabs over
// the right pane column. Shared by both hosts.
//
// Pure and UI-thread only. This decides WHICH tab a command lands on and
// whether the workspace opens or closes; the host then shows that tab's pane
// (Crop and Trim are new panes; Colour, Info and Jobs are the PR 11, PR 9 and
// PR 13 panes) and runs the command's own work exactly as before. No edit
// logic lives here: edit_session and trim_state stay the only owners.
#pragma once

#include <cstdint>
#include <span>

#include "shell/commands.h"

namespace mv::shell {

// Values are stable: both chromes send them back as the tab to show.
enum class edit_tab : std::uint8_t {
  crop = 0,    // still: crop, aspect, straighten, rotate, flip
  colour = 1,  // still: the PR 11 adjust pane
  info = 2,    // still: the PR 9 / 12 metadata pane (rating, comment)
  trim = 3,    // clip: in / out, save, split, clip tools
  jobs = 4,    // clip: the PR 13 job queue
  count
};

enum class edit_subject : std::uint8_t {
  none,   // nothing on the canvas, or something neither side edits (an animation)
  still,
  clip,
};

[[nodiscard]] std::span<const edit_tab> tabs_for(edit_subject s) noexcept;
[[nodiscard]] bool tab_offered(edit_subject s, edit_tab t) noexcept;
[[nodiscard]] edit_tab default_tab(edit_subject s) noexcept;
[[nodiscard]] const char* tab_label(edit_tab t) noexcept;       // "Crop", "Colour", ...
[[nodiscard]] const char* workspace_title(edit_subject s) noexcept;  // "Edit image" / "Edit video"

struct edit_workspace {
  bool open = false;
  edit_tab tab = edit_tab::crop;
};

enum class workspace_action : std::uint8_t {
  none,    // not the workspace's command here: the host does what it always did
  open,    // open on `tab`
  select,  // already open: show `tab`
  close,
};

struct workspace_step {
  workspace_action action = workspace_action::none;
  edit_tab tab = edit_tab::crop;
};

// What `id` does to the workspace for the item on the canvas. The command's
// own effect (start cropping, arm trim, ...) is still the host's to run.
//   edit_workspace  toggles; opens on the subject's first tab
//   crop_mode       still: opens / selects Crop (the host starts cropping)
//   adjust_pane     still: opens / selects Colour; on Colour it closes (Shift+A toggles)
//   trim_mode       clip: opens / selects Trim (the host arms or disarms trim)
//   metadata_pane   only while open on a still: selects Info; on Info it closes
//   jobs_pane       only while open on a clip: selects Jobs; on Jobs it closes
[[nodiscard]] workspace_step route_workspace(const edit_workspace& ws, edit_subject subject,
                                             command_id id) noexcept;

// Applies a step. Returns true when anything changed.
bool apply_step(edit_workspace& ws, const workspace_step& step) noexcept;

// The item on the canvas changed. With nothing editable the workspace closes;
// otherwise it stays open and a tab the new subject lacks becomes its first
// (Crop on a still, Trim on a clip). Returns true when anything changed.
bool follow_subject(edit_workspace& ws, edit_subject subject) noexcept;

}  // namespace mv::shell
