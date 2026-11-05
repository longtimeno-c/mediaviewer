// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Restore faces (docs/plans/edit-refresh.md S10, owner call C): the
// alignment to the FFHQ template, the inverse warp, the feathered mask and the
// pipeline with a stand-in model and pack (only the faces change; the People
// guard keeps a face that drifted). The real models run behind hidden tags
// with local copies (never downloaded here):
//
//   MV_ENHANCE_RUNTIME_DIR=<onnxruntime lib dir> MV_FACE_MODEL_DIR=<dir with
//   restoreformer_plus_plus.onnx and/or GFPGANv1.4.onnx> MV_FACE_RGB=<raw RGB>
//   MV_FACE_W=.. MV_FACE_H=.. MV_FACE_LM="10 numbers, pixels"
//   mv_import_tests "[.face-model]"
#include "catch_compat.h"
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "addons/enhance/face_restore.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/spec.h"
#include "core/json.h"
#include "infer/ort.h"
#include "import_fixture.h"

using namespace mv::enhance;
using Catch::Approx;

namespace {

rgb8 noise_rgb8(std::uint32_t w, std::uint32_t h, std::uint32_t seed = 7) {
  rgb8 r;
  r.width = w;
  r.height = h;
  r.rgb.resize(std::size_t{w} * h * 3);
  std::uint32_t s = seed;
  for (auto& v : r.rgb) {
    s = s * 1664525u + 1013904223u;
    v = static_cast<std::uint8_t>(s >> 24);
  }
  return r;
}

// A smooth picture (a gradient): bilinear sampling of it is exact enough to
// compare a round trip.
rgb8 gradient_rgb8(std::uint32_t w, std::uint32_t h) {
  rgb8 r;
  r.width = w;
  r.height = h;
  r.rgb.resize(std::size_t{w} * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = r.rgb.data() + (std::size_t{y} * w + x) * 3;
      p[0] = static_cast<std::uint8_t>(40 + 160 * x / w);
      p[1] = static_cast<std::uint8_t>(40 + 160 * y / h);
      p[2] = 128;
    }
  }
  return r;
}

std::array<float, 10> tpl_at(std::uint32_t side) {
  std::array<float, 10> t{};
  for (std::size_t i = 0; i < 10; ++i) t[i] = kFfhq512[i] * static_cast<float>(side) / 512.0f;
  return t;
}

// The template at `side` carried into a photo by `s` (template -> photo).
std::array<float, 10> landmarks_by(const similarity& s, std::uint32_t side) {
  const auto t = tpl_at(side);
  std::array<float, 10> lm{};
  for (int k = 0; k < 5; ++k) {
    double x = 0, y = 0;
    s.apply(t[2 * k], t[2 * k + 1], x, y);
    lm[2 * k] = static_cast<float>(x);
    lm[2 * k + 1] = static_cast<float>(y);
  }
  return lm;
}

// The stand-in model: the crop's colours inverted.
struct invert_model final : face_restorer {
  std::uint32_t n;
  int calls = 0;
  explicit invert_model(std::uint32_t side) : n(side) {}
  std::uint32_t side() const noexcept override { return n; }
  mv::result<rgb8> restore(const rgb8& face) override {
    ++calls;
    rgb8 r = face;
    for (auto& v : r.rgb) v = static_cast<std::uint8_t>(255 - v);
    return r;
  }
};

// The stand-in pack: the photo's faces; on an aligned crop, one face where the
// template puts it; with `other`, that face's cosine is `sim`.
struct fake_pack {
  std::uint32_t photo_w = 0, side = 0;
  std::vector<face_found> faces;
  float sim = 0.9f;
  int guard_calls = 0;
  faces_fn fn() {
    return [this](const rgb8& img, const rgb8* other) -> mv::result<std::vector<face_found>> {
      if (img.width == photo_w) return faces;
      if (img.width != side) return std::vector<face_found>{};
      face_found f;
      f.lm = tpl_at(side);
      f.x = side * 0.25f;
      f.y = side * 0.3f;
      f.w = side * 0.5f;
      f.h = side * 0.55f;
      f.score = 0.9f;
      if (other) {
        ++guard_calls;
        f.sim = sim;
      }
      return std::vector<face_found>{f};
    };
  }
};

}  // namespace

