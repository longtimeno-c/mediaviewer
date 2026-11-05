// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S3: canvas tools (shell/canvas_tool.h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "image/half.h"
#include "image/linear.h"
#include "shell/canvas_tool.h"
#include "shell/tool_overlay.h"

using namespace mv::shell;
using Catch::Approx;

namespace {

mv::edit::placement plain(std::uint32_t w, std::uint32_t h) {
  return mv::edit::place(mv::edit::geometry{}, mv::edit::size2{w, h});
}

mv::image::linear_image solid(std::uint32_t w, std::uint32_t h, float r, float g, float b) {
  mv::image::linear_image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(std::size_t{w} * h * 4);
  for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
    img.rgba[i * 4 + 0] = mv::image::float_to_half(r);
    img.rgba[i * 4 + 1] = mv::image::float_to_half(g);
    img.rgba[i * 4 + 2] = mv::image::float_to_half(b);
    img.rgba[i * 4 + 3] = mv::image::float_to_half(1.0f);
  }
  return img;
}

void paint_disc(mv::image::linear_image& img, float cx, float cy, float r, float R, float G, float B) {
  for (std::uint32_t y = 0; y < img.height; ++y) {
    for (std::uint32_t x = 0; x < img.width; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
      if (dx * dx + dy * dy > r * r) continue;
      std::uint16_t* p = img.rgba.data() + (std::size_t{y} * img.width + x) * 4;
      p[0] = mv::image::float_to_half(R);
      p[1] = mv::image::float_to_half(G);
      p[2] = mv::image::float_to_half(B);
    }
  }
}

}  // namespace

TEST_CASE("canvas tool: each tool opens its home tab, unfolded", "[canvas-tool]") {
  tool_state s;
  edit_workspace ws;
  CHECK(arm_tool(s, ws, canvas_tool::remove_brush));
  CHECK(s.tool == canvas_tool::remove_brush);
  CHECK(ws.open);
  CHECK(ws.tab == edit_tab::enhance);
  CHECK_FALSE(arm_tool(s, ws, canvas_tool::remove_brush));  // already there
  ws.collapsed = true;
  CHECK(arm_tool(s, ws, canvas_tool::red_eye));
  CHECK(ws.tab == edit_tab::colour);
  CHECK_FALSE(ws.collapsed);
  CHECK(home_tab(canvas_tool::wb_pick) == edit_tab::colour);
  CHECK(home_tab(canvas_tool::faces) == edit_tab::enhance);
  // Extend lives on the Enhance tab (its card), not Crop (owner 2026-11-05).
  CHECK(home_tab(canvas_tool::extend) == edit_tab::enhance);
  s.pan_held = s.erase_held = true;
  CHECK_FALSE(arm_tool(s, ws, canvas_tool::none));  // putting it down leaves the pane
  CHECK(s.tool == canvas_tool::none);
  CHECK_FALSE(s.pan_held);
  CHECK_FALSE(s.erase_held);
  CHECK(ws.open);
}

TEST_CASE("canvas tool: adjust_pick's wire values map onto tools", "[canvas-tool]") {
  for (const adjust_pick p : {adjust_pick::none, adjust_pick::white_balance, adjust_pick::red_eye, adjust_pick::remove}) {
    CHECK(pick_of_tool(tool_of_pick(p)) == p);
  }
  CHECK(tool_of_pick(adjust_pick::remove) == canvas_tool::remove_brush);
  CHECK(pick_of_tool(canvas_tool::faces) == adjust_pick::none);
  CHECK(sized(canvas_tool::remove_brush));
  CHECK(sized(canvas_tool::red_eye));
  CHECK_FALSE(sized(canvas_tool::wb_pick));
}

TEST_CASE("canvas tool: [ ] size the ring in whole points, 4 to 400", "[canvas-tool]") {
  CHECK(step_tool_px(40, 1) == 48);
  CHECK(step_tool_px(48, -1) == 40);
  CHECK(step_tool_px(4, -1) == 4);
  CHECK(step_tool_px(5, 1) == 6);    // ×1.2 rounds back to 5: one point anyway
  CHECK(step_tool_px(5, -1) == 4);
  CHECK(step_tool_px(400, 1) == 400);
  CHECK(step_tool_px(390, 1) == 400);
  CHECK(clamp_tool_px(std::nanf("")) == kBrushDefaultPx);
  float px = 40;
  for (int i = 0; i < 40; ++i) px = step_tool_px(px, 1);
  CHECK(px == kToolMaxPx);
  for (int i = 0; i < 60; ++i) px = step_tool_px(px, -1);
  CHECK(px == kToolMinPx);

  tool_state s;
  CHECK_FALSE(set_tool_px(s, 60, 1000));  // no tool, no size
  s.tool = canvas_tool::red_eye;
  CHECK(set_tool_px(s, 60, 1000));
  CHECK(s.redeye_px == 60);
  CHECK(s.brush_px == kBrushDefaultPx);
  CHECK(s.hud_until_ms == 1000 + kToolLabelMs);
  CHECK_FALSE(set_tool_px(s, 60, 2000));  // same size: the label still restarts
  CHECK(s.hud_until_ms == 2000 + kToolLabelMs);
  CHECK(tool_px(s) == 60);
  CHECK(ring_radius_px(48, 2.0f) == 48);
}

