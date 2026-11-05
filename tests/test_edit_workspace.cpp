// SPDX-License-Identifier: GPL-3.0-or-later
// shell/edit_workspace.h and the PR 29 rows of the command table (docs/design/20).
#include "catch_compat.h"
#include <catch2/catch_approx.hpp>

#include <array>
#include <cmath>

#include <string>

#include "shell/commands.h"
#include "shell/edit_workspace.h"
#include "shell/key_router.h"

using namespace mv::shell;

TEST_CASE("a still offers Crop, Adjust, Enhance and Info; a clip Trim and Jobs", "[edit_workspace][pr29]") {
  CHECK(tabs_for(edit_subject::still).size() == 4);
  // The rail's order (docs/plans/edit-refresh.md §1).
  CHECK(tabs_for(edit_subject::still)[0] == edit_tab::crop);
  CHECK(tabs_for(edit_subject::still)[1] == edit_tab::colour);
  CHECK(tabs_for(edit_subject::still)[2] == edit_tab::enhance);
  CHECK(tabs_for(edit_subject::still)[3] == edit_tab::info);
  CHECK(tabs_for(edit_subject::clip).size() == 2);
  CHECK(tabs_for(edit_subject::none).empty());
  CHECK(default_tab(edit_subject::still) == edit_tab::crop);
  CHECK(default_tab(edit_subject::clip) == edit_tab::trim);
  CHECK(tab_offered(edit_subject::still, edit_tab::info));
  CHECK_FALSE(tab_offered(edit_subject::still, edit_tab::trim));
  CHECK_FALSE(tab_offered(edit_subject::clip, edit_tab::crop));
  CHECK(std::string(workspace_title(edit_subject::still)) == "Edit image");
  CHECK(std::string(workspace_title(edit_subject::clip)) == "Edit video");
  // The wire values stay; tab 1 is labelled Adjust now, tab 5 is new.
  CHECK(static_cast<int>(edit_tab::colour) == 1);
  CHECK(static_cast<int>(edit_tab::enhance) == 5);
  CHECK(std::string(tab_label(edit_tab::colour)) == "Adjust");
  CHECK(std::string(tab_label(edit_tab::enhance)) == "Enhance");
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
  // Outside crop, X is issue #207's reject.
  CHECK(router.on_key({char_key('X'), mod_none}, s).command == command_id::reject);
  // Y held shows the original on a still, and lets go on release.
  const route y = router.on_key({char_key('Y'), mod_none}, s);
  CHECK(y.command == command_id::show_original);
  CHECK(router.on_key({char_key('Y'), mod_none, false, true}, s).command == command_id::show_original_release);
  // The gallery keeps its own Enter.
  s.gallery_open = true;
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::gallery_open_selected);
}

TEST_CASE("the Enhance tab is any still's, and follows the subject", "[edit_workspace][edit-refresh]") {
  CHECK(tab_offered(edit_subject::still, edit_tab::enhance));
  CHECK_FALSE(tab_offered(edit_subject::clip, edit_tab::enhance));
  CHECK_FALSE(tab_offered(edit_subject::none, edit_tab::enhance));
  // Selected from the rail, open or closed.
  edit_workspace ws;
  auto step = rail_pick(ws, edit_subject::still, edit_tab::enhance);
  CHECK(step.action == workspace_action::open);
  CHECK(step.tab == edit_tab::enhance);
  CHECK(apply_step(ws, step));
  CHECK(ws.open);
  CHECK(ws.tab == edit_tab::enhance);
  // Another still keeps it; a clip has no Enhance tab (Trim); back on a still, Crop.
  CHECK_FALSE(follow_subject(ws, edit_subject::still));
  CHECK(ws.tab == edit_tab::enhance);
  CHECK(follow_subject(ws, edit_subject::clip));
  CHECK(ws.tab == edit_tab::trim);
  CHECK(follow_subject(ws, edit_subject::still));
  CHECK(ws.tab == edit_tab::crop);
  // A clip's rail offers no Enhance: nothing happens.
  CHECK(rail_pick(ws, edit_subject::clip, edit_tab::enhance).action == workspace_action::none);
  // The keys of the other tabs move off Enhance as from any tab.
  ws = {true, edit_tab::enhance};
  CHECK(route_workspace(ws, edit_subject::still, command_id::adjust_pane).action == workspace_action::select);
  CHECK(route_workspace(ws, edit_subject::still, command_id::metadata_pane).tab == edit_tab::info);
  CHECK(route_workspace(ws, edit_subject::still, command_id::crop_mode).tab == edit_tab::crop);
  // Enter closes it, as any tab.
  CHECK(route_workspace(ws, edit_subject::still, command_id::edit_workspace).action == workspace_action::close);
}

