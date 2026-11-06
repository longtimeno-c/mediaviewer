// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/compare_source.h: what Compare compares the still on screen with
// (docs/design/20 "Compare"), the Enhance naming rule (docs/design/29) and the
// before/after split's maths.
#include "catch_compat.h"
#include <catch2/catch_approx.hpp>

#include <set>
#include <string>
#include <vector>

#include "shell/compare_source.h"

using namespace mv::shell;
using Catch::Approx;

TEST_CASE("an Enhance result's name gives the stem of its source", "[compare-source]") {
  CHECK(result_source_stem("/p/IMG_0007-enhance.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-restored.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-extended.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-removed.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-eyes.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-faces.jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/shot-enhance.png") == "/p/shot");
  // The first free name ("name (2)", io/collision_name.h).
  CHECK(result_source_stem("/p/IMG_0007-enhance (2).jpg") == "/p/IMG_0007");
  CHECK(result_source_stem("/p/IMG_0007-enhance (13).JPG") == "/p/IMG_0007");
  CHECK(result_source_stem("C:\\Photos\\IMG_0007-faces (3).jpg") == "C:\\Photos\\IMG_0007");
  // A result of a result: its source is the first result.
  CHECK(result_source_stem("/p/IMG_0007-enhance-extended.jpg") == "/p/IMG_0007-enhance");
  // A hyphen in the stem stays.
  CHECK(result_source_stem("/p/my-trip-day-2-restored.jpg") == "/p/my-trip-day-2");
  CHECK(result_source_stem("IMG-enhance.jpg") == "IMG");
}

TEST_CASE("names that are not results are not taken for one", "[compare-source]") {
  CHECK(result_source_stem("/p/IMG_0007.jpg").empty());
  CHECK(result_source_stem("/p/-enhance.jpg").empty());           // no stem
  CHECK(result_source_stem("/p/IMG_0007-enhance.heic").empty());  // results are JPEG or PNG
  CHECK(result_source_stem("/p/IMG_0007-enhance.m4a").empty());   // a sound result: no still
  CHECK(result_source_stem("/p/IMG_0007enhance.jpg").empty());
  CHECK(result_source_stem("/p/IMG_0007-enhanced.jpg").empty());
  CHECK(result_source_stem("/p/IMG_0007 (2).jpg").empty());
  CHECK(result_source_stem("/p/IMG_0007-enhance (x).jpg").empty());
  CHECK(result_source_stem("/p/IMG_0007-enhance ().jpg").empty());
  CHECK(result_source_stem("/p-enhance/IMG_0007.jpg").empty());  // the folder's name is not the file's
  CHECK(result_source_stem("photos:1234-ABCD/L0/001").empty());
  CHECK_FALSE(looks_like_result("/p/IMG_0007.jpg"));
  CHECK(looks_like_result("/p/IMG_0007-enhance.jpg"));
}

TEST_CASE("Extend's results lie inside their frame; the others are the same picture", "[compare-source]") {
  CHECK(result_layout("/p/a-extended.jpg") == compare_layout::extended);
  CHECK(result_layout("/p/a-extended (2).jpg") == compare_layout::extended);
  CHECK(result_layout("/p/a-enhance.jpg") == compare_layout::same_picture);
  CHECK(result_layout("/p/a.jpg") == compare_layout::same_picture);
}

TEST_CASE("a listing's file is a source by stem and still extension, any case", "[compare-source]") {
  CHECK(is_source_name("/p/IMG_0007-enhance.jpg", "/p/IMG_0007.JPG"));
  CHECK(is_source_name("/p/IMG_0007-enhance (2).jpg", "/p/IMG_0007.heic"));
  CHECK(is_source_name("/p/IMG_0007-enhance.jpg", "/p/img_0007.cr3"));
  CHECK_FALSE(is_source_name("/p/IMG_0007-enhance.jpg", "/p/IMG_0007.MOV"));  // a Live Photo's clip
  CHECK_FALSE(is_source_name("/p/IMG_0007-enhance.jpg", "/p/IMG_00071.jpg"));
  CHECK_FALSE(is_source_name("/p/IMG_0007-enhance.jpg", "/q/IMG_0007.jpg"));
  CHECK_FALSE(is_source_name("/p/IMG_0007-enhance.jpg", "/p/IMG_0007-enhance.jpg"));
  CHECK_FALSE(is_source_name("/p/IMG_0007.jpg", "/p/IMG_0007.jpg"));
}