TEST_CASE("canvas tool: Alt held inverts the Erase toggle", "[canvas-tool]") {
  tool_state s;
  CHECK_FALSE(erasing(s));
  s.erase_held = true;
  CHECK(erasing(s));
  s.erase = true;
  CHECK_FALSE(erasing(s));
  s.erase_held = false;
  CHECK(erasing(s));
}

TEST_CASE("canvas tool: spots map to the shown picture and back", "[canvas-tool]") {
  const mv::edit::placement p = plain(4000, 3000);
  mv::edit::red_eye_spot s;
  REQUIRE(shown_to_spot(p, 0.25f, 0.5f, 0.01f, s));
  CHECK(s.x == Approx(0.25f));
  CHECK(s.y == Approx(0.5f));
  // 0.01 of the 4000 px side is 40 px, over the 5000 px diagonal.
  CHECK(s.r == Approx(40.0f / 5000.0f));
  const auto back = spot_to_shown(p, s);
  CHECK(back[0] == Approx(0.25f));
  CHECK(back[1] == Approx(0.5f));
  CHECK(back[2] == Approx(0.01f));
  CHECK_FALSE(shown_to_spot(p, -0.1f, 0.5f, 0.01f, s));

  // A crop: the shown picture is the right half, so a spot at source u 0.75
  // is the middle of what is shown.
  mv::edit::geometry g;
  g.crop = {0.5f, 0.0f, 0.5f, 1.0f};
  const mv::edit::placement half = mv::edit::place(g, mv::edit::size2{4000, 3000});
  const auto mid = spot_to_shown(half, {0.75f, 0.5f, 0.01f});
  CHECK(mid[0] == Approx(0.5f).margin(0.01));
  CHECK(mid[1] == Approx(0.5f).margin(0.01));
}

TEST_CASE("canvas tool: a click on a fix finds it; removing it keeps the rest", "[canvas-tool]") {
  const mv::edit::placement p = plain(1000, 1000);
  mv::edit::colour c;
  const std::array<mv::edit::red_eye_spot, 3> add{{{0.2f, 0.2f, 0.01f}, {0.5f, 0.5f, 0.02f}, {0.8f, 0.8f, 0.01f}}};
  c = with_spots(c, add);
  REQUIRE(c.spot_count == 3);
  CHECK(spot_at(p, c, 0.5f, 0.5f, 0.0f) == 1);
  CHECK(spot_at(p, c, 0.52f, 0.5f, 0.0f) == 1);   // inside its 0.028 (diagonal) radius
  CHECK(spot_at(p, c, 0.6f, 0.5f, 0.0f) == -1);
  CHECK(spot_at(p, c, 0.21f, 0.2f, 0.0f) == 0);    // 10 px from a 14 px fix: on it
  CHECK(spot_at(p, c, 0.23f, 0.2f, 0.0f) == -1);   // 30 px: off it...
  CHECK(spot_at(p, c, 0.23f, 0.2f, 0.04f) == 0);  // ...but a minimum hit radius reaches it
  const mv::edit::colour less = without_spot(c, 1);
  REQUIRE(less.spot_count == 2);
  CHECK(less.spots[0] == c.spots[0]);
  CHECK(less.spots[1] == c.spots[2]);
  CHECK(less.spots[2] == mv::edit::red_eye_spot{});
  CHECK(without_spot(c, 7).spot_count == 3);
  // A fix on top of one already there is not doubled; the cap holds.
  CHECK(with_spots(c, std::array<mv::edit::red_eye_spot, 1>{{{0.5f, 0.5f, 0.02f}}}).spot_count == 3);
  std::vector<mv::edit::red_eye_spot> many;
  for (int i = 0; i < 12; ++i) many.push_back({0.05f + 0.07f * static_cast<float>(i), 0.9f, 0.005f});
  CHECK(with_spots(c, many).spot_count == mv::edit::kRedEyeSpots);
}

