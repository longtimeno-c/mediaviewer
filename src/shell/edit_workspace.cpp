// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/edit_workspace.h"

#include <algorithm>

namespace mv::shell {
namespace {

// The rail's order (docs/plans/edit-refresh.md §1): Crop, Adjust, Enhance, Info.
constexpr edit_tab kStillTabs[] = {edit_tab::crop, edit_tab::colour, edit_tab::enhance, edit_tab::info};
constexpr edit_tab kClipTabs[] = {edit_tab::trim, edit_tab::jobs};

workspace_step to(const edit_workspace& ws, edit_tab t) noexcept {
  return {ws.open ? workspace_action::select : workspace_action::open, t};
}

}  // namespace

std::span<const edit_tab> tabs_for(edit_subject s) noexcept {
  switch (s) {
    case edit_subject::still: return kStillTabs;
    case edit_subject::clip: return kClipTabs;
    default: return {};
  }
}

bool tab_offered(edit_subject s, edit_tab t) noexcept {
  const auto tabs = tabs_for(s);
  return std::find(tabs.begin(), tabs.end(), t) != tabs.end();
}

edit_tab default_tab(edit_subject s) noexcept {
  return s == edit_subject::clip ? edit_tab::trim : edit_tab::crop;
}

const char* tab_label(edit_tab t) noexcept {
  switch (t) {
    case edit_tab::crop: return "Crop";
    case edit_tab::colour: return "Adjust";  // the wire value keeps its PR 11 name
    case edit_tab::info: return "Info";
    case edit_tab::trim: return "Trim";
    case edit_tab::jobs: return "Jobs";
    case edit_tab::enhance: return "Enhance";
    default: return "";
  }
}

const char* workspace_title(edit_subject s) noexcept {
  return s == edit_subject::clip ? "Edit video" : "Edit image";
}

workspace_step route_workspace(const edit_workspace& ws, edit_subject subject,
                               command_id id) noexcept {
  if (subject == edit_subject::none) {
    // Nothing to edit: only closing is meaningful.
    if (id == command_id::edit_workspace && ws.open) return {workspace_action::close, ws.tab};
    return {};
  }
  const bool still = subject == edit_subject::still;
  switch (id) {
    case command_id::edit_workspace:
      if (ws.open) return {workspace_action::close, ws.tab};
      return {workspace_action::open, default_tab(subject)};
    case command_id::crop_mode:
      if (!still) return {};
      return to(ws, edit_tab::crop);
    case command_id::adjust_pane:
      if (!still) return {};
      // Folded to the rail, Shift+A unfolds Adjust rather than closing.
      if (ws.open && ws.tab == edit_tab::colour && !ws.collapsed) return {workspace_action::close, ws.tab};
      return to(ws, edit_tab::colour);
    case command_id::trim_mode:
      if (still) return {};
      return to(ws, edit_tab::trim);
    case command_id::metadata_pane:
      if (!ws.open || !still) return {};
      if (ws.tab == edit_tab::info && !ws.collapsed) return {workspace_action::close, ws.tab};
      return {workspace_action::select, edit_tab::info};
    case command_id::jobs_pane:
      if (!ws.open || still) return {};
      if (ws.tab == edit_tab::jobs && !ws.collapsed) return {workspace_action::close, ws.tab};
      return {workspace_action::select, edit_tab::jobs};
    default:
      return {};
  }
}

workspace_step rail_pick(const edit_workspace& ws, edit_subject subject, edit_tab t) noexcept {
  if (!tab_offered(subject, t)) return {};
  if (!ws.open) return {workspace_action::open, t};
  if (ws.collapsed) return {workspace_action::select, t};
  if (ws.tab == t) return {workspace_action::collapse, t};
  return {workspace_action::select, t};
}

bool apply_step(edit_workspace& ws, const workspace_step& step) noexcept {
  switch (step.action) {
    case workspace_action::open:
    case workspace_action::select: {
      const bool changed = !ws.open || ws.tab != step.tab || ws.collapsed;
      ws.open = true;
      ws.tab = step.tab;
      ws.collapsed = false;
      return changed;
    }
    case workspace_action::close: {
      const bool changed = ws.open;
      ws.open = false;
      ws.collapsed = false;
      return changed;
    }
    case workspace_action::collapse: {
      if (!ws.open || ws.collapsed) return false;
      ws.collapsed = true;
      return true;
    }
    default:
      return false;
  }
}

bool follow_subject(edit_workspace& ws, edit_subject subject) noexcept {
  if (!ws.open) return false;
  if (subject == edit_subject::none) {
    ws.open = false;
    ws.collapsed = false;
    return true;
  }
  if (tab_offered(subject, ws.tab)) return false;
  ws.tab = default_tab(subject);
  return true;
}

}  // namespace mv::shell
