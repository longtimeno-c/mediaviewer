// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slice 7: the Remove brush (shell/remove_brush.h). docs/plans/edit-refresh.md
// S4a: brush v2 (a stroke stack, Clear undoable, erase, the caps), the size in
// screen pixels, and the committed strokes as a worker-built tint texture
// (shell/brush_mask.h) with only the live tail in the snapshot.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "shell/brush_mask.h"
#include "shell/canvas_tool.h"
#include "shell/edit_view.h"
#include "shell/remove_brush.h"

using mv::shell::brush_dab;
using mv::shell::brush_mask_builder;
using mv::shell::brush_mask_job;
using mv::shell::brush_mask_pixels;
using mv::shell::brush_mask_sync;
using mv::shell::kBrushDabs;
using mv::shell::kBrushStackDabs;
using mv::shell::kBrushStrokes;
using mv::shell::remove_brush;

namespace {

// image/upload.h k_frame_budget_create_bytes (one frame's texture creates);
// that header pulls in the D3D11 device, so it is mirrored here.
constexpr std::uint64_t kCreateBudgetBytes = 8ull * 1024 * 1024;

// One short stroke across at height v.
void stroke(remove_brush& b, float v, bool erase = false, float r = 0.01f) {
  REQUIRE(b.begin(0.1f, v, r, erase));
  (void)b.drag(0.2f, v);
  b.end();
}

}  // namespace

TEST_CASE("remove brush: strokes spaced evenly, undone one at a time, and a mask of them", "[remove-brush]") {
  remove_brush b;
  b.set_frame(2000, 1000);
  CHECK(b.empty());
  CHECK(b.pristine());
  CHECK_FALSE(b.drag(0.5f, 0.5f));  // no stroke yet
  CHECK_FALSE(b.begin(0.1f, 0.1f, 0.0f));
  CHECK_FALSE(b.begin(0.1f, 0.1f, NAN));
  REQUIRE(b.begin(0.1f, 0.5f, 0.01f));  // radius 20 px
  CHECK(b.stroking());
  // Across 1000 px of the 2000 px side: one dab every half radius, 10 px.
  REQUIRE(b.drag(0.6f, 0.5f));
  b.end();
  CHECK_FALSE(b.stroking());
  CHECK(b.strokes() == 1);
  const std::size_t across = b.dabs().size();
  CHECK(across >= 1 + 99);
  CHECK(across <= 1 + 100);
  CHECK(b.dabs().back().u > 0.59f);
  // Down 500 px of the 1000 px side, the same density in pixels.
  REQUIRE(b.begin(0.9f, 0.0f, 0.01f));
  REQUIRE(b.drag(0.9f, 0.5f));
  b.end();
  CHECK(b.dabs().size() - across >= 1 + 49);
  CHECK(b.dabs().size() - across <= 1 + 50);
  CHECK_FALSE(b.drag(0.9f, 0.9f));  // released: nothing more

  const auto m = b.mask(2000, 1000);
  CHECK(m[500 * 2000 + 600] == 255);   // on the first stroke
  CHECK(m[530 * 2000 + 600] == 0);     // 30 px below it
  CHECK(m[250 * 2000 + 1800] == 255);  // on the second
  CHECK(m[900 * 2000 + 1800] == 0);    // past its end
  CHECK(std::all_of(m.begin(), m.end(), [](std::uint8_t v) { return v == 0 || v == 255; }));
  CHECK(b.mask(0, 0).empty());

  CHECK(b.undo_stroke());
  CHECK(b.dabs().size() == across);
  CHECK(b.undo_stroke());
  CHECK(b.empty());
  CHECK_FALSE(b.pristine());  // two strokes to redo
  CHECK_FALSE(b.undo_stroke());
}

