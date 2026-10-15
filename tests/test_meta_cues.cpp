// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #406: a clip's chapter titles and text subtitle cues (meta/cues.h), read from
// files this test writes (tests/cue_fixture.h), for Local search's index.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "cue_fixture.h"
#include "meta/cues.h"

namespace {

namespace fs = std::filesystem;
namespace meta = mv::meta;

struct temp_dir {
  fs::path path;
  explicit temp_dir(const char* name) : path(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::vector<meta::cue> read_all(const std::string& path, meta::cue_limits lim = {}) {
  std::vector<meta::cue> out;
  const mv::expected r = meta::read_cues(path, lim, [&](const meta::cue& c) {
    out.push_back(c);
    return true;
  });
  REQUIRE(r);
  return out;
}

}  // namespace

TEST_CASE("a subtitle packet's text, without its markup", "[cues]") {
  CHECK(meta::cue_text("subrip", "<i>suited</i> and\nbooted") == "suited and booted");
  CHECK(meta::cue_text("subrip", "<font color=\"#ffff00\">Hello</font>") == "Hello");
  CHECK(meta::cue_text("webvtt", "<c.yellow>hi</c> <00:00:01.000>there") == "hi there");
  CHECK(meta::cue_text("ass", "0,0,Default,,0,0,0,,{\\i1}suited{\\i0} and\\Nbooted") == "suited and booted");
  CHECK(meta::cue_text("ass", "no commas here").empty());
  const std::string tx3g = std::string("\x00\x05", 2) + "hello" + "styl-box-bytes";
  CHECK(meta::cue_text("mov_text", tx3g) == "hello");
  CHECK(meta::cue_text("mov_text", std::string("\x00\x40", 2) + "short").empty());  // length past the end
  CHECK(meta::cue_text("subrip", "1\n00:00:01,000 --> 00:00:02,000\nold style") == "old style");
  CHECK(meta::cue_text("subrip", std::string("\xFF\xFE bad", 6)).empty());  // not UTF-8: skipped
  CHECK(meta::cue_text("subrip", "<b></b>").empty());
  CHECK(meta::cue_text("text", "Grüße  aus\tKöln") == "Grüße aus Köln");
}

TEST_CASE("chapters and SubRip cues come back in start order, on the player's timeline", "[cues]") {
  temp_dir dir("mv-cues-mkv");
  const std::string path = (dir.path / "chapters.mkv").string();
  REQUIRE(mv::test::cuefx::make(path, "matroska",
                                {{0, 4000, "Arrival"}, {4000, 9000, "Birthday cake"}},
                                {{2000, 3000, "hello there"}, {7000, 8500, "<i>suited</i> and booted"}}));
  const auto cues = read_all(path);
  REQUIRE(cues.size() == 4);
  CHECK(cues[0].kind == meta::cue_kind::chapter);
  CHECK(cues[0].text == "Arrival");
  CHECK(cues[1].kind == meta::cue_kind::subtitle);
  CHECK(cues[1].start_ms == 2000);
  CHECK(cues[2].kind == meta::cue_kind::chapter);
  CHECK(cues[2].text == "Birthday cake");
  CHECK(cues[2].start_ms == 4000);
  CHECK(cues[3].kind == meta::cue_kind::subtitle);
  CHECK(cues[3].text == "suited and booted");
  CHECK(cues[3].start_ms == 7000);
  CHECK(cues[3].end_ms == 8500);

  SECTION("a resume reads from its start") {
    meta::cue_limits lim;
    lim.start_ms = 4000;
    const auto rest = read_all(path, lim);
    REQUIRE(rest.size() == 2);
    CHECK(rest[0].text == "Birthday cake");
    CHECK(rest[1].text == "suited and booted");
  }
  SECTION("the caps stop it") {
    meta::cue_limits lim;
    lim.max_cues = 2;
    CHECK(read_all(path, lim).size() == 2);
    lim.max_cues = 8000;
    lim.max_ms = 3000;
    CHECK(read_all(path, lim).size() == 2);  // Arrival and "hello there"
  }
  SECTION("the visitor stops it") {
    int seen = 0;
    const mv::expected r = meta::read_cues(path, {}, [&](const meta::cue&) { return ++seen < 2; });
    CHECK_FALSE(r);
    CHECK(r.error() == mv::status::cancelled);
    CHECK(seen == 2);
  }
}

TEST_CASE("mov_text cues in an MP4", "[cues]") {
  temp_dir dir("mv-cues-mp4");
  const std::string path = (dir.path / "subs.mp4").string();
  if (!mv::test::cuefx::make(path, "mp4", {}, {{1000, 2000, "suited and booted"}})) {
    SKIP("this FFmpeg build cannot mux a lone mov_text track");
  }
  const auto cues = read_all(path);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].kind == meta::cue_kind::subtitle);
  CHECK(cues[0].text == "suited and booted");
  CHECK(cues[0].start_ms == 1000);
}

TEST_CASE("a clip with no chapters and no text subtitles has no cues", "[cues]") {
  temp_dir dir("mv-cues-none");
  const std::string path = (dir.path / "plain.mp4").string();
  mv::test::clipfx::spec s;
  s.frames = 10;
  REQUIRE(mv::test::clipfx::make(path, s));
  CHECK(read_all(path).empty());
  CHECK_FALSE(meta::read_cues((dir.path / "missing.mkv").string(), {}, [](const meta::cue&) { return true; }));
}
