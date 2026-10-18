// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/29: a Final Cut Pro project as a timeline model. The lossless XML
// tree (what is not edited comes back byte for byte), exact FCPXML time, the
// model over the round-trip set (tests/data/fcpxml, made by
// tools/timeline/make_fixtures.py and checked against FCP's DTDs), and the new
// version a pass writes beside the original.
#include "catch_compat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nle/bridge_state.h"
#include "nle/rational.h"
#include "nle/timeline.h"
#include "nle/xml.h"

using namespace mv::nle;

namespace {

std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::filesystem::path fixtures() { return std::filesystem::path(MV_TIMELINE_FIXTURES); }

std::vector<std::filesystem::path> fixture_files() {
  std::vector<std::filesystem::path> out;
  for (const auto& e : std::filesystem::directory_iterator(fixtures())) {
    if (e.path().extension() == ".fcpxml") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

timeline load(const std::string& name, std::string_view project = {}) {
  timeline t;
  std::string error;
  const bool ok = t.load(slurp(fixtures() / name), &error, project);
  INFO(error);
  REQUIRE(ok);
  return t;
}

const item& find(const timeline& t, std::string_view name, item_kind k) {
  const item* hit = nullptr;
  for (const item& it : t.items()) {
    if (it.name == name && it.kind == k) {
      hit = &it;
      break;
    }
  }
  INFO(std::string(name));
  REQUIRE(hit != nullptr);
  return *hit;
}

rational r(std::int64_t n, std::int64_t d = 1) { return reduce({n, d}); }
rational operator+(rational a, rational b) { return *add(a, b); }

}  // namespace

TEST_CASE("FCPXML time is exact", "[timeline][rational]") {
  CHECK(parse_time("1001/30000s") == r(1001, 30000));
  CHECK(parse_time("5s") == r(5));
  CHECK(parse_time("0s") == r(0));
  CHECK(parse_time("2.5s") == r(5, 2));
  CHECK(parse_time("-3600s") == r(-3600));
  CHECK_FALSE(parse_time("").has_value());
  CHECK_FALSE(parse_time("1/0s").has_value());
  CHECK_FALSE(parse_time("abc").has_value());
  CHECK(format_time(r(1001, 30000)) == "1001/30000s");
  CHECK(format_time(r(100, 25)) == "4s");
  CHECK(format_time(r(0)) == "0s");
  // Adding thirty NTSC frames thirty thousand times lands exactly on 30030 s.
  rational acc{};
  for (int i = 0; i < 30000; ++i) acc = *add(acc, r(1001, 1000));
  CHECK(acc == r(30030));
  CHECK(*sub(r(1, 25), r(1001, 30000)) == r(199, 30000));
  // Snapping to a frame grid.
  CHECK(*snap(r(1234, 1000), r(1, 25)) == r(31, 25));  // 1.234 s -> frame 31 (1.24 s)
  CHECK(*snap(r(1, 50), r(1, 25)) == r(1, 25));        // half a frame rounds up
  CHECK(to_ms(r(1001, 30000)) == 33);
  CHECK(from_ms(1500) == r(3, 2));
  // Overflow is reported, not wrapped.
  CHECK_FALSE(add(r(INT64_MAX - 1), r(5)).has_value());
}

TEST_CASE("the XML tree writes back what it read, byte for byte", "[timeline][xml]") {
  const std::vector<std::string> docs = {
      "<a/>",
      "\xEF\xBB\xBF<?xml version='1.0'?>\r\n<!-- c --><a x='1'  y = \"2\" >t&amp;x&#169;<b/><![CDATA[<raw>]]></a>\n",
      "<!DOCTYPE x [ <!ELEMENT x ANY> <!ATTLIST x a CDATA \">\"> ]><x a=\"&lt;&gt;&quot;&apos;\"/>",
      "<?xml version=\"1.0\"?>\n<!DOCTYPE fcpxml>\n\n<fcpxml version=\"1.13\">\n\t<x\n   a=\"1\"\n/>\n</fcpxml>\n",
  };
  for (const std::string& s : docs) {
    xml::document d;
    xml::parse_error e;
    INFO(s);
    REQUIRE(d.parse(s, &e));
    CHECK(d.write() == s);
  }
}

TEST_CASE("the XML tree refuses what is not well formed", "[timeline][xml]") {
  const std::vector<std::string> bad = {
      "",
      "<a>",
      "<a></b>",
      "<a/><b/>",
      "text<a/>",
      "<a x=1/>",
      "<a x='1' x='2'/>",
      "<a x='<'/>",
      "<a>&nbsp;</a>",
      "<a>& b</a>",
      "<a><!-- open </a>",
      "<!ENTITY x 'y'><a/>",
      "<a>&#0;</a>",
  };
  for (const std::string& s : bad) {
    xml::document d;
    xml::parse_error e;
    INFO(s);
    CHECK_FALSE(d.parse(s, &e));
    CHECK_FALSE(e.message.empty());
  }
}

TEST_CASE("an edit rewrites only what it touched", "[timeline][xml]") {
  const std::string s =
      "<root>\n  <clip  name='A &amp; B'\toffset=\"0s\" duration=\"5s\"/>\n  <clip name=\"C\" duration=\"2s\">\n"
      "    <marker start=\"1s\"/>\n  </clip>\n</root>\n";
  xml::document d;
  REQUIRE(d.parse(s, nullptr));
  const auto clips = d.elements(d.root(), "clip");
  REQUIRE(clips.size() == 2);
  CHECK(*d.attr(clips[0], "name") == "A & B");
  d.set_attr(clips[0], "duration", "4s");
  // The other attributes keep their quotes, spacing and entities.
  CHECK(d.write() ==
        "<root>\n  <clip  name='A &amp; B'\toffset=\"0s\" duration=\"4s\"/>\n  <clip name=\"C\" duration=\"2s\">\n"
        "    <marker start=\"1s\"/>\n  </clip>\n</root>\n");
  // A self-closing element that gains a child is written open.
  const std::uint32_t m = d.make_element("marker");
  d.set_attr(m, "start", "0s");
  d.insert(clips[0], m);
  CHECK(d.write(clips[0]) == "<clip  name='A &amp; B'\toffset=\"0s\" duration=\"4s\"><marker start=\"0s\"/></clip>");
  // Removing a node takes its indentation with it.
  d.remove(clips[1]);
  CHECK(d.write() ==
        "<root>\n  <clip  name='A &amp; B'\toffset=\"0s\" duration=\"4s\"><marker start=\"0s\"/></clip>\n</root>\n");
  // Inserting copies the indentation of its neighbour; a clone of unchanged
  // source writes its source bytes.
  xml::document e;
  REQUIRE(e.parse(s, nullptr));
  const auto ec = e.elements(e.root(), "clip");
  const std::uint32_t copy = e.clone(ec[1]);
  e.insert(e.root(), copy, ec[0]);
  CHECK(e.write() ==
        "<root>\n  <clip name=\"C\" duration=\"2s\">\n    <marker start=\"1s\"/>\n  </clip>\n  <clip  name='A &amp; B'"
        "\toffset=\"0s\" duration=\"5s\"/>\n  <clip name=\"C\" duration=\"2s\">\n    <marker start=\"1s\"/>\n  </clip>\n"
        "</root>\n");
  // Text is escaped when written afresh.
  xml::document t;
  REQUIRE(t.parse("<title><text>Old</text></title>", nullptr));
  t.set_text(t.first(t.root(), "text"), "Fish & <Chips>");
  CHECK(t.write() == "<title><text>Fish &amp; &lt;Chips&gt;</text></title>");
  t.set_attr(t.root(), "name", "a\"b\nc");
  CHECK(t.write() == "<title name=\"a&quot;b&#10;c\"><text>Fish &amp; &lt;Chips&gt;</text></title>");
}

TEST_CASE("every project in the round-trip set comes back byte for byte", "[timeline][roundtrip]") {
  const auto files = fixture_files();
  REQUIRE(files.size() == 20);
  for (const auto& f : files) {
    INFO(f.filename().string());
    const std::string text = slurp(f);
    timeline t;
    std::string error;
    REQUIRE(t.load(text, &error));
    CHECK(t.doc().write() == text);
    CHECK_FALSE(t.items().empty());
    CHECK(t.length() > rational{});
  }
}

TEST_CASE("a new version imports as the same timeline, beside the original", "[timeline][roundtrip]") {
  for (const auto& f : fixture_files()) {
    INFO(f.filename().string());
    timeline t;
    REQUIRE(t.load(slurp(f), nullptr));
    const std::string name = next_version_name(t.project().name, t.project_names());
    const std::string out = t.write_version(name);
    timeline v;
    std::string error;
    REQUIRE(v.load(out, &error));
    // Renamed, no uid (FCP makes a new project, never replaces the original),
    // the same event and library, one project.
    CHECK(v.project().name == name);
    CHECK(v.project().uid.empty());
    CHECK(v.doc().attr(v.project().node, "modDate") == nullptr);
    CHECK(v.project().event_name == t.project().event_name);
    CHECK(v.project().library == t.project().library);
    CHECK(v.project_names().size() == 1);
    // The same clip list with the same timing, and every item's bytes
    // (effects, colour, keyframes, audio components) unchanged.
    const std::string a = t.describe(), b = v.describe();
    CHECK(a.substr(a.find('\n')) == b.substr(b.find('\n')));
    REQUIRE(t.items().size() == v.items().size());
    for (std::size_t i = 0; i < t.items().size(); ++i) {
      CHECK(t.doc().write(t.items()[i].node) == v.doc().write(v.items()[i].node));
    }
    // The sequence (format, duration, audio layout) is untouched.
    CHECK(t.doc().write(t.project().sequence) == v.doc().write(v.project().sequence));
    // Every asset the timeline uses is still there, by the same uid.
    for (const auto& [id, a2] : t.assets()) {
      bool used = false;
      for (const std::string& p : t.media_paths()) used = used || p == a2.path;
      if (!used) continue;
      const auto it = v.assets().find(id);
      REQUIRE(it != v.assets().end());
      CHECK(it->second.uid == a2.uid);
    }
  }
}

TEST_CASE("the model reads what FCP put on the timeline", "[timeline][model]") {
  SECTION("primary storyline, markers, to-dos, gaps, a disabled clip") {
    const timeline t = load("02-gaps-markers.fcpxml");
    CHECK(t.project().name == "Interview with gaps & markers");
    CHECK(t.project().frame == r(1, 25));
    const item& c = find(t, "B002_C010", item_kind::asset_clip);
    CHECK(c.at == r(2));
    CHECK(c.source == "/Users/editor/Footage/Interview/B002_C010.mov");
    CHECK(c.source_in == r(3));
    CHECK(c.role == "dialogue");
    REQUIRE(c.markers.size() == 3);
    CHECK(c.markers[0].value == "cut the um here");
    CHECK(c.markers[0].todo);
    CHECK_FALSE(c.markers[0].completed);
    CHECK(c.markers[1].completed);
    CHECK(c.markers[0].at == r(7));
    CHECK(c.markers[2].kind == "chapter-marker");
    CHECK_FALSE(find(t, "B002_C011", item_kind::asset_clip).enabled);
    CHECK(t.items()[0].kind == item_kind::gap);
    CHECK(t.length() == r(39) + r(128, 5));
  }
  SECTION("connected clips above and below") {
    const timeline t = load("03-connected-broll.fcpxml");
    const item& b = find(t, "GX010042", item_kind::asset_clip);
    CHECK(b.lane == 1);
    CHECK_FALSE(b.in_storyline);
    CHECK(b.at == r(20));
    CHECK(b.role == "video.B-roll, effects");
    CHECK(b.has_effects == false);  // a volume adjustment is not a filter
    const item& m = find(t, "Dolomites Theme", item_kind::asset_clip);
    CHECK(m.lane == -1);
    CHECK(m.source == "/Users/editor/Music/Dolomites Theme.m4a");
    CHECK(t.items()[static_cast<std::size_t>(b.parent)].name == "Talk_01");
  }
  SECTION("a connected storyline with a transition") {
    const timeline t = load("04-connected-storyline.fcpxml");
    const item& s = t.items()[1];
    CHECK(s.kind == item_kind::storyline);
    CHECK_FALSE(s.editable);
    CHECK(s.at == r(8));
    const item& p2 = find(t, "Peak 2", item_kind::asset_clip);
    CHECK(p2.in_storyline);
    CHECK(p2.storyline == 1);
    CHECK(p2.at == r(8) + r(4));
    CHECK(find(t, "Peak 3", item_kind::asset_clip).at == r(8) + r(10));
  }
  SECTION("compound clips are a unit; their media is the timeline's") {
    const timeline t = load("05-compound.fcpxml");
    const item& c = t.items()[0];
    CHECK(c.kind == item_kind::ref_clip);
    CHECK(c.opaque);
    CHECK(c.editable);
    CHECK(c.markers.size() == 1);
    CHECK(c.markers[0].at == r(4));
    CHECK(find(t, "Title bed", item_kind::asset_clip).at == r(2));
    const auto paths = t.media_paths();
    CHECK(std::find(paths.begin(), paths.end(), "/Users/editor/Footage/Compound/C1.mov") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/Users/editor/Footage/Compound/C2.mov") != paths.end());
    // C1 and C2 are inside the compound: not items of this timeline.
    for (const item& it : t.items()) CHECK(it.name != "C1");
    CHECK(find(t, "Opening compound", item_kind::ref_clip).name == "Opening compound");
    CHECK(t.items().back().has_effects);
  }
  SECTION("multicam: angles' media, positions from the sequence start") {
    const timeline t = load("06-multicam.fcpxml");
    CHECK(t.project().tc_start == r(3600));
    CHECK(t.items()[0].kind == item_kind::mc_clip);
    CHECK(t.items()[0].at == r(0));
    CHECK(t.items()[1].at == r(600 * 1001, 30000));
    const auto paths = t.media_paths();
    CHECK(paths.size() == 3);
    CHECK(t.items()[0].has_effects);  // angle choices (mc-source) are carried as effects
  }
  SECTION("titles and their text") {
    const timeline t = load("07-titles.fcpxml");
    CHECK(t.items()[0].kind == item_kind::title);
    CHECK(t.items()[0].text == "Dolomites 2026");
    const item& lt = find(t, "Anna Weber - Basic Lower Third", item_kind::title);
    CHECK(lt.text == "Anna Weber\nMountain guide");
    CHECK(lt.lane == 1);
    CHECK(lt.at == r(4) + r(2));
    CHECK(t.resource_names().at(lt.ref) == "Basic Lower Third");
  }
  SECTION("colour, effects, keyframes") {
    const timeline t = load("08-colour-effects.fcpxml");
    CHECK(t.items()[0].has_effects);
    CHECK(t.items()[1].has_effects);
    CHECK(t.project().width == 3840);
  }
  SECTION("split edits") {
    const timeline t = load("10-audio-components.fcpxml");
    const item& q2 = find(t, "Q2", item_kind::asset_clip);
    CHECK(q2.split_audio);
    CHECK(q2.audio_duration == r(18));
    CHECK_FALSE(find(t, "Q1", item_kind::asset_clip).split_audio);
  }
  SECTION("roles on audio and video elements") {
    const timeline t = load("11-roles.fcpxml");
    CHECK(find(t, "VO take 3", item_kind::audio).role == "dialogue.VO");
    CHECK(find(t, "VO take 3", item_kind::audio).lane == -1);
    CHECK(find(t, "Boat (video only)", item_kind::video).role == "video.Inserts");
  }
  SECTION("retiming is carried, flagged") {
    const timeline t = load("12-retiming.fcpxml");
    CHECK(find(t, "Jump 120fps", item_kind::asset_clip).retimed);
    CHECK(find(t, "Jump 120fps", item_kind::asset_clip).duration == r(10));
  }
  SECTION("sync clips and auditions are units") {
    const timeline s = load("13-sync-clip.fcpxml");
    CHECK(s.items()[0].kind == item_kind::sync_clip);
    CHECK(s.items()[0].opaque);
    CHECK(s.items().size() == 1);  // its recorder is inside it, not opened
    CHECK(s.media_paths().size() == 2);
    const timeline a = load("14-audition.fcpxml");
    CHECK(a.items()[1].kind == item_kind::audition);
    CHECK(a.items()[2].at == r(20));
    const auto paths = a.media_paths();
    CHECK(paths.size() == 3);
  }
  SECTION("captions") {
    const timeline t = load("15-captions.fcpxml");
    int n = 0;
    for (const item& it : t.items()) n += it.kind == item_kind::caption;
    CHECK(n == 3);
    CHECK(find(t, "It’s 2,236 m up.", item_kind::caption).text == "It’s 2,236 m up.");
  }
  SECTION("NTSC drop frame") {
    const timeline t = load("16-ntsc-drop-frame.fcpxml");
    CHECK(t.project().frame == r(1001, 30000));
    CHECK(t.items()[1].at == r(901 * 1001, 30000));
    CHECK(t.items()[0].markers[0].at == r(15 * 1001, 30000));
  }
  SECTION("an event with two projects: the first, or the one named") {
    const timeline t = load("17-event-two-projects.fcpxml");
    CHECK(t.project_names().size() == 2);
    CHECK(t.project().name == "Rough cut v1");
    const timeline v2 = load("17-event-two-projects.fcpxml", "Rough cut v2");
    CHECK(v2.project().name == "Rough cut v2");
    CHECK(v2.items().size() == 2);
    // Its new version holds that project only, and no event clips or collections.
    timeline v;
    REQUIRE(v.load(v2.write_version("Rough cut v2 (Claude 1)"), nullptr));
    CHECK(v.project_names().size() == 1);
    CHECK(v.doc().write().find("keyword-collection") == std::string::npos);
    CHECK(v.doc().write().find("smart-collection") == std::string::npos);
  }
  SECTION("FCPXML 1.8: the asset's own src, a container clip") {
    const timeline t = load("18-legacy-1-8.fcpxml");
    CHECK(t.version() == "1.8");
    CHECK(t.items()[0].kind == item_kind::clip);
    CHECK(t.items()[1].source == "/Users/editor/Footage/Old export 2.mov");
    const auto paths = t.media_paths();
    CHECK(std::find(paths.begin(), paths.end(), "/Users/editor/Footage/Old export.mov") != paths.end());
  }
  SECTION("names and paths that need escaping") {
    const timeline t = load("19-unicode-entities.fcpxml");
    CHECK(t.project().name == "Unicode & entities — \"quoted\"");
    CHECK(t.project().event_name == "Tag 1 – Ankunft");
    CHECK(t.items()[0].name == "Café \"Zum Hirschen\" & <Bar>");
    CHECK(t.items()[0].source == "/Users/editor/Footage/Ü & ö/Café #1 (100%).mov");
    CHECK(t.items()[1].markers[0].value == "Gipfel \xF0\x9F\x8F\x94 & Tal");
  }
}

TEST_CASE("what the model does not know is carried and not editable", "[timeline][model]") {
  const std::string s =
      "<fcpxml version=\"1.20\"><resources><format id=\"r1\" frameDuration=\"1/25s\"/>"
      "<asset id=\"r2\" duration=\"10s\"><media-rep src=\"file:///a.mov\"/></asset>"
      "<asset id=\"r3\" name=\"unused\" duration=\"10s\"><media-rep src=\"file:///unused.mov\"/></asset></resources>"
      "<event name=\"E\"><project name=\"P\" uid=\"U\"><sequence format=\"r1\" duration=\"9s\"><spine>"
      "<asset-clip ref=\"r2\" offset=\"0s\" duration=\"4s\" future-attr=\"x\"><future-effect k=\"1\"/></asset-clip>"
      "<hologram offset=\"4s\" duration=\"2s\"/>"
      "<asset-clip ref=\"r2\" offset=\"6s\" start=\"bad\" duration=\"3s\"/>"
      "</spine></sequence></project></event></fcpxml>";
  timeline t;
  REQUIRE(t.load(s, nullptr));
  REQUIRE(t.items().size() == 3);
  CHECK(t.items()[0].editable);
  CHECK(t.items()[1].kind == item_kind::other);
  CHECK_FALSE(t.items()[1].editable);
  CHECK(t.items()[1].not_editable.find("hologram") != std::string::npos);
  CHECK_FALSE(t.items()[2].editable);  // an unreadable time
  const std::string v = t.write_version("P (Claude 1)");
  CHECK(v.find("<hologram offset=\"4s\" duration=\"2s\"/>") != std::string::npos);
  CHECK(v.find("future-attr=\"x\"><future-effect k=\"1\"/>") != std::string::npos);
  // The resource nothing uses is not carried into the version.
  CHECK(v.find("unused") == std::string::npos);
  CHECK(v.find("uid=") == std::string::npos);
}

TEST_CASE("file URLs and version names", "[timeline]") {
  CHECK(path_of_file_url("file:///Users/a%20b/x.mov") == "/Users/a b/x.mov");
  CHECK(path_of_file_url("FILE:///C:/Footage/x.mov") == "C:/Footage/x.mov");
  CHECK(path_of_file_url("file://localhost/Volumes/X/y.mp4") == "/Volumes/X/y.mp4");
  CHECK(path_of_file_url("file://nas/share/y.mp4") == "//nas/share/y.mp4");
  CHECK(path_of_file_url("https://example.com/x") == "");
  CHECK(path_of_file_url("file:///%E2%9C%93") == "/\xE2\x9C\x93");
  CHECK(next_version_name("Interview", {}) == "Interview (Claude 1)");
  CHECK(next_version_name("Interview", {"Interview (Claude 1)"}) == "Interview (Claude 2)");
  CHECK(next_version_name("Interview (Claude 2)", {"Interview (Claude 1)", "Interview (Claude 2)"}) ==
        "Interview (Claude 3)");
  CHECK(next_version_name("Take (Claude x)", {}) == "Take (Claude x) (Claude 1)");
}

TEST_CASE("what the panel shares with the timeline agent", "[timeline][bridge]") {
  bridge_state s;
  s.project = "Interview \"A\"";
  s.event = "18-09-2026";
  s.library = "file:///Users/editor/Movies/Dolomites%20trip.fcpbundle/";
  s.fcpxml_version = "1.13";
  s.dragged_ms = 1760000000000;
  s.changed_ms = 0;
  s.active_project = "Interview \"A\"";
  s.playhead_s = 12.48;
  s.playhead_ms_at = 1760000005000;
  s.folders = {"/Users/editor/Footage/broll", "/Volumes/Card/Selects"};
  s.versions = {{"Interview \"A\" (Claude 1)", "Interview _A_ (Claude 1).fcpxml", 1760000010000, "Interview \"A\""}};
  const auto back = bridge_state_from_json(to_json(s));
  REQUIRE(back.has_value());
  CHECK(back->project == s.project);
  CHECK(back->library == s.library);
  CHECK(back->dragged_ms == s.dragged_ms);
  CHECK(back->playhead_s == 12.48);
  CHECK(back->folders == s.folders);
  REQUIRE(back->versions.size() == 1);
  CHECK(back->versions[0].file == "Interview _A_ (Claude 1).fcpxml");
  CHECK_FALSE(bridge_state_from_json("{}").has_value());
  CHECK_FALSE(bridge_state_from_json("not json").has_value());
  CHECK(version_file_name("Interview \"A\" (Claude 1)") == "Interview _A_ (Claude 1).fcpxml");
  CHECK(version_file_name("../../etc/passwd") == "_.._etc_passwd.fcpxml");
  CHECK(version_file_name("") == "version.fcpxml");
  CHECK(dragged_ago(0, 30000) == "dragged just now");
  CHECK(dragged_ago(0, 120000) == "dragged 2 min ago");
  CHECK(dragged_ago(0, 3 * 3600000) == "dragged 3 h ago");
}