TEST_CASE("canvas tool: eyes from the pack snap the ring", "[canvas-tool]") {
  const std::string json =
      R"({"faces":[{"x":0.1,"y":0.1,"w":0.3,"h":0.3,"lm":[0.2,0.3,0.3,0.3,0.25,0.35,0.22,0.4,0.28,0.4],"person":0},)"
      R"({"lm":[1,2]},{"x":0}]})";
  const auto eyes = parse_eyes(json, 1000.0f, 1000.0f);
  REQUIRE(eyes.size() == 2);
  CHECK(eyes[0].x == Approx(0.2f));
  CHECK(eyes[1].x == Approx(0.3f));
  // A tenth of 100 px between the eyes, over the 1414 px diagonal.
  CHECK(eyes[0].r == Approx(10.0f / std::sqrt(2.0e6f)));
  CHECK(parse_eyes("not json", 100, 100).empty());
  CHECK(parse_eyes(json, 0, 100).empty());

  std::vector<std::array<float, 3>> shown;
  const mv::edit::placement p = plain(1000, 1000);
  for (const eye_point& e : eyes) shown.push_back(spot_to_shown(p, {e.x, e.y, e.r}));
  // A ring of 0.02 of the side reaches 0.03 from an eye.
  auto snapped = snap_eye(shown, 0.215f, 0.3f, 0.02f, 1.0f);
  REQUIRE(snapped);
  CHECK((*snapped)[0] == Approx(0.2f));
  CHECK((*snapped)[2] == Approx(shown[0][2] * kEyeSnapIris));
  CHECK_FALSE(snap_eye(shown, 0.25f, 0.5f, 0.02f, 1.0f));
  // The nearer eye wins.
  snapped = snap_eye(shown, 0.29f, 0.3f, 0.2f, 1.0f);
  REQUIRE(snapped);
  CHECK((*snapped)[0] == Approx(0.3f));
}

TEST_CASE("canvas tool: Find eyes fixes only the red ones", "[canvas-tool]") {
  auto img = solid(400, 400, 0.4f, 0.3f, 0.25f);
  // Eyes at (100, 100) and (200, 100); only the first pupil is red.
  paint_disc(img, 100.0f, 100.0f, 6.0f, 0.6f, 0.05f, 0.06f);
  paint_disc(img, 200.0f, 100.0f, 6.0f, 0.03f, 0.03f, 0.03f);
  const float diag = std::sqrt(2.0f) * 400.0f;
  const std::array<eye_point, 2> eyes{{{0.25f, 0.25f, 10.0f / diag}, {0.5f, 0.25f, 10.0f / diag}}};
  CHECK(red_ratio(img, eyes[0]) > kRedEyeRatio);
  CHECK(red_ratio(img, eyes[1]) < kRedEyeRatio);
  CHECK(red_ratio(mv::image::linear_image{}, eyes[0]) == 0.0f);
  const auto fixes = find_red_eyes(img, eyes);
  REQUIRE(fixes.size() == 1);
  CHECK(fixes[0].x == Approx(0.25f));
  CHECK(fixes[0].r == Approx(10.0f / diag * kEyeSnapIris));
  // One op: the colour gains both at once.
  const mv::edit::colour c = with_spots(mv::edit::colour{}, fixes);
  CHECK(c.spot_count == 1);
}

TEST_CASE("canvas tool: the overlay's spots and deadlines", "[canvas-tool]") {
  const mv::edit::placement p = plain(1000, 500);
  mv::edit::colour c;
  c = with_spots(c, std::array<mv::edit::red_eye_spot, 2>{{{0.5f, 0.5f, 0.01f}, {0.25f, 0.5f, 0.01f}}});
  edit_view v;
  tool_view_in in;
  in.tool = canvas_tool::red_eye;
  in.ring_px = 12.0f;
  in.hover_spot = 1;
  in.label_until_ms = 5000;
  in.pulse_until_ms = 4000;
  apply_tool(v, in, p, c);
  CHECK(v.tool == static_cast<std::uint8_t>(canvas_tool::red_eye));
  REQUIRE(v.tool_spot_count == 2);
  CHECK(v.tool_spots[1][0] == Approx(0.25f));
  CHECK(v.tool_hover_spot == 1);
  CHECK(tool_hud_until(v) == 5000);
  CHECK(tool_overlay_wanted(v));
  // The brush shows no red-eye fixes.
  in.tool = canvas_tool::remove_brush;
  apply_tool(v, in, p, c);
  CHECK(v.tool_spot_count == 0);
  // Nothing armed and nothing brushed: no overlay, so nothing presents.
  edit_view idle;
  CHECK_FALSE(tool_overlay_wanted(idle));
  CHECK(tool_hud_until(idle) == 0);
}

TEST_CASE("canvas tool: the shown picture's long side on screen", "[canvas-tool]") {
  canvas_map m;
  CHECK(m.screen_long_px() == 0.0f);
  m.valid = true;
  m.zoom = 0.5f;
  m.width = 6000.0f;
  m.height = 4000.0f;
  CHECK(m.screen_long_px() == 3000.0f);
  // A 4 pt ring at 2x is a 4 px radius: 4 / 3000 of the long side.
  CHECK(radius_over_long(ring_radius_px(4.0f, 2.0f), m) == Approx(4.0f / 3000.0f));
}
