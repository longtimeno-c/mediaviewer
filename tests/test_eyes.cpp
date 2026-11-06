// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 6: eyes open from a neighbouring frame.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "edit/eyes.h"
#include "image/half.h"

namespace {

namespace edit = mv::edit;
namespace image = mv::image;
using Catch::Approx;

struct face {
  float cx, cy;   // between the eyes, px
  float d;        // eye distance, px
  float turn;     // radians
  bool open;
  float light;    // exposure of the whole frame
};

// Skin, two eyes on a line: open = white almond with a dark iris; shut = a
// thin line of lashes.
image::linear_image draw(std::uint32_t w, std::uint32_t h, const face& f) {
  image::linear_image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  const float c = std::cos(f.turn), s = std::sin(f.turn);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      float r = 0.45f, g = 0.30f, b = 0.22f;  // skin
      for (float side : {-0.5f, 0.5f}) {
        const float ex = f.cx + c * side * f.d, ey = f.cy + s * side * f.d;
        const float ux = (static_cast<float>(x) - ex) * c + (static_cast<float>(y) - ey) * s;
        const float uy = -(static_cast<float>(x) - ex) * s + (static_cast<float>(y) - ey) * c;
        if (f.open) {
          const float q = (ux / (0.25f * f.d)) * (ux / (0.25f * f.d)) + (uy / (0.11f * f.d)) * (uy / (0.11f * f.d));
          if (q < 1.0f) r = g = b = 0.75f;  // white
          if (ux * ux + uy * uy < (0.09f * f.d) * (0.09f * f.d)) {
            r = 0.05f;
            g = 0.04f;
            b = 0.03f;  // iris
          }
        } else if (std::abs(uy) < 0.012f * f.d + 0.5f && std::abs(ux) < 0.25f * f.d) {
          r = 0.06f;
          g = 0.05f;
          b = 0.05f;  // lashes
        }
      }
      std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = image::float_to_half(r * f.light);
      p[1] = image::float_to_half(g * f.light);
      p[2] = image::float_to_half(b * f.light);
      p[3] = image::float_to_half(1.0f);
    }
  }
  return img;
}

edit::face_landmarks marks(const image::linear_image& img, const face& f) {
  const float c = std::cos(f.turn), s = std::sin(f.turn);
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  edit::face_landmarks lm{};
  lm[0] = (f.cx - c * 0.5f * f.d) / w;
  lm[1] = (f.cy - s * 0.5f * f.d) / h;
  lm[2] = (f.cx + c * 0.5f * f.d) / w;
  lm[3] = (f.cy + s * 0.5f * f.d) / h;
  return lm;
}

float red_at(const image::linear_image& img, int x, int y) {
  return image::half_to_float(img.rgba[(static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4]);
}

}  // namespace

TEST_CASE("eyes: openness tells open from shut for the same face", "[eyes]") {
  const face shut{200, 150, 120, 0.0f, false, 1.0f};
  const face open{200, 150, 120, 0.0f, true, 1.0f};
  const auto a = draw(400, 300, shut), b = draw(400, 300, open);
  const float so = edit::eye_openness(a, marks(a, shut));
  const float oo = edit::eye_openness(b, marks(b, open));
  INFO("shut " << so << " open " << oo);
  CHECK(oo > so * edit::kEyesBetter);
  // A tilted, darker frame of the open face still scores open.
  const face tilt{210, 140, 110, 0.12f, true, 0.7f};
  const auto c = draw(400, 300, tilt);
  CHECK(edit::eye_openness(c, marks(c, tilt)) > so * edit::kEyesBetter);
}

TEST_CASE("eyes: a shut pair takes the donor's, aligned, colour-matched, nothing else moves",
          "[eyes]") {
  const face shut{200, 150, 120, 0.0f, false, 1.0f};
  // The next frame: the head a little turned, nearer, and the light brighter.
  const face donor_face{230, 140, 132, 0.08f, true, 1.25f};
  auto target = draw(400, 300, shut);
  const auto before = target;
  const auto donor = draw(400, 300, donor_face);
  REQUIRE(edit::transfer_eyes(target, marks(target, shut), donor, marks(donor, donor_face)));
  const float now = edit::eye_openness(target, marks(target, shut));
  const float was = edit::eye_openness(before, marks(before, shut));
  INFO("was " << was << " now " << now);
  CHECK(now > was * edit::kEyesBetter);
  // Each iris landed on its eye's centre (dark), its white beside it.
  CHECK(red_at(target, 140, 150) < 0.12f);
  CHECK(red_at(target, 260, 150) < 0.12f);
  // The donor's brighter light was matched to this frame's skin: the skin
  // just inside the ellipse stays near this frame's.
  CHECK(red_at(target, 140, 172) == Approx(0.45f).epsilon(0.1));  // skin inside the ellipse, below the white
  CHECK(red_at(target, 140, 162) == Approx(0.75f).epsilon(0.1));  // the white, at this frame's light
  // Away from the eyes nothing changed.
  CHECK(red_at(target, 200, 250) == red_at(before, 200, 250));
  CHECK(red_at(target, 20, 20) == red_at(before, 20, 20));
  // Faces too small to carry eyes are refused.
  const face tiny{50, 50, 8, 0.0f, false, 1.0f};
  auto t2 = draw(100, 100, tiny);
  CHECK_FALSE(edit::transfer_eyes(t2, marks(t2, tiny), donor, marks(donor, donor_face)));
}
