// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S8: the Faces tool (shell/faces_tool.h): the
// cards from the pack's answer and the still, choosing faces, the boxes on
// the canvas, and what the panel's controls send.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>

#include "image/half.h"
#include "image/linear.h"
#include "shell/canvas_tool.h"
#include "shell/faces_tool.h"

using namespace mv::shell;
using Catch::Approx;

namespace {

// Skin, and two faces' eyes: the left face's open (white almond, dark iris),
// the right face's shut (a line of lashes). Eyes `d` px apart.
mv::image::linear_image two_faces(std::uint32_t w, std::uint32_t h, float d) {
  mv::image::linear_image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(std::size_t{w} * h * 4);
  const float cy = static_cast<float>(h) * 0.5f;
  const float centres[2] = {static_cast<float>(w) * 0.27f, static_cast<float>(w) * 0.73f};
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      float r = 0.45f, g = 0.30f, b = 0.22f;
      for (int f = 0; f < 2; ++f) {
        for (float side : {-0.5f, 0.5f}) {
          const float ux = static_cast<float>(x) - (centres[f] + side * d), uy = static_cast<float>(y) - cy;
          if (f == 0) {
            if ((ux / (0.25f * d)) * (ux / (0.25f * d)) + (uy / (0.11f * d)) * (uy / (0.11f * d)) < 1.0f) r = g = b = 0.75f;
            if (ux * ux + uy * uy < (0.09f * d) * (0.09f * d)) r = 0.05f, g = 0.04f, b = 0.03f;
          } else if (std::abs(uy) < 0.012f * d + 0.5f && std::abs(ux) < 0.25f * d) {
            r = 0.06f, g = 0.05f, b = 0.05f;
          }
        }
      }
      std::uint16_t* p = img.rgba.data() + (std::size_t{y} * w + x) * 4;
      p[0] = mv::image::float_to_half(r);
      p[1] = mv::image::float_to_half(g);
      p[2] = mv::image::float_to_half(b);
      p[3] = mv::image::float_to_half(1.0f);
    }
  }
  return img;
}

// The pack's answer for two_faces: boxes, landmarks, who they are.
std::string answer(float w, float h, float d) {
  std::string out = "{\"faces\":[";
  const float cy = h * 0.5f;
  const float centres[2] = {w * 0.27f, w * 0.73f};
  for (int f = 0; f < 2; ++f) {
    const float cx = centres[f];
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,\"lm\":[%g,%g,%g,%g,%g,%g,%g,%g,%g,%g],\"person\":%d%s,"
                  "\"photos\":%d,\"donors\":[]}",
                  f ? "," : "", (cx - d) / w, (cy - d) / h, 2 * d / w, 2.4f * d / h, (cx - d / 2) / w, cy / h,
                  (cx + d / 2) / w, cy / h, cx / w, (cy + d / 2) / h, (cx - d / 3) / w, (cy + d) / h,
                  (cx + d / 3) / w, (cy + d) / h, f ? 9 : 7, f ? "" : ",\"name\":\"Anna\"", f ? 0 : 37);
    out += buf;
  }
  return out + "]}";
}

mv::edit::placement plain(std::uint32_t w, std::uint32_t h) {
  return mv::edit::place(mv::edit::geometry{}, mv::edit::size2{w, h});
}

}  // namespace