TEST_CASE("source_of finds the source through the caller's check", "[compare-source]") {
  const std::set<std::string> disk = {"/p/IMG_0007.heic", "/p/IMG_0007.jpg.xmp", "/p/IMG_0008.jpg"};
  int asked = 0;
  const auto exists = [&](const std::string& p) {
    ++asked;
    return disk.count(p) != 0;
  };
  CHECK(source_of("/p/IMG_0007-enhance.jpg", exists) == "/p/IMG_0007.heic");
  CHECK(asked > 0);
  asked = 0;
  CHECK(source_of("/p/IMG_0009-enhance.jpg", exists).empty());
  CHECK(asked == static_cast<int>(std::size(kCompareSourceExtensions)));
  asked = 0;
  // Not a result's name: the check is never asked (no I/O for a plain photo).
  CHECK(source_of("/p/IMG_0008.jpg", exists).empty());
  CHECK(asked == 0);
  const std::vector<std::string> c = source_candidates("/p/IMG_0007-enhance (2).jpg");
  REQUIRE(c.size() == std::size(kCompareSourceExtensions));
  CHECK(c.front() == "/p/IMG_0007.jpg");
  CHECK(source_candidates("/p/IMG_0007.jpg").empty());
}

TEST_CASE("a result the app was told of beats the naming rule, Photos keys too", "[compare-source]") {
  const auto never = [](const std::string&) { return false; };
  CHECK(source_of("/t/noted-enhance.jpg", never).empty());
  note_result("/t/noted-enhance.jpg", "/t/elsewhere/original.dng");
  CHECK(source_of("/t/noted-enhance.jpg", never) == "/t/elsewhere/original.dng");
  // Any name: one the app wrote under a name of its own.
  note_result("photos:NEW-KEY", "photos:OLD-KEY", compare_rect{0.1f, 0.2f, 0.8f, 0.6f});
  compare_rect where;
  CHECK(noted_source("photos:NEW-KEY", &where) == "photos:OLD-KEY");
  CHECK(where.known());
  CHECK(where.x == Approx(0.1f));
  CHECK(source_of("photos:NEW-KEY", never) == "photos:OLD-KEY");
  // A second note of one result replaces the first.
  note_result("photos:NEW-KEY", "photos:OTHER");
  CHECK(noted_source("photos:NEW-KEY", &where) == "photos:OTHER");
  CHECK_FALSE(where.known());
  // Nothing is noted for an empty or self pair.
  note_result("/t/self.jpg", "/t/self.jpg");
  CHECK(noted_source("/t/self.jpg").empty());
  // The registry is bounded: the oldest go.
  for (int i = 0; i < 100; ++i) note_result("/t/r" + std::to_string(i) + ".jpg", "/t/s.jpg");
  CHECK(noted_source("/t/r99.jpg") == "/t/s.jpg");
  CHECK(noted_source("/t/r0.jpg").empty());
  CHECK(noted_source("/t/noted-enhance.jpg").empty());
}

TEST_CASE("nothing to compare says why", "[compare-source]") {
  CHECK(can_compare(compare_kind::edits));
  CHECK(can_compare(compare_kind::source));
  CHECK_FALSE(can_compare(compare_kind::none));
  CHECK_FALSE(can_compare(compare_kind::source_missing));
  CHECK_FALSE(can_compare(compare_kind::looking));
  CHECK(std::string(compare_unavailable_reason(compare_kind::none)) == "No edits yet to compare");
  CHECK_FALSE(std::string(compare_unavailable_reason(compare_kind::source_missing)).empty());
  CHECK(std::string(compare_unavailable_reason(compare_kind::edits)).empty());
}

namespace {
// Where output uv (u, v) samples the source.
void sample(const compare_map& m, float u, float v, float& su, float& sv) {
  su = m.m[0] * u + m.m[1] * v + m.m[2];
  sv = m.m[3] * u + m.m[4] * v + m.m[5];
}
}  // namespace

TEST_CASE("the source fills a larger result of the same picture", "[compare-source]") {
  // Enhance image at 4x: the identity, nothing clipped.
  const compare_map m = source_map(16000, 12000, 4000, 3000);
  CHECK(m.m[0] == 1.0f);
  CHECK(m.m[2] == 0.0f);
  CHECK(m.m[4] == 1.0f);
  CHECK_FALSE(m.clip);
  // Sizes not known yet: the identity.
  CHECK_FALSE(source_map(0, 0, 4000, 3000).clip);
}