TEST_CASE("faces: the similarity fitted to five points, and its inverse", "[enhance][faces]") {
  similarity s;
  const double ang = 0.3, k = 0.42;
  s.a = k * std::cos(ang);
  s.b = k * std::sin(ang);
  s.tx = 120;
  s.ty = -35;
  const auto from = tpl_at(512);
  std::array<float, 10> to{};
  for (int i = 0; i < 5; ++i) {
    double x = 0, y = 0;
    s.apply(from[2 * i], from[2 * i + 1], x, y);
    to[2 * i] = static_cast<float>(x);
    to[2 * i + 1] = static_cast<float>(y);
  }
  const similarity f = fit_similarity(from, to);
  CHECK(f.a == Approx(s.a).margin(1e-5));
  CHECK(f.b == Approx(s.b).margin(1e-5));
  CHECK(f.tx == Approx(s.tx).margin(1e-3));
  CHECK(f.ty == Approx(s.ty).margin(1e-3));
  CHECK(f.scale() == Approx(k).margin(1e-5));
  // The inverse undoes it.
  const similarity inv = f.inverse();
  double x = 0, y = 0, bx = 0, by = 0;
  f.apply(300, 77, x, y);
  inv.apply(x, y, bx, by);
  CHECK(bx == Approx(300).margin(1e-6));
  CHECK(by == Approx(77).margin(1e-6));
  CHECK(inv.scale() == Approx(1.0 / k).margin(1e-6));

  // A mirrored face is never fitted with a reflection: the best rotation
  // (a, b always a rotation and a scale) is what comes back.
  std::array<float, 10> mirrored{};
  for (int i = 0; i < 5; ++i) {
    mirrored[2 * i] = 512.0f - from[2 * i];
    mirrored[2 * i + 1] = from[2 * i + 1];
  }
  const similarity m = fit_similarity(from, mirrored);
  CHECK(m.scale() < 1.0);  // a reflection would fit at scale 1 exactly
  // Degenerate points: identity.
  std::array<float, 10> same{};
  const similarity d = fit_similarity(same, from);
  CHECK(d.a == 1.0);
  CHECK(d.b == 0.0);
}

TEST_CASE("faces: the warp samples pixel centres and averages where it shrinks", "[enhance][faces]") {
  const rgb8 img = noise_rgb8(40, 30);
  // Identity at the same size: every pixel exactly.
  const rgb8 same = warp_rgb(img, similarity{}, 40, 30);
  CHECK(same.rgb == img.rgb);
  // A shift by whole pixels: exact too.
  similarity shift;
  shift.tx = -5;
  shift.ty = -3;
  const rgb8 moved = warp_rgb(img, shift, 20, 20);
  for (std::uint32_t y = 0; y < 20; ++y) {
    for (std::uint32_t x = 0; x < 20; ++x) {
      for (int c = 0; c < 3; ++c) {
        CHECK(moved.rgb[(std::size_t{y} * 20 + x) * 3 + c] == img.rgb[(std::size_t{y + 3} * 40 + x + 5) * 3 + c]);
      }
    }
  }
  // A one-pixel checkerboard halved: grey, not aliased back to black or white.
  rgb8 board;
  board.width = board.height = 64;
  board.rgb.resize(64 * 64 * 3);
  for (std::uint32_t y = 0; y < 64; ++y) {
    for (std::uint32_t x = 0; x < 64; ++x) {
      const std::uint8_t v = ((x + y) & 1) ? 255 : 0;
      for (int c = 0; c < 3; ++c) board.rgb[(std::size_t{y} * 64 + x) * 3 + c] = v;
    }
  }
  similarity half;
  half.a = 0.5;
  const rgb8 small = warp_rgb(board, half, 32, 32);
  for (std::uint32_t y = 2; y < 30; ++y) {
    for (std::uint32_t x = 2; x < 30; ++x) {
      const int v = small.rgb[(std::size_t{y} * 32 + x) * 3];
      CHECK(std::abs(v - 128) <= 2);
    }
  }
}

TEST_CASE("faces: the face mask is feathered, whole at the face and zero at the crop's edge", "[enhance][faces]") {
  const std::uint32_t side = 256;
  const std::vector<float> m = face_mask(side);
  REQUIRE(m.size() == std::size_t{side} * side);
  const auto at = [&](std::uint32_t x, std::uint32_t y) { return m[std::size_t{y} * side + x]; };
  // Whole between the eyes and the mouth.
  CHECK(at(side / 2, static_cast<std::uint32_t>(side * 0.56)) == Approx(1.0f));
  const auto t = tpl_at(side);
  for (int k = 0; k < 5; ++k) CHECK(at(static_cast<std::uint32_t>(t[2 * k]), static_cast<std::uint32_t>(t[2 * k + 1])) == Approx(1.0f));
  // Nothing at the edge or the corners.
  for (std::uint32_t i = 0; i < side; ++i) {
    CHECK(at(i, 0) == 0.0f);
    CHECK(at(i, side - 1) == 0.0f);
    CHECK(at(0, i) == 0.0f);
    CHECK(at(side - 1, i) == 0.0f);
  }
  // Values 0..1, falling steadily from the centre outwards, no hard step.
  float worst_step = 0;
  for (std::uint32_t y = 0; y < side; ++y) {
    for (std::uint32_t x = 0; x + 1 < side; ++x) {
      CHECK(at(x, y) >= 0.0f);
      CHECK(at(x, y) <= 1.0f);
      worst_step = std::max(worst_step, std::abs(at(x + 1, y) - at(x, y)));
    }
  }
  CHECK(worst_step < 0.08f);
  const std::uint32_t cy = static_cast<std::uint32_t>(side * 0.56);
  for (std::uint32_t x = side / 2; x + 1 < side; ++x) CHECK(at(x + 1, cy) <= at(x, cy));
}

