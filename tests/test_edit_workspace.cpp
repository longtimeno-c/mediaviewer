// SPDX-License-Identifier: GPL-3.0-or-later
// shell/edit_workspace.h and the PR 29 rows of the command table (plan/20).
#include "catch_compat.h"

#include <string>

#include "shell/commands.h"
#include "shell/edit_workspace.h"
#include "shell/key_router.h"

using namespace mv::shell;

TEST_CASE("a still offers Crop, Colour and Info; a clip Trim and Jobs", "[edit_workspace][pr29]") {
  CHECK(tabs_for(edit_subject::still).size() == 3);
  CHECK(tabs_for(edit_subject::clip).size() == 2);
  CHECK(tabs_for(edit_subject::none).empty());
  CHECK(default_tab(edit_subject::still) == edit_tab::crop);
  CHECK(default_tab(edit_subject::clip) == edit_tab::trim);
  CHECK(tab_offered(edit_subject::still, edit_tab::info));
  CHECK_FALSE(tab_offered(edit_subject::still, edit_tab::trim));
  CHECK_FALSE(tab_offered(edit_subject::clip, edit_tab::crop));
  CHECK(std::string(workspace_title(edit_subject::still)) == "Edit image");
  CHECK(std::string(workspace_title(edit_subject::clip)) == "Edit video");
}

TEST_CASE("Enter toggles the workspace on its subject's first tab", "[edit_workspace][pr29]") {
  edit_workspace ws;
  auto step = route_workspace(ws, edit_subject::still, command_id::edit_workspace);
  CHECK(step.action == workspace_action::open);
  CHECK(step.tab == edit_tab::crop);
  CHECK(apply_step(ws, step));
  CHECK(ws.open);
  step = route_workspace(ws, edit_subject::still, command_id::edit_workspace);
  CHECK(step.action == workspace_action::close);
  CHECK(apply_step(ws, step));
  CHECK_FALSE(ws.open);
  step = route_workspace(ws, edit_subject::clip, command_id::edit_workspace);
  CHECK(step.tab == edit_tab::trim);
  // Nothing to edit: Enter does nothing, but can still close an open one.
  edit_workspace closed;
  CHECK(route_workspace(closed, edit_subject::none, command_id::edit_workspace).action ==
        workspace_action::none);
  edit_workspace open{true, edit_tab::colour};
  CHECK(route_workspace(open, edit_subject::none, command_id::edit_workspace).action ==
        workspace_action::close);
}

TEST_CASE("the existing edit keys land in the workspace", "[edit_workspace][pr29]") {
  edit_workspace ws;
  // Shift+C, Shift+A and Ctrl+T open it on their tab.
  CHECK(route_workspace(ws, edit_subject::still, command_id::crop_mode).tab == edit_tab::crop);
  CHECK(route_workspace(ws, edit_subject::still, command_id::adjust_pane).action ==
        workspace_action::open);
  CHECK(route_workspace(ws, edit_subject::clip, command_id::trim_mode).tab == edit_tab::trim);
  // ... but not on the wrong subject: the host keeps its old behaviour (a beep).
  CHECK(route_workspace(ws, edit_subject::clip, command_id::crop_mode).action == workspace_action::none);
  CHECK(route_workspace(ws, edit_subject::still, command_id::trim_mode).action == workspace_action::none);
  // Closed, I and Ctrl+J are their own panes, as before.
  CHECK(route_workspace(ws, edit_subject::still, command_id::metadata_pane).action ==
        workspace_action::none);
  CHECK(route_workspace(ws, edit_subject::clip, command_id::jobs_pane).action == workspace_action::none);

  // Open, they switch tabs, and a second press of the tab's own key closes.
  ws = {true, edit_tab::crop};
  auto step = route_workspace(ws, edit_subject::still, command_id::metadata_pane);
  CHECK(step.action == workspace_action::select);
  CHECK(step.tab == edit_tab::info);
  apply_step(ws, step);
  CHECK(route_workspace(ws, edit_subject::still, command_id::metadata_pane).action ==
        workspace_action::close);
  step = route_workspace(ws, edit_subject::still, command_id::adjust_pane);
  CHECK(step.action == workspace_action::select);
  apply_step(ws, step);
  CHECK(route_workspace(ws, edit_subject::still, command_id::adjust_pane).action ==
        workspace_action::close);
  // Shift+C never closes: it starts cropping.
  ws = {true, edit_tab::crop};
  CHECK(route_workspace(ws, edit_subject::still, command_id::crop_mode).action ==
        workspace_action::select);
  ws = {true, edit_tab::trim};
  CHECK(route_workspace(ws, edit_subject::clip, command_id::jobs_pane).tab == edit_tab::jobs);
  CHECK_FALSE(apply_step(ws, {workspace_action::none, edit_tab::crop}));
}

TEST_CASE("walking to another item keeps the workspace on a tab it offers", "[edit_workspace][pr29]") {
  edit_workspace ws{true, edit_tab::colour};
  CHECK_FALSE(follow_subject(ws, edit_subject::still));
  CHECK(ws.tab == edit_tab::colour);
  CHECK(follow_subject(ws, edit_subject::clip));
  CHECK(ws.open);
  CHECK(ws.tab == edit_tab::trim);
  CHECK(follow_subject(ws, edit_subject::still));
  CHECK(ws.tab == edit_tab::crop);
  CHECK(follow_subject(ws, edit_subject::none));
  CHECK_FALSE(ws.open);
  CHECK_FALSE(follow_subject(ws, edit_subject::still));  // closed stays closed
}

TEST_CASE("Enter opens the workspace; crop and trim keep their Enter", "[edit_workspace][keys][pr29]") {
  key_router router;
  view_state s;
  s.item = item_kind::still;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::edit_workspace);
  s.item = item_kind::clip;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::edit_workspace);
  s.trim = true;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::trim_keyframe);
  s = {};
  s.item = item_kind::still;
  s.crop = true;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::crop_commit);
  // A and X pick the aspect in crop; outside it A still walks the folder.
  CHECK(router.on_key({char_key('A'), mod_none}, s).command == command_id::crop_aspect_cycle);
  CHECK(router.on_key({char_key('X'), mod_none}, s).command == command_id::crop_aspect_swap);
  s.crop = false;
  CHECK(router.on_key({char_key('A'), mod_none}, s).command == command_id::prev);
  CHECK(router.on_key({char_key('X'), mod_none}, s).command == command_id::none);
  // Y held shows the original on a still, and lets go on release.
  const route y = router.on_key({char_key('Y'), mod_none}, s);
  CHECK(y.command == command_id::show_original);
  CHECK(router.on_key({char_key('Y'), mod_none, false, true}, s).command == command_id::show_original_release);
  // The gallery keeps its own Enter.
  s.gallery_open = true;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::gallery_open_selected);
}
