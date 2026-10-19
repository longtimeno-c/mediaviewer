// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/29 "Edits": the editor over the round-trip set. Ripple and lift
// cuts, connected clips that follow their content (and music that plays
// through), a batch in the timeline's own times, atomic failure, and every
// other operation; each result reads back as a timeline and a new version.
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nle/edit.h"
#include "nle/timeline.h"

using namespace mv::nle;

namespace {

std::string slurp_fixture(const std::string& name) {
  std::ifstream f(std::filesystem::path(MV_TIMELINE_FIXTURES) / name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

timeline load_fixture(const std::string& name) {
  timeline t;
  std::string error;
  const bool ok = t.load(slurp_fixture(name), &error);
  INFO(error);
  REQUIRE(ok);
  return t;
}

rational r(std::int64_t n, std::int64_t d = 1) { return reduce({n, d}); }
rational sec(double s) { return reduce({static_cast<std::int64_t>(s * 1000 + (s < 0 ? -0.5 : 0.5)), 1000}); }

bool run(timeline& t, const std::string& ops_json, std::string* error = nullptr,
         const std::vector<std::pair<std::string, media_probe>>& probes = {}, edit_options o = {false, true}) {
  std::vector<edit_op> ops;
  std::string why;
  if (!parse_ops(ops_json, ops, &why)) {
    if (error) *error = why;
    return false;
  }
  editor e(t, o);
  const bool ok = e.apply(ops, probes, &why);
  if (error) *error = why;
  return ok;
}

// The edited document still reads as a timeline, and so does its new version.
void reads_back(const timeline& t) {
  timeline again;
  std::string error;
  REQUIRE(again.load(t.doc().write(), &error));
  CHECK(again.describe() == t.describe());
  timeline v;
  REQUIRE(v.load(t.write_version("X (Claude 1)"), &error));
  const std::string a = t.describe(), b = v.describe();
  CHECK(a.substr(a.find('\n')) == b.substr(b.find('\n')));
}

const item* named(const timeline& t, std::string_view name, int nth = 0) {
  for (const item& it : t.items()) {
    if (it.name == name && nth-- == 0) return &it;
  }
  return nullptr;
}

std::vector<const item*> primary(const timeline& t) {
  std::vector<const item*> out;
  for (const item& it : t.items()) {
    if (it.parent == -1 && it.in_storyline) out.push_back(&it);
  }
  return out;
}

}  // namespace

TEST_CASE("clock text", "[timeline][edit]") {
  CHECK(parse_clock("12.5") == r(25, 2));
  CHECK(parse_clock("1:02.04") == r(6204, 100));
  CHECK(parse_clock("1:02:03.25") == r(372325, 100));
  CHECK(parse_clock("750/25s") == r(30));
  CHECK(parse_clock("5s") == r(5));
  CHECK_FALSE(parse_clock("1:2:3:4").has_value());
  CHECK_FALSE(parse_clock("abc").has_value());
  CHECK(format_clock(r(6204, 100)) == "1:02.04");
  CHECK(format_clock(r(3723)) == "1:02:03.00");
}

TEST_CASE("a batch that does not parse says why", "[timeline][edit]") {
  std::vector<edit_op> ops;
  std::string error;
  CHECK_FALSE(parse_ops("{}", ops, &error));
  CHECK_FALSE(parse_ops(R"([{"op":"explode"}])", ops, &error));
  CHECK(error.find("unknown op") != std::string::npos);
  CHECK_FALSE(parse_ops(R"([{"op":"cut","from":"1"}])", ops, &error));
  CHECK_FALSE(parse_ops(R"([{"op":"trim","from":"1","to":"2"}])", ops, &error));  // no item
  CHECK_FALSE(parse_ops(R"([{"op":"volume","item":1,"db":40}])", ops, &error));
  CHECK(parse_ops(R"([{"op":"cut","from":"0:01.5","to":2.25},{"op":"marker","at":3,"text":"x","todo":true}])", ops,
                  &error));
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].from == r(3, 2));
  CHECK(ops[0].to == r(9, 4));
  CHECK(ops[1].todo);
}