TEST_CASE("remove brush: redo, erase dabs and the hand-off", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 1000);
  REQUIRE(b.begin(0.2f, 0.5f, 0.05f));
  REQUIRE(b.drag(0.8f, 0.5f));
  b.end();
  const std::size_t painted = b.dabs().size();
  CHECK_FALSE(b.can_redo());
  CHECK(b.can_undo());
  const std::uint32_t g0 = b.generation();
  REQUIRE(b.undo_stroke());
  CHECK(b.generation() != g0);  // dabs came off: a mask of them no longer holds
  CHECK(b.can_redo());
  CHECK_FALSE(b.can_undo());
  const std::uint32_t g1 = b.generation();
  REQUIRE(b.redo_stroke());
  CHECK(b.generation() == g1);  // redo only appends
  CHECK(b.dabs().size() == painted);
  CHECK(b.strokes() == 1);
  CHECK_FALSE(b.redo_stroke());

  // An erase stroke over the middle takes it out of the mask and the hand-off.
  REQUIRE(b.begin(0.5f, 0.5f, 0.08f, /*erase=*/true));
  CHECK(b.erasing());
  b.end();
  CHECK_FALSE(b.erasing());
  CHECK(b.dabs().back().erase == 1);
  CHECK(b.dabs().front().erase == 0);
  const auto m = b.mask(1000, 1000);
  CHECK(m[500 * 1000 + 500] == 0);
  CHECK(m[500 * 1000 + 250] == 255);
  const auto flat = b.handoff();
  REQUIRE(flat.size() % 3 == 0);
  CHECK(flat.size() / 3 < painted);
  for (std::size_t i = 0; i < flat.size(); i += 3) CHECK(std::abs(flat[i] - 0.5f) > 0.07f);
  // Painting again after the erase covers the hole again (order counts).
  REQUIRE(b.begin(0.5f, 0.5f, 0.01f));
  b.end();
  CHECK(b.mask(1000, 1000)[500 * 1000 + 500] == 255);
  CHECK(b.handoff().size() == flat.size() + 3);
  REQUIRE(b.undo_stroke());

  // Undoing the erase stroke brings the painted middle back.
  REQUIRE(b.undo_stroke());
  CHECK(b.mask(1000, 1000)[500 * 1000 + 500] == 255);
  CHECK(b.handoff().size() / 3 == painted);

  // A new stroke ends the redo chain.
  REQUIRE(b.begin(0.1f, 0.1f, 0.01f));
  b.end();
  CHECK_FALSE(b.can_redo());
  CHECK_FALSE(b.redo_stroke());
}

TEST_CASE("remove brush: Clear is one undo step", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 500);
  CHECK_FALSE(b.clear());  // nothing to clear: no step
  CHECK_FALSE(b.can_undo());
  stroke(b, 0.2f);
  stroke(b, 0.4f);
  stroke(b, 0.6f);
  const std::size_t dabs = b.dabs().size();
  const std::uint32_t g = b.generation();
  REQUIRE(b.clear());
  CHECK(b.empty());
  CHECK(b.strokes() == 0);
  CHECK(b.generation() != g);
  CHECK(b.can_undo());  // the bar's Undo stays on with nothing brushed
  CHECK_FALSE(b.pristine());
  CHECK_FALSE(b.clear());  // a second Clear of nothing is no step

  // Undo puts every stroke back, one step; redo takes them off again.
  REQUIRE(b.undo_stroke());
  CHECK(b.strokes() == 3);
  CHECK(b.dabs().size() == dabs);
  REQUIRE(b.redo_stroke());
  CHECK(b.empty());
  REQUIRE(b.undo_stroke());
  CHECK(b.strokes() == 3);

  // Strokes after a Clear come off first, then the Clear.
  REQUIRE(b.clear());
  stroke(b, 0.8f);
  CHECK(b.strokes() == 1);
  REQUIRE(b.undo_stroke());
  CHECK(b.empty());
  REQUIRE(b.undo_stroke());
  CHECK(b.strokes() == 3);
  // ... and each of the three, one at a time.
  REQUIRE(b.undo_stroke());
  REQUIRE(b.undo_stroke());
  REQUIRE(b.undo_stroke());
  CHECK(b.empty());
  CHECK_FALSE(b.undo_stroke());

  // A Clear ends the redo chain as a stroke does.
  stroke(b, 0.2f);
  stroke(b, 0.4f);
  REQUIRE(b.undo_stroke());
  CHECK(b.can_redo());
  REQUIRE(b.clear());
  CHECK_FALSE(b.can_redo());

  // Only the newest kBrushClears Clears stay undoable; the stack stays whole.
  remove_brush c;
  c.set_frame(1000, 500);
  for (std::size_t i = 0; i < mv::shell::kBrushClears + 3; ++i) {
    stroke(c, 0.5f);
    REQUIRE(c.clear());
  }
  std::size_t undone = 0;
  while (c.undo_stroke()) ++undone;
  CHECK(undone == 2 * mv::shell::kBrushClears);  // each kept Clear, and the stroke under it
  CHECK(c.empty());

  // Reset (another item, a new geometry, Remove took them) is not undoable.
  b.reset();
  CHECK(b.pristine());
  CHECK_FALSE(b.can_undo());
  CHECK_FALSE(b.can_redo());
}