TEST_CASE("faces tool: cards from the pack's answer and the still", "[faces-tool]") {
  const auto img = two_faces(800, 400, 80);
  const faces_scan scan = read_faces(mv::status::ok, answer(800, 400, 80), &img);
  REQUIRE(scan.status == faces_status::ready);
  REQUIRE(scan.faces.size() == 2);
  const face_card& anna = scan.faces[0];
  const face_card& other = scan.faces[1];
  CHECK(anna.name == "Anna");
  CHECK(anna.person == 7);
  CHECK(anna.photos == 37);
  CHECK(other.name.empty());
  CHECK_FALSE(anna.shut);
  CHECK(other.shut);
  // Open eyes only where they look shut; Improve face everywhere.
  CHECK(actions_of(anna) == (1u << static_cast<unsigned>(face_action::improve)));
  CHECK(actions_of(other) == ((1u << static_cast<unsigned>(face_action::improve)) |
                              (1u << static_cast<unsigned>(face_action::open_eyes))));
  // A thumbnail a face, opaque sRGB, the skin's colour about its edge.
  REQUIRE(anna.thumb_w == kFaceThumbPx);
  REQUIRE(anna.thumb.size() == std::size_t{kFaceThumbPx} * kFaceThumbPx * 4);
  CHECK(anna.thumb[3] == 255);
  CHECK(anna.thumb[0] > anna.thumb[2]);  // warm skin: red over blue
  // The region holds the box.
  CHECK(anna.region[0] <= anna.box[0]);
  CHECK(anna.region[0] + anna.region[2] >= anna.box[0] + anna.box[2]);
  CHECK(face_source_line(anna) == "from 37 photos of Anna");
  CHECK(face_source_line(other).empty());
  face_card one = other;
  one.photos = 1;
  CHECK(face_source_line(one) == "from 1 photo of this person");

  // No still: the cards, without thumbnails, nothing reads as shut.
  const faces_scan bare = read_faces(mv::status::ok, answer(800, 400, 80), nullptr);
  REQUIRE(bare.faces.size() == 2);
  CHECK(bare.faces[1].thumb.empty());
  CHECK_FALSE(bare.faces[1].shut);
}

TEST_CASE("faces tool: the pack's refusals are the panel's empty states", "[faces-tool]") {
  CHECK(read_faces(mv::status::not_found, "", nullptr).status == faces_status::no_pack);
  CHECK(read_faces(mv::status::invalid_arg, "", nullptr).status == faces_status::people_off);
  CHECK(read_faces(mv::status::io, "", nullptr).status == faces_status::failed);
  CHECK(read_faces(mv::status::ok, "not json", nullptr).status == faces_status::failed);
  const faces_scan none = read_faces(mv::status::ok, "{\"faces\":[]}", nullptr);
  CHECK(none.status == faces_status::ready);
  CHECK(none.faces.empty());
  // Malformed faces are skipped; at most kToolFaces are kept.
  std::string many = "{\"faces\":[{\"x\":0.1}";
  for (int i = 0; i < kToolFaces + 4; ++i) {
    many += ",{\"x\":0.1,\"y\":0.1,\"w\":0.05,\"h\":0.05,\"lm\":[0.11,0.12,0.13,0.12,0.12,0.13,0.11,0.14,0.13,0.14]}";
  }
  many += "]}";
  CHECK(read_faces(mv::status::ok, many, nullptr).faces.size() == static_cast<std::size_t>(kToolFaces));
}

TEST_CASE("faces tool: every face starts chosen; a click leaves one out; a stale read is dropped", "[faces-tool]") {
  faces_tool t;
  t.looking(5);
  CHECK(t.status == faces_status::looking);
  CHECK(faces_status_line(t) == "Looking for faces…");
  const std::uint32_t rev = t.revision;
  CHECK_FALSE(t.landed(6, read_faces(mv::status::ok, answer(800, 400, 80), nullptr)));  // another photo's
  CHECK(t.status == faces_status::looking);
  REQUIRE(t.landed(5, read_faces(mv::status::ok, answer(800, 400, 80), nullptr)));
  CHECK(t.revision != rev);
  CHECK(t.chosen_count() == 2);
  CHECK(faces_status_line(t) == "2 faces · 2 chosen");
  CHECK(t.toggle(1));
  CHECK(t.chosen_count() == 1);
  CHECK(t.is_chosen(0));
  CHECK_FALSE(t.is_chosen(1));
  CHECK_FALSE(t.toggle(2));
  CHECK_FALSE(t.toggle(-1));
  // The run's boxes: the chosen faces, or the one face asked for.
  const auto chosen = t.run_boxes(-1);
  REQUIRE(chosen.size() == 4);
  CHECK(chosen[0] == Approx(t.faces[0].box[0]));
  const auto second = t.run_boxes(1);
  REQUIRE(second.size() == 4);
  CHECK(second[0] == Approx(t.faces[1].box[0]));
  t.choose_none();
  CHECK(t.run_boxes(-1).empty());
  t.choose_all();
  CHECK(t.chosen_count() == 2);
  CHECK(t.set_hover(1));
  CHECK_FALSE(t.set_hover(1));
  CHECK(t.set_hover(9));  // off the list: none
  CHECK(t.hover == -1);
  t.clear();
  CHECK(t.status == faces_status::idle);
  CHECK(t.count() == 0);
}