TEST_CASE("an Extend source sits inside the frame at its own scale", "[compare-source]") {
  // All round: a tenth each side, the same aspect; still 1:1, centred.
  const compare_map m = source_map(4800, 3600, 4000, 3000, compare_layout::extended);
  CHECK(m.clip);
  float su = 0, sv = 0;
  sample(m, 400.0f / 4800.0f, 300.0f / 3600.0f, su, sv);  // the source's top-left corner
  CHECK(su == Approx(0.0f).margin(1e-5));
  CHECK(sv == Approx(0.0f).margin(1e-5));
  sample(m, 4400.0f / 4800.0f, 3300.0f / 3600.0f, su, sv);
  CHECK(su == Approx(1.0f).margin(1e-5));
  CHECK(sv == Approx(1.0f).margin(1e-5));
  sample(m, 0.0f, 0.0f, su, sv);  // the new margin: outside the source
  CHECK(su < 0.0f);
  // Where the writer said it is.
  const compare_map at = source_map(6000, 3000, 4000, 3000, compare_layout::extended,
                                    compare_rect{0.0f, 0.0f, 4000.0f / 6000.0f, 1.0f});
  sample(at, 0.0f, 0.0f, su, sv);
  CHECK(su == Approx(0.0f).margin(1e-5));
  sample(at, 4000.0f / 6000.0f, 1.0f, su, sv);
  CHECK(su == Approx(1.0f).margin(1e-5));
  CHECK(sv == Approx(1.0f).margin(1e-5));
}

TEST_CASE("a different aspect is fitted inside the frame, centred", "[compare-source]") {
  // A square result of a 3:2 source (not an Extend): letterboxed.
  const compare_map m = source_map(3000, 3000, 3000, 2000);
  CHECK(m.clip);
  float su = 0, sv = 0;
  sample(m, 0.5f, 0.5f, su, sv);
  CHECK(su == Approx(0.5f));
  CHECK(sv == Approx(0.5f));
  sample(m, 0.0f, 0.5f - 1.0f / 3.0f, su, sv);
  CHECK(su == Approx(0.0f).margin(1e-5));
  CHECK(sv == Approx(0.0f).margin(1e-5));
  // An Extend whose source does not fit (a crop came with it): fitted too.
  const compare_map e = source_map(3000, 3000, 4000, 2000, compare_layout::extended);
  sample(e, 0.0f, 0.25f, su, sv);
  CHECK(su == Approx(0.0f).margin(1e-5));
  CHECK(sv == Approx(0.0f).margin(1e-5));
}

TEST_CASE("the divider lies across the picture's visible span", "[compare-source]") {
  // A fitted portrait in a wide view: the span is the picture.
  compare_frame f = compare_frame_of(0, 40, 1000, 600, 300, 40, 700, 640, 0.5f);
  REQUIRE(f.valid);
  CHECK(f.x0 == 300.0f);
  CHECK(f.x1 == 700.0f);
  CHECK(f.divider_x == 500.0f);
  // Zoomed in past the view: the span is the view.
  f = compare_frame_of(0, 40, 1000, 600, -2000, -500, 3000, 3000, 0.25f);
  REQUIRE(f.valid);
  CHECK(f.x0 == 0.0f);
  CHECK(f.x1 == 1000.0f);
  CHECK(f.y0 == 40.0f);
  CHECK(f.y1 == 640.0f);
  CHECK(f.divider_x == 250.0f);
  // Off screen: nothing to draw or grab.
  CHECK_FALSE(compare_frame_of(0, 0, 1000, 600, 1200, 0, 1600, 600, 0.5f).valid);
  CHECK_FALSE(divider_hit(compare_frame{}, 0, 0, 1));
}

TEST_CASE("a press near the divider grabs it, and a drag sets the split", "[compare-source]") {
  const compare_frame f = compare_frame_of(0, 0, 1000, 600, 200, 0, 800, 600, 0.5f);
  CHECK(divider_hit(f, 500, 300, 1.0f));
  CHECK(divider_hit(f, 500 + kCompareGrabDip, 10, 1.0f));
  CHECK_FALSE(divider_hit(f, 500 + kCompareGrabDip + 1, 10, 1.0f));
  CHECK(divider_hit(f, 500 + kCompareGrabDip + 1, 10, 2.0f));  // DIP, not pixels
  CHECK_FALSE(divider_hit(f, 500, 601, 1.0f));
  CHECK(split_at(f, 200) == 0.0f);
  CHECK(split_at(f, 650) == Approx(0.75f));
  CHECK(split_at(f, -50) == 0.0f);
  CHECK(split_at(f, 5000) == 1.0f);
}
