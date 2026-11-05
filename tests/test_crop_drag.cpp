// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 Behaviour 4: crop mode's handles on the canvas. The hit-test
// both hosts run on a press (shell/crop_drag.h crop_hit), the drag's maths
// (crop_drag_rect) and edit_session::drag_crop (the locked ratio, the
// straightened source's covered area).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "edit/edit_stack.h"
#include "shell/crop_drag.h"
#include "shell/edit_session.h"

namespace {

using mv::edit::rect;
using mv::shell::command_id;
using mv::shell::crop_drag_rect;
using mv::shell::crop_hit;
using mv::shell::edit_effect;
using mv::shell::edit_session;

constexpr std::uint8_t L = mv::shell::extend_left, T = mv::shell::extend_top, R = mv::shell::extend_right,
                       B = mv::shell::extend_bottom, M = mv::shell::extend_move;
constexpr std::uint8_t LT = L | T, RB = R | B, RT = R | T;

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) < eps; }

bool same(const rect& a, const rect& b, float eps = 1e-4f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.w, b.w, eps) && near(a.h, b.h, eps);
}

edit_session cropping(std::uint32_t w = 600, std::uint32_t h = 400) {
  edit_session s;
  (void)s.set_item(mv::shell::edit_item{"/photos/IMG_0002.PNG", 1000, 1, w, h, false});
  REQUIRE(s.run(command_id::crop_mode) == edit_effect::redraw);
  return s;
}

}  // namespace

TEST_CASE("crop handles: edges, corners and the inside of the draft", "[crop-drag]") {
  const rect r{0.2f, 0.2f, 0.6f, 0.6f};
  const float reach = 0.02f;
  CHECK(crop_hit(r, 0.2f, 0.5f, reach, reach) == L);
  CHECK(crop_hit(r, 0.81f, 0.5f, reach, reach) == R);
  CHECK(crop_hit(r, 0.5f, 0.19f, reach, reach) == T);
  CHECK(crop_hit(r, 0.5f, 0.795f, reach, reach) == B);
  CHECK(crop_hit(r, 0.205f, 0.21f, reach, reach) == LT);
  CHECK(crop_hit(r, 0.8f, 0.8f, reach, reach) == RB);
  CHECK(crop_hit(r, 0.79f, 0.2f, reach, reach) == RT);
  CHECK(crop_hit(r, 0.5f, 0.5f, reach, reach) == M);
  // Off the rect, past the reach: the canvas keeps the press (a pan).
  CHECK(crop_hit(r, 0.1f, 0.5f, reach, reach) == mv::shell::extend_none);
  CHECK(crop_hit(r, 0.5f, 0.9f, reach, reach) == mv::shell::extend_none);
  // The full-frame draft crop mode starts with: its edges are the photo's.
  const rect full{0.0f, 0.0f, 1.0f, 1.0f};
  CHECK(crop_hit(full, 0.995f, 0.5f, reach, reach) == R);
  CHECK(crop_hit(full, 0.0f, 0.0f, reach, reach) == LT);
  CHECK(crop_hit(full, 1.01f, 1.01f, reach, reach) == RB);
  // A rect thinner than two reaches: the nearer edge wins.
  const rect thin{0.5f, 0.2f, 0.02f, 0.6f};
  CHECK(crop_hit(thin, 0.505f, 0.5f, reach, reach) == L);
  CHECK(crop_hit(thin, 0.515f, 0.5f, reach, reach) == R);
}

TEST_CASE("crop handles, free: each held edge follows the pointer", "[crop-drag]") {
  const rect start{0.0f, 0.0f, 1.0f, 1.0f};
  CHECK(same(crop_drag_rect(start, R, -0.3f, 0.4f, 0.0f, 0.05f), {0.0f, 0.0f, 0.7f, 1.0f}));
  CHECK(same(crop_drag_rect(start, LT, 0.1f, 0.25f, 0.0f, 0.05f), {0.1f, 0.25f, 0.9f, 0.75f}));
  // Never past the frame, never smaller than the minimum.
  CHECK(same(crop_drag_rect(start, L, -0.5f, 0.0f, 0.0f, 0.05f), start));
  CHECK(same(crop_drag_rect(start, B, 0.0f, -2.0f, 0.0f, 0.05f), {0.0f, 0.0f, 1.0f, 0.05f}));
  // A move keeps the size and stays inside the frame.
  const rect small{0.2f, 0.3f, 0.4f, 0.4f};
  CHECK(same(crop_drag_rect(small, M, 0.1f, -0.1f, 0.0f, 0.05f), {0.3f, 0.2f, 0.4f, 0.4f}));
  CHECK(same(crop_drag_rect(small, M, 5.0f, 5.0f, 0.0f, 0.05f), {0.6f, 0.6f, 0.4f, 0.4f}));
  CHECK(same(crop_drag_rect(small, M, -5.0f, -5.0f, 0.0f, 0.05f), {0.0f, 0.0f, 0.4f, 0.4f}));
  // A non-finite pointer is no travel.
  CHECK(same(crop_drag_rect(small, R, NAN, INFINITY, 0.0f, 0.05f), small));
}