TEST_CASE("the rail's active icon folds the inspector; any icon unfolds it", "[edit_workspace][edit-refresh]") {
  edit_workspace ws{true, edit_tab::colour};
  // Another tab: select.
  auto step = rail_pick(ws, edit_subject::still, edit_tab::info);
  CHECK(step.action == workspace_action::select);
  CHECK(apply_step(ws, step));
  // The active one: collapse, the tab kept.
  step = rail_pick(ws, edit_subject::still, edit_tab::info);
  CHECK(step.action == workspace_action::collapse);
  CHECK(apply_step(ws, step));
  CHECK(ws.open);
  CHECK(ws.collapsed);
  CHECK(ws.tab == edit_tab::info);
  CHECK_FALSE(apply_step(ws, step));  // already folded
  // Folded: the same icon unfolds it, another unfolds on that tab.
  step = rail_pick(ws, edit_subject::still, edit_tab::info);
  CHECK(step.action == workspace_action::select);
  CHECK(apply_step(ws, step));
  CHECK_FALSE(ws.collapsed);
  ws.collapsed = true;
  CHECK(apply_step(ws, rail_pick(ws, edit_subject::still, edit_tab::crop)));
  CHECK_FALSE(ws.collapsed);
  CHECK(ws.tab == edit_tab::crop);
  // A tab's own key unfolds a folded inspector rather than closing it.
  ws = {true, edit_tab::colour, true};
  step = route_workspace(ws, edit_subject::still, command_id::adjust_pane);
  CHECK(step.action == workspace_action::select);
  CHECK(apply_step(ws, step));
  CHECK_FALSE(ws.collapsed);
  ws = {true, edit_tab::info, true};
  CHECK(route_workspace(ws, edit_subject::still, command_id::metadata_pane).action == workspace_action::select);
  // Closing (or the subject going) forgets the fold: the next open shows the inspector.
  ws = {true, edit_tab::crop, true};
  CHECK(apply_step(ws, {workspace_action::close, ws.tab}));
  CHECK_FALSE(ws.collapsed);
  ws = {true, edit_tab::crop, true};
  CHECK(follow_subject(ws, edit_subject::none));
  CHECK_FALSE(ws.collapsed);
  // A closed workspace cannot fold.
  edit_workspace closed;
  CHECK_FALSE(apply_step(closed, {workspace_action::collapse, edit_tab::crop}));
  CHECK_FALSE(closed.open);
}

// ---- docs/plans/edit-refresh.md S6: Extend's frame ----------------------------------

