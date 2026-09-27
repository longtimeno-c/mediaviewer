// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Preview → full without a visible pop (plan/04 step 4): the pure parts.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>

#include "canvas/camera.h"
#include "canvas/refinement.h"

using Catch::Matchers::WithinAbs;
using mv::canvas::camera;
using mv::canvas::classify_publish;
using mv::canvas::crossfade;
using mv::canvas::image_quality;
using mv::canvas::publish_identity;
using mv::canvas::publish_kind;

namespace {
std::uint64_t key(const char* s) { return mv::canvas::item_key_for(s, std::strlen(s)); }
}  // namespace

TEST_CASE("a second publish of the same item and view is a refinement", "[canvas][refine]") {
  const publish_identity preview{key("C:/dump/IMG_0001.CR2"), 7, image_quality::preview};
  publish_identity full = preview;
  full.quality = image_quality::full;

  REQUIRE(classify_publish(false, {}, preview) == publish_kind::new_item);
  REQUIRE(classify_publish(true, preview, full) == publish_kind::refinement);
  // Top level, then the same image with its mips: still the same item.
  publish_identity top = preview;
  top.quality = image_quality::full_top;
  REQUIRE(classify_publish(true, top, full) == publish_kind::refinement);
  REQUIRE(classify_publish(true, full, full) == publish_kind::refinement);
}

TEST_CASE("navigation is never a refinement, even back to the same file", "[canvas][refine]") {
  const publish_identity a{key("C:/dump/a.jpg"), 3, image_quality::full};
  publish_identity b{key("C:/dump/b.jpg"), 4, image_quality::preview};
  REQUIRE(classify_publish(true, a, b) == publish_kind::new_item);

  // Right then Left lands on `a` again under a newer generation: a new view.
  publish_identity a_again{key("C:/dump/a.jpg"), 5, image_quality::preview};
  REQUIRE(classify_publish(true, a, a_again) == publish_kind::new_item);

  // No identity (0) is always treated as navigation.
  publish_identity anon{0, 3, image_quality::full};
  REQUIRE(classify_publish(true, a, anon) == publish_kind::new_item);
  REQUIRE(classify_publish(true, anon, a) == publish_kind::new_item);
}

TEST_CASE("a slow preview landing after the full decode is dropped", "[canvas][refine]") {
  const publish_identity full{key("x.nef"), 9, image_quality::full};
  const publish_identity preview{key("x.nef"), 9, image_quality::preview};
  REQUIRE(classify_publish(true, full, preview) == publish_kind::stale);
  const publish_identity top{key("x.nef"), 9, image_quality::full_top};
  REQUIRE(classify_publish(true, full, top) == publish_kind::stale);
}

TEST_CASE("item keys are stable and distinguish paths", "[canvas][refine]") {
  REQUIRE(key("a.jpg") == key("a.jpg"));
  REQUIRE(key("a.jpg") != key("b.jpg"));
  REQUIRE(mv::canvas::item_key_for("", 0) != 0);
}

TEST_CASE("the fade runs 80 ms on real time, smoothstep, then ends", "[canvas][refine]") {
  crossfade f;
  REQUIRE_FALSE(f.active(0.0));
  REQUIRE(f.alpha(0.0) == 1.0f);  // no fade: fully the new texture

  f.begin(10.0);
  REQUIRE(f.active(10.0));
  REQUIRE(f.alpha(10.0) == 0.0f);
  REQUIRE_THAT(f.alpha(10.040), WithinAbs(0.5f, 1e-4f));
  REQUIRE(f.alpha(10.020) < 0.25f);  // eased in, not linear
  REQUIRE(f.alpha(10.020) > 0.0f);
  REQUIRE(f.active(10.079));
  REQUIRE_FALSE(f.active(10.081));
  REQUIRE(f.alpha(10.2) == 1.0f);
  // Monotonic.
  float prev = 0.0f;
  for (int i = 0; i <= 80; ++i) {
    const float a = f.alpha(10.0 + i * 0.001);
    REQUIRE(a >= prev);
    prev = a;
  }
  f.cancel();
  REQUIRE_FALSE(f.active(10.01));
}