TEST_CASE("faces: the blend and the paste back through the inverse warp", "[enhance][faces]") {
  const std::uint32_t side = 128;
  const std::vector<float> mask = face_mask(side);
  const rgb8 a = noise_rgb8(side, side, 1);
  const rgb8 r = noise_rgb8(side, side, 2);
  CHECK(blend_face(a, r, mask, 0.0f).rgb == a.rgb);
  const rgb8 full = blend_face(a, r, mask, 1.0f);
  const std::size_t centre = (std::size_t{72} * side + 64) * 3;
  CHECK(full.rgb[centre] == r.rgb[centre]);
  CHECK(full.rgb[0] == a.rgb[0]);  // the corner: mask 0
  const rgb8 part = blend_face(a, r, mask, 0.5f);
  CHECK(std::abs(int(part.rgb[centre]) - (int(a.rgb[centre]) + int(r.rgb[centre])) / 2) <= 1);

  // A face 2.5x smaller in the photo than the crop (the usual case): the
  // aligned crop pasted back unchanged leaves the photo as it was; outside the
  // crop's footprint nothing is touched at all.
  rgb8 photo = gradient_rgb8(200, 160);
  const rgb8 before = photo;
  similarity to;  // photo -> crop
  to.a = 2.5 * std::cos(0.2);
  to.b = 2.5 * std::sin(0.2);
  to.tx = -150;
  to.ty = -120;
  const rgb8 aligned = warp_rgb(photo, to, side, side);
  pixel_box box = paste_face(photo, to, aligned, aligned, mask, 1.0f);
  REQUIRE(box.width > 0);
  int worst = 0;
  for (std::size_t i = 0; i < photo.rgb.size(); ++i) worst = std::max(worst, std::abs(photo.rgb[i] - before.rgb[i]));
  CHECK(worst <= 3);
  // The restored face (inverted) pasted at full strength: the face centre in
  // the photo takes the inverse; every pixel outside the box is untouched.
  photo = before;
  rgb8 inverted = aligned;
  for (auto& v : inverted.rgb) v = static_cast<std::uint8_t>(255 - v);
  box = paste_face(photo, to, aligned, inverted, mask, 1.0f);
  REQUIRE(box.width > 0);
  double cx = 0, cy = 0;
  to.inverse().apply(side * 0.5, side * 0.56, cx, cy);
  const std::size_t c = (std::size_t(cy) * photo.width + std::size_t(cx)) * 3;
  CHECK(std::abs(int(photo.rgb[c]) - (255 - int(before.rgb[c]))) <= 6);
  std::size_t outside_changed = 0;
  for (std::uint32_t y = 0; y < photo.height; ++y) {
    for (std::uint32_t x = 0; x < photo.width; ++x) {
      if (x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height) continue;
      const std::size_t i = (std::size_t{y} * photo.width + x) * 3;
      outside_changed += photo.rgb[i] != before.rgb[i] || photo.rgb[i + 1] != before.rgb[i + 1] ||
                         photo.rgb[i + 2] != before.rgb[i + 2];
    }
  }
  CHECK(outside_changed == 0);
  // The box is inside the crop's footprint in the photo (side / 2.5 across).
  CHECK(box.width <= side / 2.5 * 1.5 + 2);

  // A face larger in the photo than the crop: only the model's change is
  // carried, so a restored crop equal to the aligned one changes nothing.
  photo = noise_rgb8(400, 400, 3);
  const rgb8 sharp = photo;
  similarity down;  // the face is 2x larger in the photo
  down.a = 0.5;
  down.tx = -40;
  down.ty = -40;
  const rgb8 small = warp_rgb(photo, down, side, side);
  box = paste_face(photo, down, small, small, mask, 1.0f);
  REQUIRE(box.width > 0);
  CHECK(photo.rgb == sharp.rgb);  // the photo's own detail, not the crop's blur
  // Strength 0 pastes nothing.
  CHECK(paste_face(photo, down, small, inverted, mask, 0.0f).width == 0);
  CHECK(photo.rgb == sharp.rgb);
}

