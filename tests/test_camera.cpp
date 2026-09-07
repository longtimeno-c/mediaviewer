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

TEST_CASE("zoom out floors at 50 percent and recentres", "[canvas]") {
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

  // Back out to the rest pose. This image fits at 0.2, so the floor is fit, not
  // 50 %: the wheel has to be able to return to the view it opened on. It used
  // to stop at 0.5 here — two and a half times the opening size, with the whole
  // image no longer on screen and no way back except the Fit command.
  cam.wheel_toward(100.0f, 80.0f, -40.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(opening, 1e-5f));
  REQUIRE_THAT(cam.target_pan_x(), WithinAbs(2000.0f, 1e-3f));
  REQUIRE_THAT(cam.target_pan_y(), WithinAbs(1500.0f, 1e-3f));
  // Bottoming out on fit IS fit mode, so a later resize re-fits.
  REQUIRE(cam.fit_mode());
}

TEST_CASE("zoom out floors at 50 percent when the image is small enough to fit",
          "[canvas]") {
  // The other half of the same rule, and the one that must not change: this
  // image fits at 4x, so 50 % is well below fit and the wheel may letterbox
  // down to it.
  camera cam;
  cam.fit(200.0f, 150.0f, 800.0f, 600.0f, true);
  REQUIRE_THAT(cam.zoom(), WithinAbs(4.0f, 1e-5f));

  cam.wheel_toward(400.0f, 300.0f, -40.0f, 800.0f, 600.0f, 200.0f, 150.0f);
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(0.5f, 1e-5f));
  REQUIRE_FALSE(cam.fit_mode());
}

TEST_CASE("a 4K clip can always be zoomed back out to the whole frame", "[canvas]") {
  // The reported bug, in the shape it was reported: a 4K clip in a windowed
  // canvas fits below 50 %, so wheeling in and back out has to return to the
  // whole frame rather than stopping half-way and leaving the viewer clipped in.
  camera cam;
  const float w = 1600.0f, h = 900.0f;
  cam.fit(3840.0f, 2160.0f, w, h, true);
  const float opening = cam.zoom();
  REQUIRE(opening < 0.5f);

  for (int i = 0; i < 6; ++i) cam.wheel_toward(800.0f, 450.0f, 1.0f, w, h, 3840.0f, 2160.0f);
  REQUIRE(cam.target_zoom() > opening);

  for (int i = 0; i < 40; ++i) cam.wheel_toward(800.0f, 450.0f, -1.0f, w, h, 3840.0f, 2160.0f);
  // Wheeling to the floor leaves the rubber band dipped below the rest pose;
  // it pops back once the wheel stops, so settle before reading the result.
  for (int i = 0; i < 240; ++i) cam.step(1.0f / 60.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(opening, 0.002f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(1920.0f, 0.05f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(1080.0f, 0.05f));
}

TEST_CASE("zoom out past fit rubber-bands then pops back", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  const float opening = cam.zoom();

  cam.wheel_toward(400.0f, 300.0f, -1.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(opening, 1e-5f));
  REQUIRE(cam.target_zoom() < opening);
  REQUIRE(cam.target_zoom() > opening * 0.84f);
  REQUIRE(cam.fit_mode());
  REQUIRE(cam.moving());

  for (int i = 0; i < 8; ++i) cam.step(1.0f / 60.0f);
  REQUIRE(cam.zoom() < opening);

  for (int i = 0; i < 180; ++i) cam.step(1.0f / 60.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(opening, 0.002f));
  REQUIRE_THAT(cam.pan_x(), WithinAbs(2000.0f, 0.05f));
  REQUIRE_THAT(cam.pan_y(), WithinAbs(1500.0f, 0.05f));
  REQUIRE_FALSE(cam.moving());
}

TEST_CASE("rubber-band zoom out has a hard floor", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  const float opening = cam.zoom();
  for (int i = 0; i < 16; ++i)
    cam.wheel_toward(400.0f, 300.0f, -1.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(opening, 1e-5f));
  REQUIRE(cam.target_zoom() >= opening * 0.84f - 1e-4f);
  REQUIRE(cam.target_zoom() <= opening);
  REQUIRE(cam.fit_mode());
}

TEST_CASE("held zoom-out does not teleport displayed zoom", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  const float opening = cam.zoom();
  cam.wheel_toward(400.0f, 300.0f, -1.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  const float after_first = cam.zoom();
  REQUIRE_THAT(after_first, WithinAbs(opening, 1e-6f));
  cam.wheel_toward(400.0f, 300.0f, -1.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(after_first, 1e-6f));
  cam.step(1.0f / 60.0f);
  const float after_step = cam.zoom();
  cam.wheel_toward(400.0f, 300.0f, -1.0f, 800.0f, 600.0f, 4000.0f, 3000.0f);
  REQUIRE_THAT(cam.zoom(), WithinAbs(after_step, 1e-6f));
  REQUIRE(cam.target_zoom() < opening);
}

TEST_CASE("set_zoom 50 percent works even when smaller than fit", "[canvas]") {
  camera cam;
  cam.fit(1000.0f, 1000.0f, 800.0f, 600.0f, true);
  REQUIRE(cam.zoom() > 0.5f);
  cam.set_zoom(0.5f, 1000.0f, 1000.0f, 800.0f, 600.0f);
  REQUIRE_FALSE(cam.fit_mode());
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("set_zoom will not go below 50 percent", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  cam.one_to_one();
  cam.set_zoom(0.1f, 4000.0f, 3000.0f, 800.0f, 600.0f);
  REQUIRE_FALSE(cam.fit_mode());
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("set_zoom presets keep the centre", "[canvas]") {
  camera cam;
  cam.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  cam.set_zoom(2.0f, 4000.0f, 3000.0f, 800.0f, 600.0f);
  REQUIRE_FALSE(cam.fit_mode());
  REQUIRE_THAT(cam.target_zoom(), WithinAbs(2.0f, 1e-5f));
  REQUIRE_THAT(cam.target_pan_x(), WithinAbs(2000.0f, 1e-3f));
  REQUIRE_THAT(cam.target_pan_y(), WithinAbs(1500.0f, 1e-3f));
}

TEST_CASE("fit to a chrome-inset window is not the full client", "[canvas]") {
  camera full;
  camera inset;
  full.fit(4000.0f, 3000.0f, 800.0f, 600.0f, true);
  inset.fit(4000.0f, 3000.0f, 800.0f, 552.0f, true);  // 48 DIP bar at 96 dpi
  REQUIRE(inset.zoom() < full.zoom());
  REQUIRE_THAT(inset.zoom(), WithinAbs(552.0f / 3000.0f, 1e-5f));
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
