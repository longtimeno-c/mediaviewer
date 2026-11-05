// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 6: eyes open from a neighbouring frame.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
  // The white at this frame's light (0.75), well under the donor's brighter
  // white (about 0.94); the grain and the glint knee move it a little, and
  // the toolchains' float contraction differs, so a band rather than 10 %.
  CHECK(red_at(target, 140, 162) > 0.65f);
  CHECK(red_at(target, 140, 162) < 0.87f);
  // Away from the eyes nothing changed.
  CHECK(red_at(target, 200, 250) == red_at(before, 200, 250));
  CHECK(red_at(target, 20, 20) == red_at(before, 20, 20));
  // Faces too small to carry eyes are refused.
  const face tiny{50, 50, 8, 0.0f, false, 1.0f};
  auto t2 = draw(100, 100, tiny);
  CHECK_FALSE(edit::transfer_eyes(t2, marks(t2, tiny), donor, marks(donor, donor_face)));
}

TEST_CASE("eyes: pose distance is 0 for one face anywhere and grows as the head turns", "[eyes]") {
  // Right eye, left eye, nose, mouth right, mouth left (0..1 of a 1000 x 800 image).
  const mv::edit::face_landmarks front{0.40f, 0.40f, 0.50f, 0.40f, 0.45f, 0.4625f, 0.42f, 0.525f, 0.48f, 0.525f};
  // The same face moved, scaled and the image a different size: the same pose.
  mv::edit::face_landmarks moved{};
  for (int i = 0; i < 10; i += 2) {
    moved[i] = (front[i] * 1000.0f * 0.5f + 100.0f) / 2000.0f;
    moved[i + 1] = (front[i + 1] * 800.0f * 0.5f + 50.0f) / 1000.0f;
  }
  CHECK(mv::edit::pose_distance(front, 1000, 800, moved, 2000, 1000) < 0.01f);
  // Turned: the nose and mouth slide toward one eye.
  mv::edit::face_landmarks turned = front;
  turned[4] += 0.03f;
  turned[6] += 0.03f;
  turned[8] += 0.03f;
  const float d = mv::edit::pose_distance(front, 1000, 800, turned, 1000, 800);
  CHECK(d > mv::edit::kPoseMatch);
  // Tilted (rolled) only: the frame follows the eye line, so it still matches.
  mv::edit::face_landmarks rolled{};
  const float a = 0.3f, cx = 450.0f, cy = 360.0f;
  for (int i = 0; i < 10; i += 2) {
    const float x = front[i] * 1000.0f - cx, y = front[i + 1] * 800.0f - cy;
    rolled[i] = (cx + std::cos(a) * x - std::sin(a) * y) / 1000.0f;
    rolled[i + 1] = (cy + std::sin(a) * x + std::cos(a) * y) / 800.0f;
  }
  CHECK(mv::edit::pose_distance(front, 1000, 800, rolled, 1000, 800) < 0.01f);
}

namespace {
// All five points (the nose and mouth too), for the whole-face transfer.
mv::edit::face_landmarks five(const mv::image::linear_image& img, const face& f) {
  mv::edit::face_landmarks lm = marks(img, f);
  const float c = std::cos(f.turn), s = std::sin(f.turn);
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  const auto put = [&](int i, float ux, float uy) {
    lm[2 * i] = (f.cx + c * ux * f.d - s * uy * f.d) / w;
    lm[2 * i + 1] = (f.cy + s * ux * f.d + c * uy * f.d) / h;
  };
  put(2, 0.0f, 0.55f);    // nose
  put(3, -0.35f, 1.0f);   // mouth corners
  put(4, 0.35f, 1.0f);
  return lm;
}
}  // namespace