TEST_CASE("faces: the pack's answer, read into the picture's pixels", "[enhance][faces]") {
  const auto r = parse_faces(
      R"({"faces":[{"x":0.1,"y":0.2,"w":0.3,"h":0.4,"score":0.9,"lm":[0.1,0.2,0.3,0.2,0.2,0.3,0.15,0.4,0.25,0.4],"sim":0.7},
                   {"x":0.5,"y":0.5,"w":0.1,"h":0.1,"score":0.8,"lm":[1,2,3]}]})",
      200, 100);
  REQUIRE(r);
  REQUIRE(r->size() == 1);  // the second has no five landmarks
  const face_found& f = r->front();
  CHECK(f.x == Approx(20));
  CHECK(f.y == Approx(20));
  CHECK(f.w == Approx(60));
  CHECK(f.h == Approx(40));
  CHECK(f.lm[0] == Approx(20));
  CHECK(f.lm[1] == Approx(20));
  CHECK(f.lm[9] == Approx(40));
  CHECK(f.sim == Approx(0.7f));
  CHECK_FALSE(parse_faces("{\"nope\":1}", 10, 10));
  const auto none = parse_faces(R"({"faces":[{"x":0,"y":0,"w":1,"h":1,"lm":[0,0,0,0,0,0,0,0,0,0]}]})", 10, 10);
  REQUIRE(none);
  CHECK(none->front().sim == -1.0f);
}

TEST_CASE("faces: with a stand-in model only the faces change, and the guard keeps a drifted face",
          "[enhance][faces]") {
  const std::uint32_t side = 128;
  rgb8 photo = noise_rgb8(640, 400);
  const rgb8 original = photo;
  // Two faces, each the template at 0.45 of the crop's size (about 23 px
  // between the eyes), turned a little.
  fake_pack pack;
  pack.photo_w = photo.width;
  pack.side = side;
  std::vector<similarity> places;
  for (const auto& [cx, ang] : {std::pair{160.0, 0.15}, std::pair{470.0, -0.2}}) {
    similarity s;  // template -> photo
    s.a = 0.45 * std::cos(ang);
    s.b = 0.45 * std::sin(ang);
    double ox = 0, oy = 0;
    s.apply(side * 0.5, side * 0.56, ox, oy);
    s.tx = cx - ox;
    s.ty = 200 - oy;
    places.push_back(s);
    face_found f;
    f.lm = landmarks_by(s, side);
    f.x = static_cast<float>(cx - 30);
    f.y = 165;
    f.w = 60;
    f.h = 70;
    f.score = 0.95f;
    pack.faces.push_back(f);
  }
  // A face too small to restore (3 px between the eyes).
  {
    similarity s;
    s.a = 3.0 / (kFfhq512[2] - kFfhq512[0]) * 512.0 / side;
    s.tx = 320;
    s.ty = 60;
    face_found f;
    f.lm = landmarks_by(s, side);
    f.x = 318;
    f.y = 58;
    f.w = 8;
    f.h = 8;
    pack.faces.push_back(f);
  }
  const faces_fn faces = pack.fn();
  auto found = find_faces(photo, faces);
  REQUIRE(found);
  REQUIRE(found->size() == 3);

  invert_model model(side);
  face_options o;
  o.strength = 1.0f;
  std::vector<std::pair<std::size_t, std::size_t>> steps;
  o.progress = [&](std::size_t d, std::size_t t) {
    steps.emplace_back(d, t);
    return true;
  };
  auto out = restore_faces(photo, *found, model, faces, o);
  REQUIRE(out);
  CHECK(out->found == 3);
  CHECK(out->restored == 2);
  CHECK(out->small == 1);
  CHECK(out->kept == 0);
  CHECK(model.calls == 2);
  CHECK(pack.guard_calls == 2);
  CHECK(steps.back() == std::pair<std::size_t, std::size_t>{3, 3});
  REQUIRE(out->changed.size() == 2);
  // Each face's pixels changed; nothing outside the faces' boxes did. The
  // box here is the mask's footprint: the ellipse round the template, carried
  // into the photo, grown by the bilinear reach.
  const auto in_face = [&](std::uint32_t x, std::uint32_t y) {
    for (const similarity& s : places) {
      double u = 0, v = 0;
      s.inverse().apply(x + 0.5, y + 0.5, u, v);
      const double du = (u / side - 0.5) / 0.34, dv = (v / side - 0.56) / 0.42;
      if (du * du + dv * dv < 1.15 * 1.15) return true;
    }
    return false;
  };
  std::size_t changed_in = 0, changed_out = 0;
  for (std::uint32_t y = 0; y < photo.height; ++y) {
    for (std::uint32_t x = 0; x < photo.width; ++x) {
      const std::size_t i = (std::size_t{y} * photo.width + x) * 3;
      const bool diff = photo.rgb[i] != original.rgb[i] || photo.rgb[i + 1] != original.rgb[i + 1] ||
                        photo.rgb[i + 2] != original.rgb[i + 2];
      if (in_face(x, y)) {
        changed_in += diff;
      } else {
        changed_out += diff;
      }
    }
  }
  CHECK(changed_out == 0);
  CHECK(changed_in > 2 * 400);
  // The reported boxes cover every change.
  for (std::uint32_t y = 0; y < photo.height; ++y) {
    for (std::uint32_t x = 0; x < photo.width; ++x) {
      const std::size_t i = (std::size_t{y} * photo.width + x) * 3;
      if (photo.rgb[i] == original.rgb[i] && photo.rgb[i + 1] == original.rgb[i + 1] &&
          photo.rgb[i + 2] == original.rgb[i + 2]) {
        continue;
      }
      bool covered = false;
      for (const pixel_box& b : out->changed) {
        covered |= x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
      }
      CHECK(covered);
    }
  }

  // The guard: a restored face that stops looking like the person keeps the
  // original, and with every face kept the photo is untouched.
  photo = original;
  pack.sim = kFaceGuard - 0.05f;
  out = restore_faces(photo, *found, model, faces, o);
  REQUIRE(out);
  CHECK(out->restored == 0);
  CHECK(out->kept == 2);
  CHECK(photo.rgb == original.rgb);
  // The pack losing the face on the restored crop counts as drift too.
  pack.sim = -1.0f;
  out = restore_faces(photo, *found, model, faces, o);
  REQUIRE(out);
  CHECK(out->restored == 0);
  CHECK(photo.rgb == original.rgb);

  // Strength 0 pastes nothing, so no face counts as restored.
  pack.sim = 0.9f;
  {
    face_options none = o;
    none.strength = 0.0f;
    out = restore_faces(photo, *found, model, faces, none);
    REQUIRE(out);
    CHECK(out->restored == 0);
    CHECK(out->changed.empty());
    CHECK(photo.rgb == original.rgb);
  }
  // Cancelled between faces.
  o.progress = [](std::size_t d, std::size_t) { return d < 1; };
  CHECK(restore_faces(photo, *found, model, faces, o).error() == mv::status::cancelled);

  // Without the pack (or People off) the answer says so.
  const faces_fn missing = [](const rgb8&, const rgb8*) -> mv::result<std::vector<face_found>> {
    return mv::err(mv::status::permission_denied);
  };
  CHECK(find_faces(original, missing).error() == mv::status::permission_denied);
}