TEST_CASE("a ripple cut inside a clip splits it and closes the gap", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const rational before = t.length();
  const item c2 = *named(t, "A001_C002");  // at 16.48 s, 50 s long, from 40 s of its file
  REQUIRE(run(t, R"([{"op":"cut","from":"20","to":"21.2"}])"));
  CHECK(t.length() == *sub(before, sec(1.2)));
  const auto line = primary(t);
  REQUIRE(line.size() == 4);
  CHECK(line[1]->name == "A001_C002");
  CHECK(line[1]->duration == *sub(r(20), c2.at));
  CHECK(line[2]->name == "A001_C002");
  CHECK(line[2]->at == r(20));
  CHECK(line[2]->source_in == *add(c2.source_in, *sub(sec(21.2), c2.at)));
  CHECK(t.project().duration == t.length());  // the sequence's duration follows
  reads_back(t);
}

TEST_CASE("cuts in one batch are in the timeline's times before the batch", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const rational before = t.length();
  REQUIRE(run(t, R"([{"op":"cut","from":"10","to":"11"},{"op":"cut","from":"30","to":"31"},
                     {"op":"cut","from":"30.5","to":"32"}])"));
  // The third overlaps the second: together 1 + 2 s, from where they were.
  CHECK(t.length() == *sub(before, r(3)));
  const auto line = primary(t);
  // 30 s of the original is now at 29 s, and 32 s is too (30-32 is gone).
  bool found = false;
  for (const item* it : line) found = found || it->at == r(29);
  CHECK(found);
  reads_back(t);
}

TEST_CASE("a cut across an edit, and a whole clip", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const rational before = t.length();
  // A001_C001 ends at 16.48 s.
  REQUIRE(run(t, R"([{"op":"cut","from":"15","to":"18"}])"));
  CHECK(t.length() == *sub(before, r(3)));
  CHECK(primary(t).size() == 3);
  CHECK(primary(t)[1]->at == r(15));
  REQUIRE(run(t, R"([{"op":"remove","item":0}])"));
  CHECK(primary(t).size() == 2);
  CHECK(primary(t)[0]->name == "A001_C002");
  CHECK(primary(t)[0]->at == r(0));
  reads_back(t);
}

TEST_CASE("a lift leaves a gap and moves nothing", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const rational before = t.length();
  REQUIRE(run(t, R"([{"op":"cut","from":"20","to":"22","ripple":false}])"));
  CHECK(t.length() == before);
  const auto line = primary(t);
  REQUIRE(line.size() == 5);
  CHECK(line[2]->kind == item_kind::gap);
  CHECK(line[2]->at == r(20));
  CHECK(line[2]->duration == r(2));
  CHECK(line[3]->at == r(22));
  reads_back(t);
}

TEST_CASE("connected clips follow their content; music plays through", "[timeline][edit]") {
  timeline t = load_fixture("03-connected-broll.fcpxml");
  // Talk_01 0-100 s; GX010042 lane 1 at 20 s for 5 s; GX010043 at 36 s; music lane -1 under all of it.
  REQUIRE(run(t, R"([{"op":"cut","from":"21","to":"23"}])"));
  CHECK(t.length() == r(103));
  const item* first = named(t, "GX010042", 0);
  const item* rest = named(t, "GX010042", 1);
  REQUIRE(first);
  REQUIRE(rest);
  CHECK(first->at == r(20));
  CHECK(first->duration == r(1));
  CHECK(rest->at == r(21));
  CHECK(rest->duration == r(2));
  CHECK(rest->source_in == *add(r(4), r(3)));  // it skipped what was cut
  CHECK(rest->lane == 1);
  CHECK(named(t, "GX010043")->at == r(34));
  const item* music = named(t, "Dolomites Theme");
  REQUIRE(music);
  CHECK(music->duration == r(98));  // shorter by the cut, not cut in two
  CHECK(named(t, "Dolomites Theme", 1) == nullptr);
  reads_back(t);
}

TEST_CASE("a connected clip that starts inside a cut keeps what is after it", "[timeline][edit]") {
  timeline t = load_fixture("03-connected-broll.fcpxml");
  REQUIRE(run(t, R"([{"op":"cut","from":"19","to":"22"}])"));
  const item* b = named(t, "GX010042");
  REQUIRE(b);
  CHECK(named(t, "GX010042", 1) == nullptr);
  CHECK(b->at == r(19));
  CHECK(b->duration == r(3));
  CHECK(b->source_in == r(6));
  reads_back(t);
}