TEST_CASE("crop handles, locked: the ratio holds and the far corner stays", "[crop-drag]") {
  const float k = 1.5f;  // normalised w / h
  const rect start{0.1f, 0.1f, 0.6f, 0.4f};
  // A corner pulled in: the opposite corner (0.1, 0.1) stays and the size is
  // the pointer projected onto the diagonal.
  const rect a = crop_drag_rect(start, RB, -0.3f, -0.05f, k, 0.05f);
  CHECK(near(a.x, 0.1f));
  CHECK(near(a.y, 0.1f));
  CHECK(near(a.w / a.h, k));
  CHECK(near(a.w, (0.3f * k + 0.35f) / (k * k + 1.0f) * k));
  // A pull along one axis alone still shrinks it.
  const rect a2 = crop_drag_rect(start, RB, -0.3f, 0.0f, k, 0.05f);
  CHECK(a2.w < start.w - 0.1f);
  CHECK(near(a2.w / a2.h, k));
  // Pulled out past the frame: as large as the room from the fixed corner allows.
  const rect b = crop_drag_rect(start, RB, 1.0f, 1.0f, k, 0.05f);
  CHECK(near(b.w / b.h, k));
  CHECK(b.x + b.w <= 1.0f + 1e-5f);
  CHECK(b.y + b.h <= 1.0f + 1e-5f);
  CHECK(near(b.x, 0.1f));
  CHECK(near(b.y, 0.1f));
  // A top-left corner keeps the bottom-right one.
  const rect c = crop_drag_rect(start, LT, 0.15f, 0.0f, k, 0.05f);
  CHECK(near(c.x + c.w, 0.7f));
  CHECK(near(c.y + c.h, 0.5f));
  CHECK(near(c.w / c.h, k));
  // An edge resizes the other axis about its centre.
  const rect d = crop_drag_rect(start, R, -0.15f, 0.0f, k, 0.05f);
  CHECK(near(d.x, 0.1f));
  CHECK(near(d.w, 0.45f));
  CHECK(near(d.w / d.h, k));
  CHECK(near(d.y + d.h * 0.5f, 0.3f));
  const rect e = crop_drag_rect(start, T, 0.0f, 0.1f, k, 0.05f);
  CHECK(near(e.y + e.h, 0.5f));
  CHECK(near(e.h, 0.3f));
  CHECK(near(e.w / e.h, k));
  CHECK(near(e.x + e.w * 0.5f, 0.4f));
  // Never below the minimum on either side.
  const rect f = crop_drag_rect(start, RB, -5.0f, -5.0f, k, 0.05f);
  CHECK(f.h >= 0.05f - 1e-6f);
  CHECK(f.w >= 0.05f - 1e-6f);
}

TEST_CASE("drag_crop resizes the draft, and Enter applies what was dragged", "[crop-drag][shell][edit]") {
  edit_session s = cropping();
  const rect start = s.crop_overlay();
  CHECK(same(start, {0.0f, 0.0f, 1.0f, 1.0f}));
  REQUIRE(s.drag_crop(start, RB, -0.25f, -0.5f) == edit_effect::redraw);
  CHECK(same(s.crop_overlay(), {0.0f, 0.0f, 0.75f, 0.5f}));
  // The same drag again changes nothing.
  CHECK(s.drag_crop(start, RB, -0.25f, -0.5f) == edit_effect::none);
  // A move from the new draft.
  const rect placed = s.crop_overlay();
  REQUIRE(s.drag_crop(placed, M, 0.1f, 0.2f) == edit_effect::redraw);
  CHECK(same(s.crop_overlay(), {0.1f, 0.2f, 0.75f, 0.5f}));
  REQUIRE(s.run(command_id::crop_commit) == edit_effect::redraw);
  CHECK(s.preview_placement().cropped == mv::edit::size2{450, 200});
  // Outside crop mode a drag is nothing.
  CHECK(s.drag_crop(placed, R, -0.1f, 0.0f) == edit_effect::none);
}

TEST_CASE("drag_crop keeps a locked preset's ratio", "[crop-drag][shell][edit]") {
  edit_session s = cropping();
  REQUIRE(s.set_crop_aspect(mv::shell::crop_aspect::square, false) == edit_effect::redraw);
  const rect start = s.crop_overlay();
  REQUIRE(s.drag_crop(start, LT, 0.2f, 0.05f) == edit_effect::redraw);
  const rect r = s.crop_overlay();
  CHECK(near(r.w * 600.0f, r.h * 400.0f, 0.05f));  // 1:1 in pixels
  CHECK(near(r.x + r.w, start.x + start.w));
  CHECK(near(r.y + r.h, start.y + start.h));
}

TEST_CASE("drag_crop stops where a straightened corner would show", "[crop-drag][shell][edit]") {
  edit_session s = cropping();
  REQUIRE(s.set_straighten(6.0f) == edit_effect::redraw);
  const rect start = s.crop_overlay();  // the angle's largest fit
  // Pulled outward the fit cannot grow: nothing moves.
  CHECK(s.drag_crop(start, RB, 0.2f, 0.2f) == edit_effect::none);
  // Pulled in, then out again: it grows back no further than the fit allows.
  REQUIRE(s.drag_crop(start, R, -0.2f, 0.0f) == edit_effect::redraw);
  const rect in = s.crop_overlay();
  CHECK(near(in.w, start.w - 0.2f));
  REQUIRE(s.drag_crop(in, R, 0.5f, 0.0f) == edit_effect::redraw);
  const rect out = s.crop_overlay();
  const rect safe = mv::edit::constrain_crop(out, 6.0f, {600, 400});
  CHECK(same(safe, out, 1e-4f));
  CHECK(out.w > in.w);
  CHECK(out.w <= start.w + 1e-3f);
}