TEST_CASE("remove brush: the caps (strokes and dabs)", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 1000);
  for (std::size_t i = 0; i < kBrushStrokes; ++i) {
    REQUIRE(b.begin(0.5f, 0.5f, 0.01f));
    b.end();
  }
  CHECK(b.full());
  CHECK(b.strokes() == kBrushStrokes);
  CHECK_FALSE(b.begin(0.5f, 0.5f, 0.01f));  // the host says "Brush full: Remove, then brush more"
  REQUIRE(b.undo_stroke());
  CHECK_FALSE(b.full());
  REQUIRE(b.redo_stroke());
  CHECK(b.full());
  // Undo one, start another: the undone one cannot come back past the cap.
  REQUIRE(b.undo_stroke());
  REQUIRE(b.begin(0.5f, 0.5f, 0.01f));
  b.end();
  CHECK_FALSE(b.redo_stroke());

  // Dabs: a tiny brush across the photo stops at kBrushStackDabs, and says so.
  remove_brush d;
  d.set_frame(1000, 1000);
  REQUIRE(d.begin(0.0f, 0.5f, 0.0001f));
  CHECK_FALSE(d.drag(1.0f, 0.5f));
  d.end();
  CHECK(d.full());
  CHECK(d.dabs().size() == kBrushStackDabs);
  CHECK_FALSE(d.begin(0.5f, 0.5f, 0.01f));
  // More than the snapshot carries: only the newest kBrushDabs travel live.
  CHECK(d.dabs().size() > kBrushDabs);
  REQUIRE(d.clear());
  CHECK_FALSE(d.full());
  REQUIRE(d.undo_stroke());
  CHECK(d.full());
}

TEST_CASE("remove brush: the size is in screen pixels", "[remove-brush]") {
  // A 6000 x 4000 photo at 400 %: its long side is 24000 px on screen.
  mv::shell::canvas_map m;
  m.valid = true;
  m.zoom = 4.0f;
  m.width = 6000.0f;
  m.height = 4000.0f;
  CHECK(m.screen_long_px() == 24000.0f);
  // A 4 px brush (the smallest) is a 2 px radius on screen, at any zoom.
  const float r = mv::shell::radius_over_long(mv::shell::ring_radius_px(4.0f, 1.0f), m);
  CHECK(std::abs(r - 2.0f / 24000.0f) < 1e-9f);
  // On a 2x display the ring is twice the physical pixels, the same points.
  CHECK(std::abs(mv::shell::radius_over_long(mv::shell::ring_radius_px(4.0f, 2.0f), m) - 4.0f / 24000.0f) < 1e-9f);
  // At 25 % the same brush covers 16x the photo's pixels.
  m.zoom = 0.25f;
  CHECK(std::abs(mv::shell::radius_over_long(mv::shell::ring_radius_px(4.0f, 1.0f), m) - 2.0f / 1500.0f) < 1e-7f);
  // No map yet: no radius (begin refuses it).
  m.valid = false;
  CHECK(mv::shell::radius_over_long(20.0f, m) == 0.0f);
  remove_brush b;
  b.set_frame(6000, 4000);
  CHECK_FALSE(b.begin(0.5f, 0.5f, mv::shell::radius_over_long(20.0f, m)));
  // A 2 px radius on a 6000 px photo is under a texel of the 1024 px tint: it
  // still marks the texel its centre is in.
  REQUIRE(b.begin(0.5f, 0.5f, r));
  b.end();
  const auto [w, h] = mv::shell::brush_mask_size(6000, 4000);
  CHECK(w == 1024);
  CHECK(h == 683);
  std::vector<std::uint8_t> cover(std::size_t{w} * h, 0);
  mv::shell::stamp_brush_dabs(b.dabs(), w, h, cover);
  CHECK(std::count(cover.begin(), cover.end(), 255) == 1);
}