TEST_CASE("faces: a large photo is asked about in tiles too, each face once", "[enhance][faces]") {
  const rgb8 photo = noise_rgb8(4200, 2400);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> asked;
  const faces_fn faces = [&](const rgb8& img, const rgb8*) -> mv::result<std::vector<face_found>> {
    asked.emplace_back(img.width, img.height);
    std::vector<face_found> out;
    // Always "a face" in the middle of whatever was asked.
    face_found f;
    f.x = img.width * 0.45f;
    f.y = img.height * 0.45f;
    f.w = img.width * 0.1f;
    f.h = img.width * 0.1f;
    for (int k = 0; k < 5; ++k) {
      f.lm[2 * k] = f.x + f.w * (0.3f + 0.1f * k);
      f.lm[2 * k + 1] = f.y + f.h * 0.5f;
    }
    out.push_back(f);
    return out;
  };
  auto found = find_faces(photo, faces);
  REQUIRE(found);
  REQUIRE(asked.size() > 1);
  CHECK(std::max(asked.front().first, asked.front().second) == kFindEdge);
  for (std::size_t i = 1; i < asked.size(); ++i) CHECK(std::max(asked[i].first, asked[i].second) <= kFindTile);
  // Faces in photo pixels, inside the photo; no two at one place.
  for (const face_found& f : *found) {
    CHECK(f.x >= 0);
    CHECK(f.x + f.w <= photo.width + 1);
    CHECK(f.y + f.h <= photo.height + 1);
  }
  for (std::size_t i = 0; i < found->size(); ++i) {
    for (std::size_t j = i + 1; j < found->size(); ++j) {
      const auto& a = (*found)[i];
      const auto& b = (*found)[j];
      CHECK(std::hypot(a.x + a.w / 2 - b.x - b.w / 2, a.y + a.h / 2 - b.y - b.h / 2) > 1.0f);
    }
  }
}

