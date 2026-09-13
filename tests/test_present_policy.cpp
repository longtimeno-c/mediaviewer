// SPDX-License-Identifier: GPL-2.0-or-later
// Present-or-idle policy. The Metal lab and the D3D11 lab must agree, so this
// is tested as arithmetic rather than as a GPU soak.

#include <catch2/catch_test_macros.hpp>

#include "gfx/present_policy.h"

TEST_CASE("a still with no input yet is not a fake 500 ms tail", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.has_still = true;
  r.last_input_time = -1.0;
  r.elapsed_seconds = 0.05;
  r.painted_static = false;
  const auto d = mv::gfx::decide_present(r);
  REQUIRE_FALSE(d.pan_tail);
  REQUIRE_FALSE(d.live);
  REQUIRE(d.wants_frame);  // first static paint
}

TEST_CASE("after the first static paint, idle stops presenting", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.has_still = true;
  r.painted_static = true;
  r.last_input_time = -1.0;
  REQUIRE_FALSE(mv::gfx::decide_present(r).wants_frame);
}

TEST_CASE("input starts a 500 ms present tail on a still", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.has_still = true;
  r.painted_static = true;
  r.last_input_time = 1.0;
  r.elapsed_seconds = 1.4;
  const auto d = mv::gfx::decide_present(r);
  REQUIRE(d.pan_tail);
  REQUIRE(d.live);
  REQUIRE(d.wants_frame);

  r.elapsed_seconds = 1.6;
  const auto after = mv::gfx::decide_present(r);
  REQUIRE_FALSE(after.pan_tail);
  REQUIRE_FALSE(after.live);
  REQUIRE_FALSE(after.wants_frame);
}

TEST_CASE("the sweep is live even with no still", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.animating = true;
  r.painted_static = true;
  REQUIRE(mv::gfx::decide_present(r).wants_frame);
  REQUIRE(mv::gfx::decide_present(r).live);
}

TEST_CASE("occlusion and hidden windows do not present", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.animating = true;
  r.occluded = true;
  REQUIRE_FALSE(mv::gfx::decide_present(r).wants_frame);

  r.occluded = false;
  r.window_visible = false;
  REQUIRE_FALSE(mv::gfx::decide_present(r).wants_frame);
}

TEST_CASE("inactive window idles unless a soak is running", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.animating = true;
  r.window_active = false;
  REQUIRE_FALSE(mv::gfx::decide_present(r).wants_frame);

  r.soak = true;
  REQUIRE(mv::gfx::decide_present(r).wants_frame);
}

TEST_CASE("a redraw of a static frame presents once", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.painted_static = true;
  r.redraw = true;
  REQUIRE(mv::gfx::decide_present(r).wants_frame);
  REQUIRE_FALSE(mv::gfx::decide_present(r).live);
}

TEST_CASE("video loading is live so the empty-canvas welcome cannot stick",
          "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.painted_static = true;
  r.video_loading = true;
  const auto d = mv::gfx::decide_present(r);
  REQUIRE(d.live);
  REQUIRE(d.wants_frame);
}

TEST_CASE("camera springs keep presenting until they settle", "[gfx][present_policy]") {
  mv::gfx::present_request r;
  r.has_still = true;
  r.painted_static = true;
  r.camera_moving = true;
  REQUIRE(mv::gfx::decide_present(r).wants_frame);
}

TEST_CASE("constants match plan/03", "[gfx][present_policy]") {
  REQUIRE(mv::gfx::k_input_tail_seconds == 0.5);
  REQUIRE(mv::gfx::k_warmup_seconds == 1.0);
  REQUIRE(mv::gfx::k_occlusion_poll_ms == 200);
}