TEST_CASE("brush mask: size, stamp and tint", "[remove-brush]") {
  CHECK(mv::shell::brush_mask_size(0, 100) == std::pair<std::uint32_t, std::uint32_t>{0, 0});
  CHECK(mv::shell::brush_mask_size(800, 600) == std::pair<std::uint32_t, std::uint32_t>{800, 600});  // never larger
  CHECK(mv::shell::brush_mask_size(3000, 6000) == std::pair<std::uint32_t, std::uint32_t>{512, 1024});
  // At most 4 MB of RGBA8, inside a frame's create budget.
  const auto [w, h] = mv::shell::brush_mask_size(100000, 100000);
  CHECK(std::uint64_t{w} * h * 4 <= 4ull * 1024 * 1024);
  CHECK(std::uint64_t{w} * h * 4 <= kCreateBudgetBytes);

  // A disc of radius 10 texels at the centre of 64 x 64, an erase hole in it.
  const brush_dab dabs[] = {{0.5f, 0.5f, 10.0f / 64.0f, 0}, {0.5f, 0.5f, 3.0f / 64.0f, 1}};
  std::vector<std::uint8_t> cover(64 * 64, 0);
  mv::shell::stamp_brush_dabs(dabs, 64, 64, cover);
  CHECK(cover[32 * 64 + 32] == 0);   // erased
  CHECK(cover[32 * 64 + 38] == 255);  // painted, outside the hole
  CHECK(cover[32 * 64 + 44] == 0);    // outside the disc
  CHECK(cover[0] == 0);
  // The span fill matches a per-pixel disc test.
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const float px = static_cast<float>(x) + 0.5f - 32.0f, py = static_cast<float>(y) + 0.5f - 32.0f;
      const float d2 = px * px + py * py;
      const bool want = d2 <= 100.0f && d2 > 9.0f;
      if (std::abs(d2 - 100.0f) < 0.5f || std::abs(d2 - 9.0f) < 0.5f) continue;  // the rim: rounding
      CHECK((cover[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)] == 255) == want);
    }
  }
  // Out-of-range dabs do nothing; a short buffer is left alone.
  const brush_dab off[] = {{-2.0f, -2.0f, 0.1f, 0}, {NAN, 0.5f, 0.1f, 0}};
  std::vector<std::uint8_t> blank(64 * 64, 0);
  mv::shell::stamp_brush_dabs(off, 64, 64, blank);
  CHECK(std::all_of(blank.begin(), blank.end(), [](std::uint8_t v) { return v == 0; }));
  std::vector<std::uint8_t> small(10, 0);
  mv::shell::stamp_brush_dabs(dabs, 64, 64, small);
  CHECK(std::all_of(small.begin(), small.end(), [](std::uint8_t v) { return v == 0; }));

  // The tint: the accent at 40 % inside, darker and stronger on the edge,
  // transparent outside (in the edge's colour).
  std::vector<std::uint8_t> rgba;
  mv::shell::tint_brush_mask(cover, 64, 64, 0x0A84FFFFu, rgba);
  REQUIRE(rgba.size() == 64 * 64 * 4);
  const auto at = [&](int x, int y) { return rgba.data() + (static_cast<std::size_t>(y) * 64 + x) * 4; };
  CHECK(at(32, 38)[0] == 0x0A);  // inside: the accent
  CHECK(at(32, 38)[2] == 0xFF);
  CHECK(at(32, 38)[3] == mv::shell::kBrushMaskFillAlpha);
  CHECK(at(32, 32)[3] == 0);  // the erase hole
  CHECK(at(0, 0)[3] == 0);
  CHECK(at(0, 0)[2] == at(32, 22)[2]);  // transparent texels carry the edge's colour
  // The disc's top row (y = 22) is edge.
  CHECK(at(32, 22)[3] == mv::shell::kBrushMaskEdgeAlpha);
  CHECK(at(32, 22)[2] < 0xFF);
}