TEST_CASE("faces: the shipped model.json and its pins", "[enhance][faces]") {
  const auto text = mv::test::read_bytes(MV_ENHANCE_FACE_SPEC);
  const auto spec =
      parse_spec(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), model_kind::face);
  REQUIRE(spec);
  CHECK(kind_name(model_kind::face) == std::string("face"));
  CHECK(kind_from("face") == model_kind::face);
  const knob* m = spec->find("model");
  REQUIRE(m);
  CHECK(m->def_choice == "restoreformer");
  CHECK(m->options.size() == 2);
  const knob* s = spec->find("strength");
  REQUIRE(s);
  CHECK(s->def == Approx(0.7));
  const mv::json::value* models = spec->io.find("models");
  REQUIRE(models);
  for (const knob_option& o : m->options) {
    const mv::json::value* one = models->find(o.id);
    REQUIRE(one);
    const std::string* file = one->str("file");
    REQUIRE(file);
    CHECK(std::find(spec->weights.begin(), spec->weights.end(), *file) != spec->weights.end());
  }
  CHECK(*models->find("gfpgan")->str("output") == "1288");
  // Each weights file pinned in remote.json: the plan's Models table.
  const std::filesystem::path remote =
      std::filesystem::path(MV_ENHANCE_FACE_SPEC).parent_path().parent_path().parent_path() / "remote.json";
  const auto rtext = mv::test::read_bytes(remote);
  const auto pins = mv::json::parse(std::string_view(reinterpret_cast<const char*>(rtext.data()), rtext.size()), 8);
  REQUIRE(pins);
  const std::pair<const char*, std::int64_t> want[] = {{"face/restoreformer_plus_plus.onnx", 294264232},
                                                       {"face/GFPGANv1.4.onnx", 340256686}};
  for (const auto& [path, size] : want) {
    bool seen = false;
    for (const mv::json::value& p : pins->a) {
      const std::string* pp = p.str("path");
      if (!pp || *pp != path) continue;
      seen = true;
      CHECK(p.integer("size").value_or(0) == size);
      CHECK(*p.str("licence") == "Apache-2.0");
      CHECK(p.str("url")->find("/resolve/") != std::string::npos);
    }
    CHECK(seen);
  }
}

