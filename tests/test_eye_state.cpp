// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// edit/eye_state: the open/closed eye model in plain C++.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "edit/eye_state.h"
#include "image/linear.h"
#include "io/file.h"

namespace {
using Catch::Approx;
using grey = std::array<float, 32 * 32>;
}  // namespace

TEST_CASE("eye state: the model gives what its ONNX original gives", "[eyes][eye-state]") {
  // Reference values from open_closed_eye.onnx run by OpenCV's DNN on the
  // same 32x32 inputs (tools/eyes/export_eye_state.py's file).
  grey wave{}, band{}, ramp{};
  for (int i = 0; i < 32 * 32; ++i) {
    wave[static_cast<std::size_t>(i)] = 127.0f + 100.0f * std::sin(static_cast<float>(i) * 0.37f);
    band[static_cast<std::size_t>(i)] = (i / 32 >= 13 && i / 32 <= 18) ? 200.0f : 60.0f;
    ramp[static_cast<std::size_t>(i)] = static_cast<float>(i % 32) * 8.0f;
  }
  CHECK(mv::edit::eye_open_probability(wave) == Approx(0.8724037).margin(1e-4));
  CHECK(mv::edit::eye_open_probability(band) == Approx(0.0098225).margin(1e-4));
  CHECK(mv::edit::eye_open_probability(ramp) == Approx(0.9431946).margin(1e-4));
}

TEST_CASE("eye state: nothing to read", "[eyes][eye-state]") {
  mv::image::linear_image none;
  CHECK_FALSE(mv::edit::read_eye_state(none, {0.4f, 0.5f, 0.6f, 0.5f, 0.5f, 0.6f, 0.45f, 0.7f, 0.55f, 0.7f}).known());
  mv::image::linear_image tiny;
  tiny.width = tiny.height = 64;
  tiny.rgba.assign(64 * 64 * 4, 0);
  // Eyes 3 px apart: not read.
  CHECK_FALSE(mv::edit::read_eye_state(tiny, {0.50f, 0.5f, 0.55f, 0.5f, 0.52f, 0.6f, 0.5f, 0.7f, 0.55f, 0.7f}).known());
}

// Real photos: MV_EYE_STATE_LIST names a file of lines "path x0 y0 x1 y1"
// (the eye centres in pixels of the upright decode); prints each eye's reading.
TEST_CASE("eye state: bench on real photos", "[.eye-state-bench]") {
  const char* list = std::getenv("MV_EYE_STATE_LIST");
  if (!list) SKIP("set MV_EYE_STATE_LIST");
  std::ifstream in(list);
  std::string line, last;
  mv::image::linear_image img;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string path;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!(ss >> std::quoted(path) >> x0 >> y0 >> x1 >> y1)) continue;
    if (path != last) {
      const auto bytes = mv::io::read_all(path);
      REQUIRE(bytes);
      auto decoded = mv::image::decode_linear(bytes.value());
      REQUIRE(decoded);
      img = std::move(*decoded);
      last = path;
    }
    const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
    const mv::edit::eye_state s =
        mv::edit::read_eye_state(img, {x0 / w, y0 / h, x1 / w, y1 / h, 0, 0, 0, 0, 0, 0});
    std::printf("%s %.4f %.4f\n", path.c_str(), s.right, s.left);
  }
}