TEST_CASE("faces: the whole face comes from another photo, in this photo's light, at its pose only",
          "[eyes][face]") {
  // This photo: shut, dim, small. The donor: open, twice as bright, larger and
  // a little tilted (another day).
  const face here{200, 140, 60, 0.0f, false, 0.6f};
  const face there{330, 230, 96, 0.12f, true, 1.2f};
  mv::image::linear_image target = draw(400, 320, here);
  const mv::image::linear_image donor = draw(640, 480, there);
  const mv::image::linear_image before = target;
  const auto t = five(target, here), d = five(donor, there);
  const float shut = mv::edit::eye_openness(target, t);
  REQUIRE(mv::edit::transfer_face(target, t, donor, d));
  CHECK(mv::edit::eye_openness(target, t) > shut * mv::edit::kEyesBetter);
  // The skin keeps this photo's light (the donor's is twice as bright).
  const auto red = [](const mv::image::linear_image& img, int x, int y) {
    return mv::image::half_to_float(img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4]);
  };
  CHECK(red(target, 200, 175) == Approx(red(before, 200, 175)).margin(0.06));
  // Outside the face, nothing moved.
  CHECK(red(target, 20, 20) == red(before, 20, 20));
  CHECK(red(target, 380, 300) == red(before, 380, 300));
  // A donor turned away (its nose and mouth slid to one side) is refused.
  auto turned = d;
  for (int i = 4; i < 10; i += 2) turned[i] += 0.04f;
  mv::image::linear_image again = before;
  CHECK_FALSE(mv::edit::transfer_face(again, t, donor, turned));
}

// docs/plans/edit-refresh.md S8: the Faces panel offers Open eyes only on a
// face whose eyes look shut, with no donor to compare against.
TEST_CASE("eyes: a shut pair reads as shut on its own; open, tilted, dark or tiny ones do not", "[eyes]") {
  const face shut{200, 150, 120, 0.0f, false, 1.0f};
  const face open{200, 150, 120, 0.0f, true, 1.0f};
  const face tilt{210, 140, 110, 0.12f, true, 0.7f};
  const face dark_shut{200, 150, 90, 0.08f, false, 0.4f};
  const auto a = draw(400, 300, shut), b = draw(400, 300, open), c = draw(400, 300, tilt), d = draw(400, 300, dark_shut);
  CHECK(edit::eyes_look_shut(a, marks(a, shut)));
  CHECK_FALSE(edit::eyes_look_shut(b, marks(b, open)));
  CHECK_FALSE(edit::eyes_look_shut(c, marks(c, tilt)));
  CHECK(edit::eyes_look_shut(d, marks(d, dark_shut)));
  // Too small to tell (eyes under 8 px apart), or plain skin: not shut.
  const face tiny{200, 150, 6, 0.0f, false, 1.0f};
  const auto t = draw(400, 300, tiny);
  CHECK_FALSE(edit::eyes_look_shut(t, marks(t, tiny)));
  image::linear_image none;
  CHECK_FALSE(edit::eyes_look_shut(none, marks(a, shut)));
}

TEST_CASE("eyes: a face's edit region holds its box and the whole-face ellipse, inside the picture", "[eyes]") {
  // Eyes 100 px apart at (200, 150) in 400 x 300, nose 50 px below.
  edit::face_landmarks lm{150.f / 400, 150.f / 300, 250.f / 400, 150.f / 300, 200.f / 400, 200.f / 300,
                          165.f / 400, 230.f / 300, 235.f / 400, 230.f / 300};
  const std::array<float, 4> box{140.f / 400, 100.f / 300, 120.f / 400, 160.f / 300};
  const auto r = edit::face_region(box, lm, 400, 300);
  // The box, grown kFaceRegionGrow each side...
  CHECK(r[0] <= box[0] - edit::kFaceRegionGrow * box[2] + 1e-6f);
  CHECK(r[0] + r[2] >= box[0] + box[2] * (1 + edit::kFaceRegionGrow) - 1e-6f);
  // ...and the ellipse: half width 1.05 eye distances about the nose.
  const float cx = 200.f + (150.f - 200.f) * 0.0f;  // the nose is level with the eyes' middle in x
  CHECK(r[0] * 400 <= cx - 105.f + 0.5f);
  CHECK((r[0] + r[2]) * 400 >= cx + 105.f - 0.5f);
  CHECK(r[1] * 300 <= 200.f + (150.f - 200.f) * 0.1f - 125.f + 0.5f);
  // Clamped to the picture.
  CHECK(r[0] >= 0.0f);
  CHECK(r[1] >= 0.0f);
  CHECK(r[0] + r[2] <= 1.0f + 1e-6f);
  CHECK(r[1] + r[3] <= 1.0f + 1e-6f);
  // With no picture size, the grown box alone.
  const auto g = edit::face_region(box, lm, 0, 0);
  CHECK(g[0] == Approx(box[0] - edit::kFaceRegionGrow * box[2]));
  CHECK(g[2] == Approx(box[2] * (1 + 2 * edit::kFaceRegionGrow)));
}