TEST_CASE("faces: the runner writes <stem>-restored.jpg and says why when it cannot", "[enhance][faces]") {
  const auto text = mv::test::read_bytes(MV_ENHANCE_FACE_SPEC);
  const auto spec =
      parse_spec(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), model_kind::face);
  REQUIRE(spec);
  const std::uint32_t side = 512;  // the spec's
  const rgb8 original = gradient_rgb8(900, 700);
  fake_pack pack;
  pack.photo_w = original.width;
  pack.side = side;
  similarity place;
  place.a = 0.3;
  place.tx = 300;
  place.ty = 200;
  face_found f;
  f.lm = landmarks_by(place, side);
  f.x = 330;
  f.y = 240;
  f.w = 90;
  f.h = 110;
  pack.faces.push_back(f);

  std::string suffix_seen, choice_seen;
  std::vector<std::uint8_t> written;
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = original.width;
    s.height = original.height;
    s.rgb = original.rgb;
    return s;
  };
  std::atomic<int> plain_writes{0};
  port.write_still = [&](const std::string&, const run_port::still&) -> mv::result<std::string> {
    ++plain_writes;  // Restore faces names its own file
    return std::string();
  };
  port.write_still_as = [&](const std::string&, const run_port::still& pic,
                            const std::string& suffix) -> mv::result<std::string> {
    suffix_seen = suffix;
    written = pic.rgb;
    return std::string("/photos/IMG_0001-") + suffix + ".jpg";
  };
  port.load_face = [&](const model_spec&, const std::string&, const std::string& choice,
                       int) -> mv::result<std::unique_ptr<face_restorer>> {
    choice_seen = choice;
    return std::unique_ptr<face_restorer>(new invert_model(side));
  };
  port.still_faces = pack.fn();

  struct waiter {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    std::string name, message;
  };
  const auto run = [&](job_runner& jr, std::map<std::string, mv::json::value> values) {
    auto w = std::make_shared<waiter>();
    job_request r;
    r.path = "/photos/IMG_0001.JPG";
    r.item = item_kind::still;
    job_request::part p;
    p.kind = model_kind::face;
    p.spec = &*spec;
    p.models_dir = "/data/models/face";
    p.values = std::move(values);
    r.parts.push_back(p);
    CHECK(jr.check(r).empty());
    job_events ev;
    ev.done = [w](bool ok, const std::string& name, const std::string& message) {
      std::lock_guard lock(w->m);
      w->done = true;
      w->ok = ok;
      w->name = name;
      w->message = message;
      w->cv.notify_all();
    };
    jr.start(std::move(r), std::move(ev));
    std::unique_lock lock(w->m);
    w->cv.wait_for(lock, std::chrono::seconds(60), [&] { return w->done; });
    REQUIRE(w->done);
    return std::tuple{w->ok, w->name, w->message};
  };

  job_runner jr(port);
  CHECK(jr.supports(model_kind::face, item_kind::still));
  CHECK_FALSE(jr.supports(model_kind::face, item_kind::media));
  {
    [[maybe_unused]] const auto [ok, name, message] = run(jr, {});
    CHECK(ok);
    CHECK(name == "IMG_0001-restored.jpg");
    CHECK(suffix_seen == "restored");
    CHECK(choice_seen == "restoreformer");  // the default look
    REQUIRE(written.size() == original.rgb.size());
    // Strength defaulted to 0.7: the face centre is 70 % of the way to the
    // inverse, a far corner is the photo's own.
    double cx = 0, cy = 0;
    place.apply(side * 0.5, side * 0.56, cx, cy);
    const std::size_t c = (std::size_t(cy) * original.width + std::size_t(cx)) * 3;
    const double expect = original.rgb[c] + 0.7 * (255.0 - 2.0 * original.rgb[c]);
    CHECK(std::abs(written[c] - expect) <= 8);
    CHECK(written[0] == original.rgb[0]);
  }
  {
    mv::json::value g;
    g.k = mv::json::kind::string;
    g.s = "gfpgan";
    [[maybe_unused]] const auto [ok, name, message] = run(jr, {{"model", g}});
    CHECK(ok);
    CHECK(choice_seen == "gfpgan");
  }
  // Strength 0 would change nothing: said, the model never loaded, nothing written.
  {
    mv::json::value zero;
    zero.k = mv::json::kind::number;
    zero.is_integer = true;
    zero.i = 0;
    zero.d = 0;
    choice_seen.clear();
    written.clear();
    [[maybe_unused]] const auto [ok, name, message] = run(jr, {{"strength", zero}});
    CHECK_FALSE(ok);
    CHECK(message.find("Strength") != std::string::npos);
    CHECK(choice_seen.empty());
    CHECK(written.empty());
  }
  // People off: said so, nothing written.
  written.clear();
  port.still_faces = [](const rgb8&, const rgb8*) -> mv::result<std::vector<face_found>> {
    return mv::err(mv::status::permission_denied);
  };
  {
    job_runner off(port);
    [[maybe_unused]] const auto [ok, name, message] = run(off, {});
    CHECK_FALSE(ok);
    CHECK(message.find("People") != std::string::npos);
    CHECK(written.empty());
  }
  // No faces: said so.
  port.still_faces = [](const rgb8&, const rgb8*) -> mv::result<std::vector<face_found>> {
    return std::vector<face_found>{};
  };
  {
    job_runner none(port);
    [[maybe_unused]] const auto [ok, name, message] = run(none, {});
    CHECK_FALSE(ok);
    CHECK(message.find("No faces") != std::string::npos);
  }
  // Every face drifted: the photo is not written again unchanged.
  pack.sim = 0.1f;
  port.still_faces = pack.fn();
  {
    job_runner drift(port);
    [[maybe_unused]] const auto [ok, name, message] = run(drift, {});
    CHECK_FALSE(ok);
    CHECK(message.find("same person") != std::string::npos);
    CHECK(written.empty());
  }
  CHECK(plain_writes.load() == 0);
  // Without the pack's row the kind is not offered.
  port.still_faces = nullptr;
  CHECK_FALSE(job_runner(port).supports(model_kind::face, item_kind::still));
}