TEST_CASE("cutting a clip away re-attaches what was connected after the cut", "[timeline][edit]") {
  timeline t = load_fixture("20-long-interview-broll.fcpxml");
  // Each 40 s talk clip has a drone shot connected 8 s in; cut 0:39-0:41
  // (the end of the first and the start of the second).
  const rational before = t.length();
  REQUIRE(run(t, R"([{"op":"cut","from":"39","to":"41"}])"));
  CHECK(t.length() == *sub(before, r(2)));
  const item* d2 = named(t, "Drone 02");
  REQUIRE(d2);
  CHECK(d2->at == r(46));  // was 48 s
  reads_back(t);
}

TEST_CASE("a connected storyline across a cut refuses it", "[timeline][edit]") {
  timeline t = load_fixture("04-connected-storyline.fcpxml");
  const std::string before = t.doc().write();
  std::string error;
  CHECK_FALSE(run(t, R"([{"op":"cut","from":"10","to":"11"}])", &error));
  CHECK(error.find("connected storyline") != std::string::npos);
  CHECK(t.doc().write() == before);
  // Outside it, fine.
  REQUIRE(run(t, R"([{"op":"cut","from":"40","to":"41"}])"));
  reads_back(t);
}

TEST_CASE("a batch is all or nothing", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const std::string before = t.doc().write();
  std::string error;
  CHECK_FALSE(run(t, R"([{"op":"cut","from":"1","to":"2"},{"op":"split","item":0,"at":"500"}])", &error));
  CHECK(error.find("op 1") != std::string::npos);
  CHECK(error.find("nothing in this batch was applied") != std::string::npos);
  CHECK(t.doc().write() == before);
  CHECK_FALSE(run(t, R"([{"op":"move","item":0},{"op":"cut","from":"1","to":"2"}])", &error));
  CHECK(t.doc().write() == before);
}

TEST_CASE("what MediaViewer does not open is moved and trimmed only as a unit", "[timeline][edit]") {
  timeline t = load_fixture("05-compound.fcpxml");
  // A cut inside the compound trims or splits it, never opens it.
  REQUIRE(run(t, R"([{"op":"cut","from":"2","to":"3"}])"));
  CHECK(t.items()[0].kind == item_kind::ref_clip);
  reads_back(t);
  timeline a = load_fixture("14-audition.fcpxml");
  std::string error;
  CHECK_FALSE(run(a, R"([{"op":"cut","from":"10","to":"11"}])", &error));
  CHECK(error.find("audition") != std::string::npos);
}

TEST_CASE("split, move, trim, lengthen", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  const rational length = t.length();
  REQUIRE(run(t, R"([{"op":"split","item":1,"at":"30"}])"));
  CHECK(primary(t).size() == 4);
  CHECK(t.length() == length);
  REQUIRE(run(t, R"([{"op":"move","item":3,"before":0}])"));
  CHECK(primary(t)[0]->name == "A001_C003");
  CHECK(t.length() == length);
  // Trim the first clip to 2-10 s of where it sits.
  REQUIRE(run(t, R"([{"op":"trim","item":0,"from":"2","to":"10"}])"));
  CHECK(primary(t)[0]->duration == r(8));
  CHECK(t.length() == *sub(length, *sub(primary(t)[0]->duration.num ? r(733, 25) : r(0), r(8))));
  // A001_C001 starts 10 s into its file: 2 s more at the head is there, 20 s is not.
  const int c1 = [&] {
    for (std::size_t i = 0; i < t.items().size(); ++i) {
      if (t.items()[i].name == "A001_C001") return static_cast<int>(i);
    }
    return -1;
  }();
  REQUIRE(c1 >= 0);
  const rational d = t.items()[static_cast<std::size_t>(c1)].duration;
  REQUIRE(run(t, R"([{"op":"trim","item":)" + std::to_string(c1) + R"(,"head":"-2"}])"));
  CHECK(t.items()[static_cast<std::size_t>(c1)].duration == *add(d, r(2)));
  std::string error;
  CHECK_FALSE(run(t, R"([{"op":"trim","item":)" + std::to_string(c1) + R"(,"head":"-20"}])", &error));
  CHECK(error.find("of media before it") != std::string::npos);
  reads_back(t);
}

