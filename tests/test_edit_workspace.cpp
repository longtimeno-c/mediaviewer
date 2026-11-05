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

// ---- Extend's ratio (owner, 2026-11-05: "allow to set aspect ratios") -----------

namespace {
// The result's pixels for a draft over a 3000 x 2000 (3:2) picture.
std::array<std::uint32_t, 2> extend_px(const mv::shell::extend_draft& d, std::uint32_t w = 3000,
                                       std::uint32_t h = 2000) {
  const mv::shell::extend_size_t s = mv::shell::extend_size(d, w, h);
  return {s.width, s.height};
}
}  // namespace

TEST_CASE("extend_to_aspect: each of Crop's ratios grows a 3:2 photo evenly", "[edit_workspace][extend]") {
  using namespace mv::shell;
  const std::uint32_t w = 3000, h = 2000;
  extend_draft none;
  none.on = true;
  const auto grown = [&](crop_aspect a, bool portrait = false) {
    return extend_to_aspect(none, w, h, crop_aspect_pixels(a, portrait, w, h));
  };
  // Free gives the frame back; Original and 3:2 are already the photo's.
  for (crop_aspect a : {crop_aspect::free, crop_aspect::original, crop_aspect::r3_2}) {
    const extend_aspect_fit f = grown(a);
    CHECK(f.reached);
    CHECK(extend_empty(f.draft));
    CHECK(f.draft.on);
  }
  // 1:1 grows the short axis, half above and half below.
  extend_aspect_fit f = grown(crop_aspect::square);
  CHECK(f.reached);
  CHECK(f.draft.t == Catch::Approx(0.25f));
  CHECK(f.draft.b == Catch::Approx(0.25f));
  CHECK(f.draft.l == 0.0f);
  CHECK(f.draft.r == 0.0f);
  CHECK(extend_px(f.draft) == std::array<std::uint32_t, 2>{3000, 3000});
  // 4:3 and 5:4 are taller than 3:2: they grow down and up.
  f = grown(crop_aspect::r4_3);
  CHECK(f.reached);
  CHECK(f.draft.t == Catch::Approx(0.0625f));
  CHECK(extend_px(f.draft) == std::array<std::uint32_t, 2>{3000, 2250});
  f = grown(crop_aspect::r5_4);
  CHECK(f.reached);
  CHECK(extend_px(f.draft) == std::array<std::uint32_t, 2>{3000, 2400});
  // 16:9 is wider: left and right, the chip's sums to the pixel.
  f = grown(crop_aspect::r16_9);
  CHECK(f.reached);
  CHECK(f.draft.l == Catch::Approx(f.draft.r));
  CHECK(f.draft.t == 0.0f);
  CHECK(extend_px(f.draft)[1] == 2000u);
  CHECK(extend_px(f.draft)[0] >= 3555u);
  CHECK(extend_px(f.draft)[0] <= 3556u);
  CHECK(extend_at_aspect(f.draft, w, h, 16.0 / 9.0));
  CHECK_FALSE(extend_at_aspect(f.draft, w, h, 1.5));
  // Only ever bigger: a frame already wider than 1:1 asks for more height.
  extend_draft wide = none;
  wide.l = wide.r = 0.1f;  // 3600 x 2000
  f = extend_to_aspect(wide, w, h, 1.0);
  CHECK(f.draft.l == Catch::Approx(0.1f));
  CHECK(f.draft.t == Catch::Approx(0.4f));  // 3600 high: (3600 - 2000) / 2 / 2000
  CHECK(f.reached);
  // The words are Crop's.
  CHECK(std::string(crop_aspect_label(crop_aspect::r5_4)) == "5:4");
  CHECK(crop_aspect_turns(crop_aspect::r4_3));
  CHECK_FALSE(crop_aspect_turns(crop_aspect::square));
  CHECK_FALSE(crop_aspect_turns(crop_aspect::free));
}