// The real models, from local copies (never downloaded by a test). The face's
// own landmarks come in MV_FACE_LM; the guard is not the pack's here (its
// threshold is measured by tools/enhance/face_guard.py), so this checks the
// models' tensors, the alignment and the paste, and times a face.
TEST_CASE("faces: RestoreFormer++ and GFPGAN through ONNX Runtime", "[.face-model]") {
  const char* ort = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  const char* dir = std::getenv("MV_FACE_MODEL_DIR");
  const char* raw = std::getenv("MV_FACE_RGB");
  const char* ws = std::getenv("MV_FACE_W");
  const char* hs = std::getenv("MV_FACE_H");
  const char* lms = std::getenv("MV_FACE_LM");
  if (!ort || !dir || !raw || !ws || !hs || !lms) SKIP("MV_ENHANCE_RUNTIME_DIR / MV_FACE_MODEL_DIR / MV_FACE_* not set");
  rgb8 photo;
  photo.width = static_cast<std::uint32_t>(std::atoi(ws));
  photo.height = static_cast<std::uint32_t>(std::atoi(hs));
  photo.rgb = mv::test::read_bytes(raw);
  REQUIRE(photo.rgb.size() == std::size_t{photo.width} * photo.height * 3);
  face_found face;
  {
    std::istringstream in(lms);
    for (float& v : face.lm) in >> v;
    REQUIRE(in);
  }
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int k = 0; k < 5; ++k) {
    x0 = std::min(x0, face.lm[2 * k]);
    x1 = std::max(x1, face.lm[2 * k]);
    y0 = std::min(y0, face.lm[2 * k + 1]);
    y1 = std::max(y1, face.lm[2 * k + 1]);
  }
  face.x = x0 - (x1 - x0) * 0.5f;
  face.w = (x1 - x0) * 2.0f;
  face.y = y0 - (y1 - y0) * 0.8f;
  face.h = (y1 - y0) * 2.4f;
  auto rt = mv::infer::runtime::load(ort);
  REQUIRE(rt);
  struct one {
    const char* file;
    const char* output;
    const char* name;
  };
  int ran = 0;
  for (const one& m : {one{"restoreformer_plus_plus.onnx", "", "RestoreFormer++"},
                       one{"GFPGANv1.4.onnx", "1288", "GFPGAN v1.4"}}) {
    const std::string file = std::string(dir) + "/" + m.file;
    if (!std::filesystem::exists(file)) continue;
    ++ran;
    face_io io;
    io.file = m.file;
    io.output = m.output;
    auto model = face_model::open(**rt, file, io, 0);
    INFO(mv::infer::session::last_error());
    REQUIRE(model);
    // The face aligned as the pipeline does it, restored once to time it.
    const similarity to = fit_similarity(face.lm, tpl_at(512));
    const rgb8 aligned = warp_rgb(photo, to, 512, 512);
    const auto t0 = std::chrono::steady_clock::now();
    auto restored = (*model)->restore(aligned);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(restored);
    // A face, not noise or a flat field: close to the input on average, yet changed.
    double diff = 0;
    for (std::size_t i = 0; i < aligned.rgb.size(); ++i) diff += std::abs(aligned.rgb[i] - restored->rgb[i]);
    diff /= static_cast<double>(aligned.rgb.size());
    std::printf("faces: %s restored a 512 face in %.2f s on CPU; mean change %.1f levels\n", m.name, secs, diff);
    CHECK(diff > 1.0);
    CHECK(diff < 40.0);
    // The whole pipeline with a pack that answers the given landmarks and a
    // guard that passes: only the face changes.
    const faces_fn faces = [&](const rgb8& img, const rgb8* other) -> mv::result<std::vector<face_found>> {
      if (img.width == photo.width && img.height == photo.height) return std::vector<face_found>{face};
      face_found c;
      c.lm = tpl_at(512);
      c.x = 128;
      c.y = 150;
      c.w = 256;
      c.h = 280;
      if (other) c.sim = 1.0f;
      return std::vector<face_found>{c};
    };
    rgb8 out = photo;
    face_options o;
    const auto p0 = std::chrono::steady_clock::now();
    auto r = restore_faces(out, {face}, **model, faces, o);
    std::printf("faces: %s, the whole face (align, refit, restore, blend, paste) in %.2f s\n", m.name,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - p0).count());
    REQUIRE(r);
    CHECK(r->restored == 1);
    REQUIRE(r->changed.size() == 1);
    const pixel_box& b = r->changed.front();
    std::size_t outside = 0;
    for (std::uint32_t y = 0; y < out.height; ++y) {
      for (std::uint32_t x = 0; x < out.width; ++x) {
        if (x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height) continue;
        const std::size_t i = (std::size_t{y} * out.width + x) * 3;
        outside += out.rgb[i] != photo.rgb[i] || out.rgb[i + 1] != photo.rgb[i + 1] || out.rgb[i + 2] != photo.rgb[i + 2];
      }
    }
    CHECK(outside == 0);
    if (const char* keep = std::getenv("MV_FACE_OUT")) {
      mv::test::write_bytes(std::string(keep) + "." + m.file + ".rgb", out.rgb);
      mv::test::write_bytes(std::string(keep) + "." + m.file + ".aligned.rgb", aligned.rgb);
      mv::test::write_bytes(std::string(keep) + "." + m.file + ".restored.rgb", restored->rgb);
    }
  }
  CHECK(ran > 0);
}