TEST_CASE("extend_draft: edges dragged past the photo stop at 50 % and warn past 25 %", "[edit_workspace][extend]") {
  using namespace mv::shell;
  extend_draft d;
  CHECK(extend_empty(d));
  CHECK_FALSE(extend_overhangs(d));
  // A press on the right edge grabs it; inside grabs the frame; far off nothing.
  CHECK(extend_hit(d, 1.0f, 0.5f, 0.02f, 0.03f) == extend_right);
  CHECK(extend_hit(d, -0.01f, 0.5f, 0.02f, 0.03f) == extend_left);
  CHECK(extend_hit(d, 0.0f, 0.0f, 0.02f, 0.03f) == (extend_left | extend_top));
  CHECK(extend_hit(d, 1.01f, 1.02f, 0.02f, 0.03f) == (extend_right | extend_bottom));
  CHECK(extend_hit(d, 0.5f, 0.5f, 0.02f, 0.03f) == extend_move);
  CHECK(extend_hit(d, 1.5f, 0.5f, 0.02f, 0.03f) == extend_none);

  // Right edge 20 % past the photo: no warning yet.
  extend_draft a = extend_drag(d, extend_right, 0.2f, 0.0f);
  CHECK(a.r == Catch::Approx(0.2f));
  CHECK(extend_overhangs(a));
  CHECK_FALSE(extend_warns(a));
  // 30 %: past the soft warning (owner call E).
  a = extend_drag(d, extend_right, 0.3f, 0.0f);
  CHECK(extend_warns(a));
  // 90 %: held at the hard stop, 50 %.
  a = extend_drag(d, extend_right, 0.9f, 0.0f);
  CHECK(a.r == Catch::Approx(kExtendMax));
  // The left edge dragged left (negative du) grows the left side; the top up.
  a = extend_drag(d, extend_left | extend_top, -0.1f, -0.2f);
  CHECK(a.l == Catch::Approx(0.1f));
  CHECK(a.t == Catch::Approx(0.2f));
  // Into the photo is a crop, never past keeping 5 % of it.
  a = extend_drag(d, extend_left, 2.0f, 0.0f);
  CHECK(a.l == Catch::Approx(kExtendMinKeep - 1.0f));
  CHECK(extend_crops(a));
  CHECK_FALSE(extend_overhangs(a));
  // The frame never shrinks to less than 5 % of the photo either way.
  const std::array<float, 4> c = extend_crop(a);
  CHECK(c[2] == Catch::Approx(kExtendMinKeep));
  // NaN from a degenerate map leaves the draft where it was.
  a = extend_drag(d, extend_right, std::nanf(""), 0.0f);
  CHECK(extend_empty(a));
}

TEST_CASE("extend_draft: a move keeps the frame's size inside the stops", "[edit_workspace][extend]") {
  using namespace mv::shell;
  extend_draft d;
  d.l = 0.1f;
  d.r = 0.1f;
  // Moved right by 0.2: the left side becomes a crop, the right grows.
  extend_draft m = extend_drag(d, extend_move, 0.2f, 0.0f);
  CHECK(m.l == Catch::Approx(-0.1f));
  CHECK(m.r == Catch::Approx(0.3f));
  CHECK(1.0f + m.l + m.r == Catch::Approx(1.2f));
  // Moved far right: the right side stops at 50 %, the size kept.
  m = extend_drag(d, extend_move, 5.0f, 0.0f);
  CHECK(m.r == Catch::Approx(kExtendMax));
  CHECK(1.0f + m.l + m.r == Catch::Approx(1.2f));
  // Down by a lot: the bottom stops at 50 %, the top crops the same, the height kept.
  m = extend_drag(d, extend_move, 0.0f, 3.0f);
  CHECK(m.t == Catch::Approx(-kExtendMax));
  CHECK(m.b == Catch::Approx(kExtendMax));
}