TEST_CASE("extend_to_aspect: the swap turns it; a stop or a cap says the ratio is out of reach",
          "[edit_workspace][extend]") {
  using namespace mv::shell;
  const std::uint32_t w = 3000, h = 2000;
  extend_lock lock;
  extend_draft frame;
  frame.on = true;
  // 5:4, then turned (Crop's X): 4:5 grows from the photo, not from 5:4's frame.
  CHECK(extend_choose_aspect(lock, frame, crop_aspect::r5_4, false, w, h));
  CHECK(extend_px(frame) == std::array<std::uint32_t, 2>{3000, 2400});
  CHECK(extend_turn_aspect(lock, frame, w, h));
  CHECK(lock.portrait);
  CHECK(lock.aspect == crop_aspect::r5_4);
  CHECK(frame.t == Catch::Approx(0.4375f));
  CHECK(extend_px(frame) == std::array<std::uint32_t, 2>{3000, 3750});  // 4:5
  CHECK(frame.on);
  // Trying 16:9 after 4:5 does not pile one on the other either.
  CHECK(extend_choose_aspect(lock, frame, crop_aspect::r16_9, false, w, h));
  CHECK(frame.t == 0.0f);
  CHECK(extend_px(frame)[1] == 2000u);
  // 9:16 needs 5333 px of height from 2000: past the 50 % stop. The frame
  // goes as far as it can and says it fell short; it never claims the ratio.
  CHECK_FALSE(extend_choose_aspect(lock, frame, crop_aspect::r16_9, true, w, h));
  CHECK(frame.t == Catch::Approx(kExtendMax));
  CHECK(frame.b == Catch::Approx(kExtendMax));
  CHECK_FALSE(extend_at_aspect(frame, w, h, extend_lock_ratio(lock, w, h)));
  // Free and 1:1 do not turn.
  extend_lock sq;
  extend_draft f2;
  CHECK(extend_choose_aspect(sq, f2, crop_aspect::square, false, w, h));
  CHECK_FALSE(extend_turn_aspect(sq, f2, w, h));
  CHECK(extend_choose_aspect(sq, f2, crop_aspect::free, false, w, h));
  CHECK(sq.aspect == crop_aspect::free);
  CHECK(extend_px(f2) == std::array<std::uint32_t, 2>{3000, 3000});  // Free keeps the frame
  CHECK_FALSE(extend_turn_aspect(sq, f2, w, h));
  // A side the stop holds passes the rest to the other: the ratio is still met.
  extend_draft lop;
  lop.l = 0.45f;
  lop.t = lop.b = 0.2f;  // 4350 x 2800
  const extend_aspect_fit spill = extend_to_aspect(lop, w, h, 16.0 / 9.0);
  CHECK(spill.reached);
  CHECK(spill.draft.l == Catch::Approx(kExtendMax));
  CHECK(1.0f + spill.draft.l + spill.draft.r == Catch::Approx(16.0f / 9.0f * 2800.0f / 3000.0f));
  // Enhance's 100 MP cap: 9000 x 9000 to 4:3 wants 12000 x 9000 (108 MP),
  // which Enhance would shrink off the ratio. Said, not hidden.
  const extend_aspect_fit capped = extend_to_aspect(extend_draft{}, 9000, 9000, 4.0 / 3.0);
  CHECK(capped.draft.l == Catch::Approx(1.0f / 6.0f));
  CHECK_FALSE(capped.reached);
}