TEST_CASE("a large brightness step between preview and full fades longer", "[canvas][refine]") {
  using mv::canvas::refine_fade_seconds;
  REQUIRE(refine_fade_seconds(120, 120) == mv::canvas::k_refine_fade_seconds);
  REQUIRE(refine_fade_seconds(120, 126) == mv::canvas::k_refine_fade_seconds);
  // The RAW agent's measured range: 7..41 levels brighter.
  REQUIRE(refine_fade_seconds(100, 107) > mv::canvas::k_refine_fade_seconds);
  REQUIRE(refine_fade_seconds(100, 141) == mv::canvas::k_refine_fade_max_seconds);
  REQUIRE(refine_fade_seconds(141, 100) == mv::canvas::k_refine_fade_max_seconds);
  REQUIRE(refine_fade_seconds(100, 120) < refine_fade_seconds(100, 130));

  crossfade f;
  f.begin(1.0, 0.25);
  REQUIRE(f.active(1.2));
  REQUIRE_THAT(f.alpha(1.125), WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("refine keeps a zoomed, panned view across a 4x preview → full", "[canvas][refine]") {
  camera cam;
  // Preview 1500x1000 opened at fit, then the user went to 100 % of the
  // preview and panned while the full decode was running.
  cam.fit(1500.0f, 1000.0f, 800.0f, 600.0f, true);
  cam.set_zoom(1.0f, 1500.0f, 1000.0f, 800.0f, 600.0f);
  for (int i = 0; i < 120; ++i) cam.step(1.0f / 60.0f);
  cam.drag_begin();
  cam.drag_delta(-100.0f, -50.0f);
  cam.drag_end();
  REQUIRE_FALSE(cam.fit_mode());
  const float old_pan_x = cam.pan_x();
  const float old_pan_y = cam.pan_y();
  const float old_zoom = cam.zoom();

  // A screen point maps to the same fraction of the picture before and after.
  const auto screen_of = [](float img_x, float pan, float zoom) { return (img_x - pan) * zoom; };
  const float before = screen_of(900.0f, old_pan_x, old_zoom);

  cam.refine(1500.0f, 1000.0f, 6000.0f, 4000.0f, 800.0f, 600.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(old_zoom / 4.0f, 1e-5f));
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(old_zoom / 4.0f, 1e-5f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(old_pan_x * 4.0f, 1e-2f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(old_pan_y * 4.0f, 1e-2f));
  REQUIRE_THAT(screen_of(3600.0f, cam.pan_x(), cam.zoom()), WithinAbs(before, 1e-2f));
  REQUIRE_FALSE(cam.fit_mode());
  REQUIRE_FALSE(cam.moving());  // nothing to settle: it did not jump
}

TEST_CASE("refine at fit stays fitted with no visible jump", "[canvas][refine]") {
  camera cam;
  // Embedded RAW JPEG 6000x4000, LibRaw frame 6024x4024.
  cam.fit(6000.0f, 4000.0f, 1200.0f, 800.0f, true);
  const float shown_w = 6000.0f * cam.zoom();
  cam.refine(6000.0f, 4000.0f, 6024.0f, 4024.0f, 1200.0f, 800.0f);
  REQUIRE(cam.fit_mode());
  REQUIRE_THAT(cam.zoom(), WithinAbs(camera::fit_zoom(6024.0f, 4024.0f, 1200.0f, 800.0f), 1e-6f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(3012.0f, 1e-3f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(2012.0f, 1e-3f));
  // On-screen size moves by well under a percent.
  REQUIRE_THAT(6024.0f * cam.zoom(), WithinAbs(shown_w, shown_w * 0.01f));

  // CR3: a 1620x1080 preview refined by a 5400x3600 frame (3.33x).
  camera cr3;
  cr3.fit(1620.0f, 1080.0f, 1200.0f, 800.0f, true);
  const float cr3_shown = 1620.0f * cr3.zoom();
  cr3.refine(1620.0f, 1080.0f, 5400.0f, 3600.0f, 1200.0f, 800.0f);
  REQUIRE(cr3.fit_mode());
  REQUIRE_THAT(5400.0f * cr3.zoom(), WithinAbs(cr3_shown, 1e-2f));
}

TEST_CASE("refine does not cancel a spring already in flight", "[canvas][refine]") {
  camera cam;
  cam.fit(1000.0f, 1000.0f, 500.0f, 500.0f, true);
  cam.wheel_toward(250.0f, 250.0f, 3.0f, 500.0f, 500.0f, 1000.0f, 1000.0f);
  cam.step(1.0f / 120.0f);
  REQUIRE(cam.moving());
  const float target = cam.target_zoom();
  cam.refine(1000.0f, 1000.0f, 2000.0f, 2000.0f, 500.0f, 500.0f);
  REQUIRE(cam.moving());
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(target / 2.0f, 1e-5f));
  for (int i = 0; i < 240; ++i) cam.step(1.0f / 60.0f);
  REQUIRE_FALSE(cam.moving());
  REQUIRE_THAT(cam.zoom(), WithinAbs(target / 2.0f, 1e-3f));
}

TEST_CASE("refine ignores degenerate sizes", "[canvas][refine]") {
  camera cam;
  cam.fit(100.0f, 100.0f, 50.0f, 50.0f, true);
  const float z = cam.zoom();
  cam.refine(0.0f, 100.0f, 200.0f, 200.0f, 50.0f, 50.0f);
  REQUIRE(cam.zoom() == z);
}