TEST_CASE("brush mask: the builder adds only new dabs and matches a full build", "[remove-brush]") {
  remove_brush b;
  b.set_frame(4000, 3000);
  REQUIRE(b.begin(0.1f, 0.5f, 0.02f));
  REQUIRE(b.drag(0.9f, 0.5f));
  b.end();
  brush_mask_job j1;
  j1.tag = {.item = 7, .epoch = 1, .gen = b.generation(), .count = static_cast<std::uint32_t>(b.dabs().size()),
            .frame_w = 4000, .frame_h = 3000, .accent = 0xFF8000FFu};
  j1.dabs.assign(b.dabs().begin(), b.dabs().end());
  brush_mask_builder builder;
  brush_mask_pixels p1;
  REQUIRE(builder.build(j1, p1));
  CHECK(p1.width == 1024);
  CHECK(p1.height == 768);
  CHECK(builder.last_stamped() == j1.dabs.size());

  // An erase stroke after it: only its dabs are stamped this time.
  REQUIRE(b.begin(0.5f, 0.2f, 0.03f, true));
  REQUIRE(b.drag(0.5f, 0.8f));
  b.end();
  brush_mask_job j2 = j1;
  j2.tag.count = static_cast<std::uint32_t>(b.dabs().size());
  j2.dabs.assign(b.dabs().begin(), b.dabs().end());
  brush_mask_pixels p2;
  REQUIRE(builder.build(j2, p2));
  CHECK(builder.last_stamped() == j2.dabs.size() - j1.dabs.size());
  brush_mask_builder fresh;
  brush_mask_pixels p3;
  REQUIRE(fresh.build(j2, p3));
  CHECK(fresh.last_stamped() == j2.dabs.size());
  CHECK(p2.rgba == p3.rgba);
  // The erase cut the line: its middle is clear.
  CHECK(p2.rgba[(384 * 1024 + 512) * 4 + 3] == 0);
  CHECK(p2.rgba[(384 * 1024 + 200) * 4 + 3] == mv::shell::kBrushMaskFillAlpha);

  // After an undo (a new generation) it starts over.
  REQUIRE(b.undo_stroke());
  brush_mask_job j3 = j1;
  j3.tag.gen = b.generation();
  brush_mask_pixels p4;
  REQUIRE(builder.build(j3, p4));
  CHECK(builder.last_stamped() == j1.dabs.size());
  CHECK(p4.rgba == p1.rgba);
  // Nothing to draw: no build.
  brush_mask_job none = j1;
  none.tag.count = 0;
  CHECK_FALSE(builder.build(none, p4));
  none = j1;
  none.tag.frame_w = 0;
  CHECK_FALSE(builder.build(none, p4));
}