TEST_CASE("insert and connect from the scope, another take, markers, text, volume", "[timeline][edit]") {
  timeline t = load_fixture("07-titles.fcpxml");
  media_probe broll;
  broll.duration = r(30);
  broll.has_video = broll.has_audio = true;
  broll.width = 1920;
  broll.height = 1080;
  broll.frame = r(1, 25);
  const std::vector<std::pair<std::string, media_probe>> probes = {{"/Users/editor/Footage/broll/Lake & Sky.mov", broll}};
  const rational length = t.length();
  REQUIRE(run(t,
              R"([{"op":"connect","path":"/Users/editor/Footage/broll/Lake & Sky.mov","source_in":"2","duration":"3","at":"10"},
                  {"op":"insert","path":"/Users/editor/Footage/broll/Lake & Sky.mov","source_in":"10","duration":"4"},
                  {"op":"marker","at":"12","text":"check the pan","todo":true},
                  {"op":"title","item":0,"text":"Dolomites, 2026"},
                  {"op":"volume","item":1,"db":-3},
                  {"op":"enable","item":2,"on":false}])",
              nullptr, probes));
  CHECK(t.length() == *add(length, r(4)));
  const item* c = named(t, "Lake & Sky");
  REQUIRE(c);
  CHECK(c->lane == 1);
  CHECK(c->at == r(10));
  CHECK(c->source == "/Users/editor/Footage/broll/Lake & Sky.mov");
  CHECK(t.doc().write().find("srcEnable=\"video\"") != std::string::npos);
  const item* ins = named(t, "Lake & Sky", 1);
  REQUIRE(ins);
  CHECK(ins->in_storyline);
  CHECK(ins->at == length);
  CHECK(t.items()[0].text == "Dolomites, 2026");
  CHECK(t.doc().write().find("adjust-volume amount=\"-3.0dB\"") != std::string::npos);
  bool marked = false;
  for (const item& it : t.items()) {
    for (const marker& m : it.markers) marked = marked || (m.value == "check the pan" && m.todo && m.at == r(12));
  }
  CHECK(marked);
  // The new asset is in the resources once, with a format.
  CHECK(t.assets().size() == 2);
  reads_back(t);

  timeline s = load_fixture("01-interview-simple.fcpxml");
  REQUIRE(run(s, R"([{"op":"source","item":2,"source_in":"100"}])"));
  CHECK(primary(s)[2]->source_in == r(100));
  std::string error;
  CHECK_FALSE(run(s, R"([{"op":"source","item":2,"source_in":"290"}])", &error));  // past the end of the file
  reads_back(s);
}

TEST_CASE("changes are listed and marked", "[timeline][edit]") {
  timeline t = load_fixture("01-interview-simple.fcpxml");
  std::vector<edit_op> ops;
  REQUIRE(parse_ops(R"([{"op":"cut","from":"20","to":"21"},{"op":"cut","from":"40","to":"40.5"}])", ops, nullptr));
  editor e(t);
  REQUIRE(e.apply(ops, {}, nullptr));
  REQUIRE(e.changes().size() == 2);
  CHECK(e.changes()[0].removed == r(1));
  CHECK(e.changes()[1].at == r(39));
  int claude = 0;
  for (const item& it : t.items()) {
    for (const marker& m : it.markers) claude += m.value.rfind("Claude: ", 0) == 0;
  }
  CHECK(claude == 2);
  reads_back(t);
}

TEST_CASE("every project in the set takes a cut and reads back", "[timeline][edit]") {
  for (const auto& entry : std::filesystem::directory_iterator(MV_TIMELINE_FIXTURES)) {
    if (entry.path().extension() != ".fcpxml") continue;
    INFO(entry.path().filename().string());
    timeline t;
    REQUIRE(t.load(slurp_fixture(entry.path().filename().string()), nullptr));
    const rational len = t.length();
    // A frame-aligned second in the middle; some refuse (an audition, a storyline): then nothing changes.
    const rational mid = snap(reduce({len.num, len.den * 2}), t.project().frame.num ? t.project().frame : r(1, 25)).value();
    const rational frame = t.project().frame.num ? t.project().frame : r(1, 25);
    const rational end = snap(*add(mid, r(1)), frame).value();
    const std::string ops = "[{\"op\":\"cut\",\"from\":\"" + format_time(mid) + "\",\"to\":\"" +
                            format_time(end) + "\"}]";
    const std::string before = t.doc().write();
    std::string error;
    if (run(t, ops, &error, {}, {true, true})) {
      CHECK(t.length() == *sub(len, *sub(end, mid)));
      reads_back(t);
    } else {
      CHECK(t.doc().write() == before);
    }
  }
}