TEST_CASE("extend_draft: a mixed frame splits into a crop and margins of the crop", "[edit_workspace][extend]") {
  using namespace mv::shell;
  extend_draft d;
  d.l = -0.2f;  // 20 % cropped off the left
  d.r = 0.3f;   // 30 % new picture on the right
  d.t = 0.1f;
  const std::array<float, 4> crop = extend_crop(d);
  CHECK(crop[0] == Catch::Approx(0.2f));
  CHECK(crop[1] == Catch::Approx(0.0f));
  CHECK(crop[2] == Catch::Approx(0.8f));
  CHECK(crop[3] == Catch::Approx(1.0f));
  const std::array<float, 4> o = extend_overhang(d);
  CHECK(o[0] == Catch::Approx(0.0f));
  CHECK(o[1] == Catch::Approx(0.1f));
  CHECK(o[2] == Catch::Approx(0.3f / 0.8f));  // of the cropped width
  CHECK(o[3] == Catch::Approx(0.0f));
  // 3000 x 2000: cropped to 2400 x 2000, then 900 right and 200 on top.
  const extend_size_t s = extend_size(d, 3000, 2000);
  CHECK(s.crop_w == 2400u);
  CHECK(s.crop_h == 2000u);
  CHECK(s.right == 900u);
  CHECK(s.top == 200u);
  CHECK(s.width == 3300u);
  CHECK(s.height == 2200u);
  CHECK(s.growth_permille == 210);  // 3300 x 2200 over 3000 x 2000: +21 %
  // The camera frames the photo and the frame with 8 % around them.
  const std::array<float, 4> fit = extend_fit(d);
  CHECK(fit[0] == Catch::Approx(-0.08f * 1.3f));
  CHECK(fit[2] == Catch::Approx(1.3f * 1.16f));
  CHECK(fit[1] == Catch::Approx(-0.1f - 0.08f * 1.1f));
}

TEST_CASE("extend_draft: 3:2 to 16:9 by dragging the sides, and the add-on's presets", "[edit_workspace][extend]") {
  using namespace mv::shell;
  // A 3:2 photo, both sides dragged out to 16:9.
  const std::uint32_t w = 3000, h = 2000;
  const float side = (static_cast<float>(h) * 16.0f / 9.0f - static_cast<float>(w)) / 2.0f / static_cast<float>(w);
  extend_draft d = extend_drag(extend_draft{}, extend_right, side, 0.0f);
  d = extend_drag(d, extend_left, -side, 0.0f);
  const extend_size_t s = extend_size(d, w, h);
  CHECK(s.height == 2000u);
  CHECK(s.width == 3556u);  // 2000 x 16 / 9 = 3555.6
  CHECK(s.growth_permille == 185);
  // The 16:9 chip is the add-on's own sums: 555 more pixels, 277 left, 278 right.
  const extend_draft p = extend_preset_draft(5, w, h);
  const extend_size_t ps = extend_size(p, w, h);
  CHECK(ps.left == 277u);
  CHECK(ps.right == 278u);
  CHECK(ps.width == 3555u);
  CHECK(extend_preset_of(p, w, h) == 5);
  CHECK(extend_preset_of(extend_draft{}, w, h) == 0);
  CHECK(extend_preset_of(d, w, h) == -1);
  CHECK(std::string(extend_preset_label(5)) == "16:9");
  CHECK(std::string(extend_preset_name(5)) == "16x9");
  // Original is no frame; a square of a 3:2 adds 500 above and below.
  CHECK(extend_empty(extend_preset_draft(0, w, h)));
  const extend_size_t sq = extend_size(extend_preset_draft(4, w, h), w, h);
  CHECK(sq.width == 3000u);
  CHECK(sq.height == 3000u);
}

TEST_CASE("extend_size: Enhance's caps, 16384 a side and 100 MP", "[edit_workspace][extend]") {
  using namespace mv::shell;
  extend_draft d;
  d.l = d.r = 0.5f;
  // 12000 wide doubled would be 24000: held to 16384 a side.
  extend_size_t s = extend_size(d, 12000, 1000);
  CHECK(s.width == 16384u);
  // 9000 x 9000 (81 MP) to 13500 x 9000 (121 MP): shrunk evenly to 100 MP.
  s = extend_size(d, 9000, 9000);
  CHECK(std::uint64_t{s.width} * s.height <= kExtendMaxPixels);
  CHECK(std::uint64_t{s.width} * s.height > kExtendMaxPixels - 2 * 9000);  // each margin floored
  CHECK((s.left > s.right ? s.left - s.right : s.right - s.left) <= 1u);
  // A photo already at 100 MP grows nothing.
  s = extend_size(d, 10000, 10000);
  CHECK(s.width == 10000u);
  CHECK(s.height == 10000u);
}