TEST_CASE("brush mask: the sync builds one at a time and the snapshot carries only the tail", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 1000);
  brush_mask_sync sync;
  auto view = std::make_unique<mv::shell::edit_view>();
  const float cursor[3] = {-1.0f, -1.0f, 0.0f};

  CHECK_FALSE(sync.next(b, 1, 1000, 1000, 0xFFu).has_value());  // nothing brushed
  REQUIRE(b.begin(0.1f, 0.5f, 0.01f));
  REQUIRE(b.drag(0.3f, 0.5f));  // 41 dabs: under the flush
  CHECK_FALSE(sync.next(b, 1, 1000, 1000, 0xFFu).has_value());
  b.end();
  CHECK_FALSE(sync.next(b, 0, 1000, 1000, 0xFFu).has_value());  // no item
  CHECK_FALSE(sync.next(b, 1, 0, 1000, 0xFFu).has_value());     // no frame
  auto job = sync.next(b, 1, 1000, 1000, 0xFFu);
  REQUIRE(job.has_value());
  CHECK(job->tag.count == b.dabs().size());
  CHECK(sync.in_flight());
  CHECK_FALSE(sync.next(b, 1, 1000, 1000, 0xFFu).has_value());  // one at a time
  // In flight is not on screen: every dab still travels live.
  CHECK(sync.tail_from(b) == 0);
  mv::shell::apply_brush(*view, b, sync, true, 1, cursor);
  CHECK(view->brush_count == b.dabs().size());
  CHECK(view->brush_mask == 0);

  sync.landed(job->tag, true);
  CHECK_FALSE(sync.in_flight());
  CHECK(sync.tail_from(b) == b.dabs().size());
  CHECK(sync.mask_on(b));
  CHECK_FALSE(sync.next(b, 1, 1000, 1000, 0xFFu).has_value());  // up to date
  mv::shell::apply_brush(*view, b, sync, true, 2, cursor);
  CHECK(view->brush_count == 0);
  CHECK(view->brush_mask == 1);
  CHECK(view->brush_mask_epoch == sync.epoch());

  // A second stroke: its dabs are the tail until its build lands.
  const std::size_t first = b.dabs().size();
  REQUIRE(b.begin(0.1f, 0.7f, 0.01f));
  REQUIRE(b.drag(0.2f, 0.7f));
  b.end();
  mv::shell::apply_brush(*view, b, sync, true, 3, cursor);
  CHECK(view->brush_count == b.dabs().size() - first);
  CHECK(view->brush[0].v == 0.7f);
  // A new accent rebuilds too.
  job = sync.next(b, 1, 1000, 1000, 0xFFu);
  REQUIRE(job.has_value());
  sync.landed(job->tag, true);
  CHECK(sync.tail_from(b) == b.dabs().size());
  job = sync.next(b, 1, 1000, 1000, 0xFF00FFu);
  REQUIRE(job.has_value());
  sync.landed(job->tag, true);

  // An undo: the texture no longer holds a prefix, so every dab is live (the
  // newest kBrushDabs) until the rebuild lands.
  REQUIRE(b.undo_stroke());
  CHECK(sync.tail_from(b) == 0);
  CHECK(sync.mask_on(b));  // the old texture shows for the build's few ms
  job = sync.next(b, 1, 1000, 1000, 0xFF00FFu);
  REQUIRE(job.has_value());
  CHECK(job->tag.count == first);

  // A failed build is not retried in a loop; a change retries.
  sync.landed(job->tag, false);
  CHECK_FALSE(sync.next(b, 1, 1000, 1000, 0xFF00FFu).has_value());
  CHECK(sync.tail_from(b) == 0);
  REQUIRE(b.begin(0.5f, 0.9f, 0.01f));
  b.end();
  job = sync.next(b, 1, 1000, 1000, 0xFF00FFu);
  REQUIRE(job.has_value());

  // Another item while a build is in flight: its landing is ignored, and the
  // next build waits for it (one job at a time).
  const auto stale = job->tag;
  sync.reset();
  b.reset();
  CHECK_FALSE(sync.mask_on(b));
  REQUIRE(b.begin(0.5f, 0.5f, 0.01f));
  b.end();
  CHECK_FALSE(sync.next(b, 2, 1000, 1000, 0xFFu).has_value());
  sync.landed(stale, true);
  CHECK_FALSE(sync.mask_on(b));
  job = sync.next(b, 2, 1000, 1000, 0xFFu);
  REQUIRE(job.has_value());
  CHECK(job->tag.epoch == sync.epoch());
  sync.landed(job->tag, true);
  CHECK(sync.mask_on(b));
  // Cleared: the host drops the texture.
  REQUIRE(b.clear());
  CHECK_FALSE(sync.mask_on(b));
  sync.dropped();
  REQUIRE(b.undo_stroke());
  CHECK_FALSE(sync.mask_on(b));
  CHECK(sync.next(b, 2, 1000, 1000, 0xFFu).has_value());

  // Mid-stroke: a long stroke flushes every kBrushMaskFlushDabs dabs (fewer
  // while erasing), so the live tail stays short.
  remove_brush l;
  l.set_frame(1000, 1000);
  brush_mask_sync ls;
  REQUIRE(l.begin(0.0f, 0.5f, 0.001f));
  REQUIRE(l.drag(0.1f, 0.5f));  // 201 dabs
  CHECK_FALSE(ls.next(l, 1, 1000, 1000, 0xFFu).has_value());
  REQUIRE(l.drag(0.2f, 0.5f));  // 401
  job = ls.next(l, 1, 1000, 1000, 0xFFu);
  REQUIRE(job.has_value());
  ls.landed(job->tag, true);
  CHECK(ls.tail_from(l) == job->tag.count);
  REQUIRE(l.drag(0.25f, 0.5f));
  CHECK_FALSE(ls.next(l, 1, 1000, 1000, 0xFFu).has_value());
  l.end();
  CHECK(ls.next(l, 1, 1000, 1000, 0xFFu).has_value());  // the stroke ended: build now
  remove_brush e;
  e.set_frame(1000, 1000);
  brush_mask_sync es;
  REQUIRE(e.begin(0.0f, 0.5f, 0.001f, true));
  REQUIRE(e.drag(0.02f, 0.5f));  // 41 erase dabs
  CHECK(es.next(e, 1, 1000, 1000, 0xFFu).has_value());
}

