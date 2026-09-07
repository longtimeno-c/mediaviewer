// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "canvas/camera.h"
#include "canvas/spring.h"

using Catch::Matchers::WithinAbs;
using mv::canvas::camera;
using mv::canvas::spring_step;

TEST_CASE("a critically damped spring settles on its target", "[canvas][spring]") {
  float x = 0.0f;
  float v = 0.0f;
  for (int i = 0; i < 120; ++i) spring_step(x, v, 10.0f, 1.0f / 60.0f);
  REQUIRE_THAT(x, WithinAbs(10.0f, 0.05f));
  REQUIRE_THAT(v, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("fit centres the image and one_to_one is 100 percent", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  REQUIRE_THAT(cam.zoom(), WithinAbs(0.2f, 1e-5f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(2000.0f, 1e-3f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(1500.0f, 1e-3f));
  REQUIRE(cam.fit_mode());

  cam.one_to_one();
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(1.0f, 1e-6f));
  REQUIRE_FALSE(cam.fit_mode());
}

TEST_CASE("wheel zoom keeps the image point under the cursor", "[canvas]") {
  camera cam;
  cam.fit(1000.0f, 1000.0f, 1000.0f, 1000.0f, true);
  REQUIRE_THAT(cam.zoom(), WithinAbs(1.0f, 1e-5f));

  const float mouse_x = 250.0f;
  const float mouse_y = 250.0f;
  const float img_x = cam.pan_x() + (mouse_x - 500.0f) / cam.zoom();
  const float img_y = cam.pan_y() + (mouse_y - 500.0f) / cam.zoom();

  cam.wheel_toward(mouse_x, mouse_y, 1.0f, 1000.0f, 1000.0f, 1000.0f, 1000.0f);

  const float img_x2 = cam.target_pan_x() + (mouse_x - 500.0f) / cam.target_zoom();
  const float img_y2 = cam.target_pan_y() + (mouse_y - 500.0f) / cam.target_zoom();
  REQUIRE_THAT(img_x2, WithinAbs(img_x, 0.01f));
  REQUIRE_THAT(img_y2, WithinAbs(img_y, 0.01f));
}

TEST_CASE("drag pan is direct, not sprung", "[canvas]") {
  camera cam;
  cam.fit(100.0f, 100.0f, 100.0f, 100.0f, true);
  cam.wheel_toward(50.0f, 50.0f, 4.0f, 100.0f, 100.0f, 100.0f, 100.0f);
  REQUIRE_FALSE(cam.fit_mode());
  cam.drag_begin();
  const float before = cam.pan_x();
  cam.drag_delta(10.0f, 0.0f);
  REQUIRE_THAT(cam.pan_x(), WithinAbs(cam.target_pan_x(), 1e-6f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(before - 10.0f / cam.zoom(), 1e-4f));
  cam.drag_end();
}

TEST_CASE("zoom out floors at the opening fit view and recentres", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  const float opening = cam.zoom();
  REQUIRE_THAT(opening, WithinAbs(0.2f, 1e-5f));

  cam.wheel_toward(100.0f, 80.0f, 8.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE(cam.target_zoom() > opening);
  REQUIRE_FALSE(cam.fit_mode());
  cam.drag_begin();
  cam.drag_delta(40.0f, 25.0f);
  cam.drag_end();
  REQUIRE(cam.target_pan_x() != 2000.0f);

  cam.wheel_toward(100.0f, 80.0f, -40.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(opening, 1e-5f));
  REQUIRE_THAT(cam.target_pan_x(), WithinAbs(2000.0f, 1e-3f));
  REQUIRE_THAT(cam.target_pan_y(), WithinAbs(1500.0f, 1e-3f));
  REQUIRE(cam.fit_mode());

  const float zoom_before = cam.target_zoom();
  const float pan_before = cam.target_pan_x();
  cam.wheel_toward(100.0f, 80.0f, -8.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(zoom_before, 1e-6f));
  REQUIRE_THAT(cam.target_pan_x(), WithinAbs(pan_before, 1e-6f));
  REQUIRE(cam.fit_mode());
}

TEST_CASE("the opening view does not pan", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  cam.drag_begin();
  cam.drag_delta(80.0f, 40.0f);
  REQUIRE_THAT(cam.pan_x(), WithinAbs(2000.0f, 1e-3f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(1500.0f, 1e-3f));
  REQUIRE(cam.fit_mode());
  cam.drag_end();
}