TEST_CASE("faces tool: the box under the pointer, through a crop and a turn", "[faces-tool]") {
  faces_tool t;
  t.looking(1);
  REQUIRE(t.landed(1, read_faces(mv::status::ok, answer(800, 400, 80), nullptr)));
  const auto p = plain(800, 400);
  const auto& r0 = t.faces[0].region;
  CHECK(face_at(t, p, r0[0] + r0[2] * 0.5f, r0[1] + r0[3] * 0.5f) == 0);
  const auto& r1 = t.faces[1].region;
  CHECK(face_at(t, p, r1[0] + r1[2] * 0.5f, r1[1] + r1[3] * 0.5f) == 1);
  CHECK(face_at(t, p, 0.5f, 0.02f) == -1);
  // Cropped to the right half: the second face fills more of the view.
  mv::edit::geometry g;
  g.crop = {0.5f, 0.0f, 0.5f, 1.0f};
  const auto cropped = mv::edit::place(g, mv::edit::size2{800, 400});
  const auto shown = region_to_shown(cropped, r1);
  CHECK(shown[0] == Approx((r1[0] - 0.5f) / 0.5f).margin(1e-4));
  CHECK(shown[2] == Approx(r1[2] / 0.5f).margin(1e-4));
  CHECK(face_at(t, cropped, shown[0] + shown[2] * 0.5f, shown[1] + shown[3] * 0.5f) == 1);
}

TEST_CASE("faces tool: the boxes onto the edit view, with names cut whole", "[faces-tool]") {
  faces_tool t;
  t.looking(1);
  auto scan = read_faces(mv::status::ok, answer(800, 400, 80), nullptr);
  scan.faces[1].name = "Zoë Ångström-Øverlândéèê";  // 28 bytes: cut before a 2-byte letter
  REQUIRE(t.landed(1, std::move(scan)));
  (void)t.toggle(0);
  t.running = true;
  t.running_face = -1;
  (void)t.set_hover(1);
  mv::shell::edit_view v;
  v.tool = static_cast<std::uint8_t>(canvas_tool::faces);
  apply_faces(v, t, plain(800, 400));
  CHECK(v.tool_face_count == 2);
  CHECK(v.tool_face_chosen == 2u);
  CHECK(v.tool_face_busy == 2u);  // running on the chosen faces
  CHECK(v.tool_face_hover == 1);
  CHECK(v.tool_faces[0][0] == Approx(t.faces[0].region[0]));
  CHECK(std::string(v.tool_face_names[0]) == "Anna");
  const std::string cut = v.tool_face_names[1];
  CHECK(cut.size() < static_cast<std::size_t>(kToolFaceName));
  CHECK(std::string("Zoë Ångström-Øverlândéèê").rfind(cut, 0) == 0);
  CHECK((static_cast<unsigned char>(cut.back()) & 0xC0u) != 0xC0u);  // not a lead byte left alone
  // Another tool: no boxes.
  v.tool = static_cast<std::uint8_t>(canvas_tool::red_eye);
  apply_faces(v, t, plain(800, 400));
  CHECK(v.tool_face_count == 0);
}

TEST_CASE("faces tool: the panel's controls pack into one number and back", "[faces-tool]") {
  for (const faces_op op : {faces_op::run, faces_op::toggle, faces_op::choose_all, faces_op::choose_none,
                            faces_op::people}) {
    for (int face : {-1, 0, 7, kToolFaces - 1}) {
      for (int action : {kFaceDoAll, 0, kFaceActionCount - 1}) {
        const std::int32_t v = pack_faces_op(op, face, action);
        CHECK(static_cast<float>(v) == static_cast<double>(v));  // exact through chrome_cmd's float
        const auto back = unpack_faces_op(v);
        REQUIRE(back);
        CHECK(back->op == op);
        CHECK(back->face == face);
        CHECK(back->action == action);
      }
    }
  }
  CHECK_FALSE(unpack_faces_op(-5));
  CHECK_FALSE(unpack_faces_op(pack_faces_op(faces_op::run, kToolFaces, 0)));
  CHECK_FALSE(unpack_faces_op(pack_faces_op(faces_op::run, 0, kFaceActionCount)));
  CHECK_FALSE(unpack_faces_op(9 * 100000));
  // The action table is the wire order.
  for (int i = 0; i < kFaceActionCount; ++i) CHECK(static_cast<int>(kFaceActions[static_cast<std::size_t>(i)].id) == i);
}