TEST_CASE("brush mask: the snapshot never carries more than kBrushDabs", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 1000);
  REQUIRE(b.begin(0.0f, 0.5f, 0.0001f));
  (void)b.drag(1.0f, 0.5f);
  b.end();
  REQUIRE(b.dabs().size() == kBrushStackDabs);
  brush_mask_sync sync;
  auto view = std::make_unique<mv::shell::edit_view>();
  const float cursor[3] = {-1.0f, -1.0f, 0.0f};
  mv::shell::apply_brush(*view, b, sync, true, 1, cursor);
  CHECK(view->brush_count == kBrushDabs);
  CHECK(view->brush[kBrushDabs - 1].u == b.dabs().back().u);  // the newest
  auto job = sync.next(b, 1, 1000, 1000, 0xFFu);
  REQUIRE(job.has_value());
  sync.landed(job->tag, true);
  mv::shell::apply_brush(*view, b, sync, true, 2, cursor);
  CHECK(view->brush_count == 0);
  CHECK(view->brush_mask == 1);
}

// Hidden (docs/plans/edit-refresh.md S4a): the UI thread's snapshot publish
// with a full brushing (the texture holds it, a live tail of kBrushDabs) stays
// under 0.1 ms, and the worker's tint build is timed. The texture is at most
// 4 MB, which upload.h budgets as one frame's create.
TEST_CASE("perf: the brush's snapshot publish and tint build", "[.perf-bench][remove-brush-bench]") {
  remove_brush b;
  b.set_frame(6000, 4000);
  for (int s = 0; s < 63; ++s) {
    const float v = 0.05f + 0.014f * static_cast<float>(s);
    REQUIRE(b.begin(0.05f, v, 0.02f, s % 7 == 6));
    (void)b.drag(0.95f, v);
    b.end();
  }
  brush_mask_sync sync;
  auto job = sync.next(b, 1, 6000, 4000, 0x0A84FFFFu);
  REQUIRE(job.has_value());
  brush_mask_pixels px;
  double build_best = 1e9;
  for (int run = 0; run < 5; ++run) {
    brush_mask_builder cold;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(cold.build(*job, px));
    build_best = std::min(build_best,
                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  sync.landed(job->tag, true);
  // The stroke in progress: kBrushDabs live on top of the texture.
  REQUIRE(b.begin(0.05f, 0.97f, 0.0001f));
  (void)b.drag(0.95f, 0.97f);
  auto view = std::make_unique<mv::shell::edit_view>();
  const float cursor[3] = {0.5f, 0.5f, 0.004f};
  double publish_best = 1e9;
  for (int run = 0; run < 200; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    mv::shell::apply_brush(*view, b, sync, true, static_cast<std::uint32_t>(run), cursor);
    publish_best = std::min(
        publish_best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  CHECK(view->brush_count <= kBrushDabs);
  CHECK(publish_best < 0.1);
  CHECK(px.rgba.size() <= kCreateBudgetBytes);
  std::printf("{\"bench\":\"brush_publish\",\"best_ms\":%.4f,\"dabs\":%zu}\n", publish_best, b.dabs().size());
  std::printf("{\"bench\":\"brush_mask_build\",\"best_ms\":%.2f,\"bytes\":%zu}\n", build_best, px.rgba.size());
}