TEST_CASE("extend_drag_aspect: edges and corners keep the ratio; Free is the free drag",
          "[edit_workspace][extend]") {
  using namespace mv::shell;
  const std::uint32_t w = 3000, h = 2000;
  const double original = crop_aspect_pixels(crop_aspect::original, false, w, h);
  CHECK(original == Catch::Approx(1.5));
  extend_draft d;
  d.on = true;
  // The right edge out by 20 %: the height follows, half above, half below.
  extend_draft a = extend_drag_aspect(d, extend_right, 0.2f, 0.0f, w, h, original);
  CHECK(a.r == Catch::Approx(0.2f));
  CHECK(a.l == 0.0f);
  CHECK(a.t == Catch::Approx(0.1f));
  CHECK(a.b == Catch::Approx(0.1f));
  CHECK(a.on);
  CHECK(extend_at_aspect(a, w, h, original));
  // The bottom edge: the width follows on both sides.
  a = extend_drag_aspect(d, extend_bottom, 0.0f, 0.3f, w, h, original);
  CHECK(a.b == Catch::Approx(0.3f));
  CHECK(a.l == Catch::Approx(0.15f));
  CHECK(a.r == Catch::Approx(0.15f));
  // A corner keeps the opposite corner and follows the pointer on the
  // ratio's diagonal, so a pull along one axis still resizes.
  a = extend_drag_aspect(d, extend_right | extend_bottom, 0.3f, 0.0f, w, h, original);
  CHECK(a.l == 0.0f);
  CHECK(a.t == 0.0f);
  CHECK(a.r == Catch::Approx(a.b));
  CHECK(a.r > 0.15f);
  CHECK(extend_at_aspect(a, w, h, original));
  // Held to 1:1 from the square frame, the right edge dragged far: the frame
  // stops where the top and bottom reach the 50 % stop, still square.
  extend_lock lock;
  extend_draft sq = d;
  REQUIRE(extend_choose_aspect(lock, sq, crop_aspect::square, false, w, h));
  a = extend_drag_aspect(sq, extend_right, 0.9f, 0.0f, w, h, extend_lock_ratio(lock, w, h));
  CHECK(a.t == Catch::Approx(kExtendMax));
  CHECK(a.b == Catch::Approx(kExtendMax));
  CHECK(a.r == Catch::Approx(1.0f / 3.0f));
  CHECK(extend_px(a) == std::array<std::uint32_t, 2>{4000, 4000});
  // Inward too: the corner crops, still square.
  a = extend_drag_aspect(sq, extend_left | extend_top, 0.3f, 0.3f, w, h, 1.0);
  CHECK(a.l < 0.0f);
  CHECK(a.t < 0.0f);
  CHECK(a.r == 0.0f);
  CHECK(a.b == Catch::Approx(0.25f));
  CHECK(extend_at_aspect(a, w, h, 1.0));
  // Never past keeping 5 % of the photo either way.
  a = extend_drag_aspect(sq, extend_left | extend_top, 3.0f, 3.0f, w, h, 1.0);
  CHECK(extend_crop(a)[2] >= kExtendMinKeep - 1e-5f);
  CHECK(extend_crop(a)[3] >= kExtendMinKeep - 1e-5f);
  CHECK((1.0f + a.l + a.r) * w == Catch::Approx((1.0f + a.t + a.b) * h));
  // From a frame short of its ratio (9:16 past the stop), a drag lands on the
  // ratio as soon as it can: here the sides come in.
  extend_draft tall = d;
  tall.t = tall.b = kExtendMax;
  a = extend_drag_aspect(tall, extend_bottom, 0.0f, 0.1f, w, h, 9.0 / 16.0);
  CHECK(a.b == Catch::Approx(kExtendMax));
  CHECK(a.l == Catch::Approx(-0.125f));
  CHECK(a.r == Catch::Approx(-0.125f));
  CHECK(extend_at_aspect(a, w, h, 9.0 / 16.0));
  // A move keeps the size; Free (0) is exactly the free drag.
  const extend_draft moved = extend_drag_aspect(sq, extend_move, 0.1f, 0.0f, w, h, 1.0);
  CHECK(1.0f + moved.l + moved.r == Catch::Approx(1.0f + sq.l + sq.r));
  for (std::uint8_t handle : {std::uint8_t{extend_left}, std::uint8_t{extend_bottom},
                              std::uint8_t{extend_right | extend_top}, std::uint8_t{extend_move}}) {
    const extend_draft free_a = extend_drag_aspect(sq, handle, 0.13f, -0.07f, w, h, 0.0);
    const extend_draft free_b = extend_drag(sq, handle, 0.13f, -0.07f);
    CHECK(extend_same_frame(free_a, free_b));
  }
  // NaN from a degenerate map leaves the frame where it was.
  CHECK(extend_same_frame(extend_drag_aspect(sq, extend_right, std::nanf(""), 0.0f, w, h, 1.0), sq));
}

TEST_CASE("extend_lock: the chips set the ratio held; a dragged frame grows from itself",
          "[edit_workspace][extend]") {
  using namespace mv::shell;
  const std::uint32_t w = 3000, h = 2000;
  extend_lock lock;
  extend_draft frame;
  frame.on = true;
  // The add-on's 16:9 chip holds 16:9: its frame, and drags keep it.
  extend_choose_preset(lock, frame, 5, w, h);
  CHECK(lock.aspect == crop_aspect::r16_9);
  CHECK_FALSE(lock.portrait);
  CHECK(extend_preset_of(frame, w, h) == 5);
  CHECK(frame.on);
  // 4:5 is 5:4 turned.
  extend_choose_preset(lock, frame, 6, w, h);
  CHECK(lock.aspect == crop_aspect::r5_4);
  CHECK(lock.portrait);
  // Then 1:1 from the menu starts from the photo, not on top of 4:5.
  CHECK(extend_choose_aspect(lock, frame, crop_aspect::square, false, w, h));
  CHECK(extend_px(frame) == std::array<std::uint32_t, 2>{3000, 3000});
  // Wider is free again, the swap kept for the next ratio.
  lock.portrait = true;
  extend_choose_preset(lock, frame, 1, w, h);
  CHECK(lock.aspect == crop_aspect::free);
  CHECK(lock.portrait);
  CHECK(extend_preset_of(frame, w, h) == 1);
  // Held to 3:2 after a drag, a new ratio grows the dragged frame.
  CHECK(extend_choose_aspect(lock, frame, crop_aspect::r3_2, false, w, h));
  const extend_draft dragged =
      extend_drag_aspect(frame, extend_right, 0.05f, 0.0f, w, h, extend_lock_ratio(lock, w, h));
  frame = dragged;
  CHECK(extend_choose_aspect(lock, frame, crop_aspect::square, false, w, h));
  CHECK(frame.l == Catch::Approx(dragged.l));
  CHECK(frame.r == Catch::Approx(dragged.r));
  CHECK(frame.t > dragged.t);
  CHECK(extend_at_aspect(frame, w, h, 1.0));
  // The bar's wire values for the chips and the ratios do not meet.
  static_assert(kExtendChipCount <= kExtendPresetCount);
}
