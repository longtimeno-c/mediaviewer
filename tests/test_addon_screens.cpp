// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens (docs/design/25 §10, PR 58): the view-tree vocabulary both hosts render,
// its checks, diffs and events (src/addon/screen.h).
#include "catch_compat.h"

#include <chrono>
#include <string>

#include "addon/screen.h"
#include "core/json.h"

using mv::addon::screen::error;
using mv::addon::screen::tree;

namespace {

// Wall-clock bounds hold in an optimised build only. A Debug or sanitizer build
// (MSVC ASan Debug runs these ~200x slower) still runs every functional check.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MV_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MV_TEST_SANITIZED 1
#endif
#if defined(NDEBUG) && !defined(MV_TEST_SANITIZED)
constexpr bool kTimed = true;
#else
constexpr bool kTimed = false;
#endif

// One node of every type (§10), each with what it needs.
const char* kEveryType = R"({"type":"split","id":"root","axis":"horizontal","fraction":0.3,"children":[
 {"type":"scroll","id":"side","height":400,"children":[
  {"type":"stack","id":"col","axis":"vertical","spacing":8,"align":"fill","children":[
   {"type":"text","id":"t","text":"Hello","style":"heading","copyable":true,"lines":2},
   {"type":"image","id":"img","src":"icons/logo.png","label":"Logo","width":32,"height":32},
   {"type":"thumbnail","id":"th","path":"/photos/a.jpg","label":"A","size":96},
   {"type":"list","id":"l","label":"Test list","items":[{"id":"pt","text":"Portugal","detail":"3"},{"id":"es","text":"Spain"}]},
   {"type":"table","id":"tb","label":"Files","columns":[{"id":"n","title":"Name"}],"rows":[{"id":"r1","cells":["a.jpg"]}]},
   {"type":"button","id":"b","label":"Go","style":"primary","symbol":"star"},
   {"type":"toggle","id":"tg","label":"On","value":false},
   {"type":"choice","id":"ch","label":"Zoom","options":[{"id":"a","text":"A"},{"id":"b","text":"B"}],"value":"a"},
   {"type":"slider","id":"sl","label":"Radius","min":1,"max":50,"step":1,"value":15},
   {"type":"number","id":"nu","label":"Count","min":0,"max":9,"value":3},
   {"type":"text_field","id":"tf","label":"Filter","placeholder":"Type","value":""},
   {"type":"progress","id":"pr","label":"Reading","value":-1},
   {"type":"separator","id":"sep"},
   {"type":"grid","id":"g","columns":2,"children":[{"type":"text","id":"g1","text":"x"}]},
   {"type":"surface","id":"sf","surface":"pixels","label":"Test surface","height":120,"pin":{"x":0.5,"y":0.25},"pin_label":"Sintra"}
  ]}]},
 {"type":"thumbnail_grid","id":"grid","label":"Photos","tile":128,"multi":true,"items":[{"path":"/photos/a.jpg"},{"path":"/photos/b.jpg","label":"B"}]}
]})";

}  // namespace

TEST_CASE("every node type of the vocabulary parses and round-trips", "[addon][screens]") {
  tree t;
  REQUIRE(t.set(kEveryType) == error::none);
  REQUIRE(t.revision() == 1);
  // The canonical form reads back as the same tree, byte for byte.
  const std::string once = t.json();
  tree again;
  REQUIRE(again.set(once) == error::none);
  REQUIRE(again.json() == once);
  REQUIRE(t.find("grid") != nullptr);
  REQUIRE(t.surface_ids() == std::vector<std::string>{"pixels"});
  // Every type is named.
  for (const char* type : {"stack", "grid", "scroll", "split", "text", "image", "thumbnail", "thumbnail_grid",
                           "list", "table", "button", "toggle", "choice", "slider", "number", "text_field",
                           "progress", "separator", "surface"}) {
    INFO(type);
    REQUIRE(once.find(std::string("\"type\":\"") + type + "\"") != std::string::npos);
  }
}

TEST_CASE("the #442 additions: tiles, a ring, a code field, a card, display text", "[addon][screens]") {
  using namespace mv::addon::screen;
  const char* json = R"({"type":"stack","id":"page","look":"card","children":[
   {"type":"text","id":"code","text":"123 456","style":"display"},
   {"type":"tiles","id":"peers","label":"Nearby","size":72,"selection":["a"],"items":[
     {"id":"a","text":"Studio","detail":"Wi-Fi","initials":"S","progress":0.4},
     {"id":"b","text":"Laptop","detail":"Bluetooth","mark":"done"},
     {"id":"c","text":"Desk","progress":-1,"mark":"failed"}]},
   {"type":"progress","id":"ring","label":"Sending","value":0.25,"look":"ring","size":96},
   {"type":"text_field","id":"entry","label":"Code","look":"code","digits":6,"value":""}]})";
  tree t;
  REQUIRE(t.set(json) == error::none);
  tree again;
  REQUIRE(again.set(t.json()) == error::none);
  CHECK(again.json() == t.json());
  // A click on a tile is `activate` with its id; select takes one id; a
  // tile that is not there, or a non-string, is refused.
  CHECK(t.check_event("peers", ev_activate, R"("b")") == error::none);
  CHECK(t.check_event("peers", ev_activate, R"("zz")") == error::bad_value);
  CHECK(t.check_event("peers", ev_activate, "3") == error::bad_value);
  CHECK(t.check_event("peers", ev_select, R"(["c"])") == error::none);
  CHECK(t.check_event("peers", ev_select, R"(["a","b"])") == error::bad_value);
  CHECK(t.check_event("peers", ev_click, "") == error::bad_value);
  CHECK(t.check_event("entry", ev_submit, R"("123456")") == error::none);
  CHECK(t.check_event("ring", ev_click, "") == error::not_interactive);
  tree shown;
  REQUIRE(shown.set(R"({"type":"tiles","id":"from","label":"From","static":true,"items":[{"id":"a","text":"Ann","initials":"A"}]})") ==
          error::none);
  CHECK(shown.check_event("from", ev_activate, R"("a")") == error::not_interactive);
  // Each new key is checked.
  const char* bad[] = {
      R"({"type":"tiles","id":"t","label":"T"})",  // no items
      R"({"type":"tiles","id":"t","items":[]})",   // no label
      R"({"type":"tiles","id":"t","label":"T","items":[{"id":"a"}]})",
      R"({"type":"tiles","id":"t","label":"T","items":[{"id":"a","text":"A","progress":2}]})",
      R"({"type":"tiles","id":"t","label":"T","items":[{"id":"a","text":"A","mark":"maybe"}]})",
      R"({"type":"tiles","id":"t","label":"T","items":[{"id":"a","text":"A","colour":"red"}]})",
      R"({"type":"tiles","id":"t","label":"T","items":[{"id":"a","text":"A"},{"id":"a","text":"B"}]})",
      R"({"type":"tiles","id":"t","label":"T","size":500,"items":[]})",
      R"({"type":"progress","id":"p","label":"P","value":0.5,"look":"pie"})",
      R"({"type":"progress","id":"p","label":"P","value":0.5,"size":4})",
      R"({"type":"text_field","id":"f","label":"F","look":"pin"})",
      R"({"type":"text_field","id":"f","label":"F","digits":40})",
      R"({"type":"stack","id":"s","look":"glass"})",
      R"({"type":"text","id":"x","text":"x","style":"huge"})",
  };
  for (const char* b : bad) {
    INFO(b);
    tree x;
    CHECK(x.set(b) != error::none);
  }
}

TEST_CASE("the 2026-10-19 additions: a transfer's path, a button that copies", "[addon][screens]") {
  using namespace mv::addon::screen;
  const char* json = R"({"type":"stack","id":"page","children":[
   {"type":"link","id":"p1","label":"Sending to Bea. Wi-Fi.","via":"wifi","state":"moving","direction":"send",
    "from":"This computer","to":"Bea","caption":"Wi-Fi, on this network","value":0.4},
   {"type":"link","id":"p2","label":"Receiving","via":"relay","state":"connecting","through":"Rob"},
   {"type":"link","id":"p3","label":"Done","via":"dht","state":"done"},
   {"type":"button","id":"copy","label":"Copy code","copy":"ABCD-EFGH"}]})";
  tree t;
  REQUIRE(t.set(json) == error::none);
  tree again;
  REQUIRE(again.set(t.json()) == error::none);
  CHECK(again.json() == t.json());
  // A picture, not a control; the button still reports its click.
  CHECK(t.check_event("p1", ev_click, "") == error::not_interactive);
  CHECK(t.check_event("copy", ev_click, "") == error::none);
  for (const char* via : {"bluetooth", "wifi", "internet", "dht", "relay", "server"}) {
    tree x;
    CHECK(x.set(std::string(R"x({"type":"link","id":"l","label":"L","state":"paused","via":")x") + via + "\"}") ==
          error::none);
  }
  const char* bad[] = {
      R"({"type":"link","id":"l","via":"wifi","state":"moving"})",                   // no label
      R"({"type":"link","id":"l","label":"L","state":"moving"})",                    // no via
      R"({"type":"link","id":"l","label":"L","via":"wifi"})",                        // no state
      R"({"type":"link","id":"l","label":"L","via":"carrier pigeon","state":"moving"})",
      R"({"type":"link","id":"l","label":"L","via":"wifi","state":"exploding"})",
      R"({"type":"link","id":"l","label":"L","via":"wifi","state":"moving","direction":"up"})",
      R"({"type":"link","id":"l","label":"L","via":"wifi","state":"moving","value":3})",
      R"({"type":"link","id":"l","label":"L","via":"wifi","state":"moving","colour":"red"})",
      R"({"type":"button","id":"b","label":"B","copy":""})",
      R"({"type":"button","id":"b","label":"B","copy":"a\u0007b"})",
      R"({"type":"button","id":"b","label":"B","copy":7})",
  };
  for (const char* b : bad) {
    INFO(b);
    tree x;
    CHECK(x.set(b) != error::none);
  }
}

TEST_CASE("a control or picture without a label is refused (VoiceOver / Narrator)", "[addon][screens]") {
  const char* unnamed[] = {
      R"({"type":"button","id":"b"})",
      R"({"type":"toggle","id":"t","value":true})",
      R"({"type":"choice","id":"c","options":[{"id":"a","text":"A"}]})",
      R"({"type":"slider","id":"s","min":0,"max":1})",
      R"({"type":"number","id":"n"})",
      R"({"type":"text_field","id":"f"})",
      R"({"type":"progress","id":"p","value":0.5})",
      R"({"type":"image","id":"i","src":"a.png"})",
      R"({"type":"thumbnail","id":"t","path":"/a.jpg"})",
      R"({"type":"thumbnail_grid","id":"g","items":[]})",
      R"({"type":"list","id":"l","items":[]})",
      R"({"type":"table","id":"t","columns":[{"id":"a","title":"A"}],"rows":[]})",
      R"({"type":"surface","id":"s","surface":"m","height":40})",
      R"({"type":"button","id":"b","label":"   "})",
  };
  for (const char* json : unnamed) {
    INFO(json);
    tree t;
    REQUIRE(t.set(json) == error::missing_label);
    REQUIRE(t.empty());
  }
  // Text and layout need none.
  tree t;
  REQUIRE(t.set(R"({"type":"stack","id":"s","children":[{"type":"text","id":"x","text":"hi"},{"type":"separator","id":"r"}]})") ==
          error::none);
}

TEST_CASE("a tree that is not the vocabulary is refused whole", "[addon][screens]") {
  struct bad {
    const char* json;
    error why;
  };
  const bad cases[] = {
      {"not json", error::not_json},
      {R"([])", error::bad_node},
      {R"({"type":"webview","id":"w"})", error::bad_node},
      {R"({"type":"text","text":"no id"})", error::bad_node},
      {R"({"type":"text","id":"has space","text":"x"})", error::bad_node},
      {R"({"type":"text","id":"t"})", error::bad_prop},  // no text
      {R"({"type":"text","id":"t","text":"x","colour":"red"})", error::bad_prop},
      {R"({"type":"text","id":"t","text":"x","style":"blink"})", error::bad_prop},
      {R"({"type":"stack","id":"s","spacing":99})", error::bad_prop},
      {R"({"type":"slider","id":"s","label":"S","min":5,"max":1})", error::bad_prop},
      {R"({"type":"progress","id":"p","label":"P","value":2})", error::bad_prop},
      {R"({"type":"image","id":"i","label":"I","src":"../../etc/passwd.png"})", error::bad_prop},
      {R"({"type":"image","id":"i","label":"I","src":"/abs.png"})", error::bad_prop},
      {R"({"type":"image","id":"i","label":"I","src":"logo.svg"})", error::bad_prop},
      {R"({"type":"button","id":"b","label":"B","symbol":"rocket"})", error::bad_prop},
      {R"({"type":"surface","id":"s","label":"S","surface":"m","height":40,"pin":{"x":2,"y":0}})", error::bad_prop},
      {R"({"type":"list","id":"l","label":"L","items":[{"id":"a","text":"A"},{"id":"a","text":"B"}]})", error::bad_prop},
      {R"({"type":"text","id":"t","text":"bad\u0007bell"})", error::bad_prop},
      {R"({"type":"stack","id":"s","children":[{"type":"text","id":"s","text":"x"}]})", error::duplicate_id},
      {R"({"type":"text","id":"t","text":"x","children":[{"type":"separator","id":"r"}]})", error::bad_children},
      {R"({"type":"split","id":"s","children":[{"type":"separator","id":"r"}]})", error::bad_children},
  };
  for (const bad& b : cases) {
    INFO(b.json);
    tree t;
    REQUIRE(t.set(b.json) == b.why);
    REQUIRE(t.empty());
  }
  // A good tree stays when a bad one is posted after it.
  tree t;
  REQUIRE(t.set(R"({"type":"text","id":"t","text":"kept"})") == error::none);
  REQUIRE(t.set(R"({"type":"text","id":"t"})") == error::bad_prop);
  REQUIRE(t.json().find("kept") != std::string::npos);
  REQUIRE(t.revision() == 1);
}

TEST_CASE("a tree past its bounds is refused", "[addon][screens]") {
  // Too deep.
  std::string deep;
  for (int i = 0; i < 30; ++i) deep += R"({"type":"stack","id":"s)" + std::to_string(i) + R"(","children":[)";
  deep += R"({"type":"separator","id":"end"})";
  for (int i = 0; i < 30; ++i) deep += "]}";
  tree t;
  const error e = t.set(deep);
  REQUIRE((e == error::too_large || e == error::not_json));
  // Too many nodes.
  std::string many = R"({"type":"stack","id":"root","children":[)";
  for (std::size_t i = 0; i < mv::addon::screen::kMaxNodes + 1; ++i) {
    many += (i ? "," : "") + std::string(R"({"type":"separator","id":"n)") + std::to_string(i) + "\"}";
  }
  many += "]}";
  REQUIRE(t.set(many) == error::too_large);
}

TEST_CASE("diffs apply atomically", "[addon][screens]") {
  tree t;
  REQUIRE(t.set(kEveryType) == error::none);
  const auto r0 = t.revision();
  REQUIRE(t.apply(R"([{"op":"set","id":"t","props":{"text":"Changed","lines":null}}])") == error::none);
  REQUIRE(t.revision() == r0 + 1);
  REQUIRE(t.json().find("Changed") != std::string::npos);
  REQUIRE(t.json().find("\"lines\"") == std::string::npos);
  REQUIRE(t.apply(R"([{"op":"insert","parent":"col","index":0,"node":{"type":"text","id":"first","text":"1st"}}])") ==
          error::none);
  REQUIRE(t.find("col")->children.front().id == "first");
  REQUIRE(t.apply(R"([{"op":"insert","parent":"col","index":-1,"node":{"type":"text","id":"last","text":"z"}}])") ==
          error::none);
  REQUIRE(t.find("col")->children.back().id == "last");
  REQUIRE(t.apply(R"([{"op":"replace","id":"b","node":{"type":"button","id":"b2","label":"Stop"}}])") ==
          error::none);
  REQUIRE(t.find("b") == nullptr);
  REQUIRE(t.find("b2") != nullptr);
  REQUIRE(t.apply(R"([{"op":"remove","id":"sep"}])") == error::none);
  REQUIRE(t.find("sep") == nullptr);

  // Every entry or none: the second entry fails, the first is not kept.
  const std::string before = t.json();
  const auto r1 = t.revision();
  REQUIRE(t.apply(R"([{"op":"set","id":"t","props":{"text":"lost"}},{"op":"remove","id":"nope"}])") ==
          error::no_such_node);
  REQUIRE(t.apply(R"([{"op":"set","id":"b2","props":{"label":""}}])") == error::missing_label);
  REQUIRE(t.apply(R"([{"op":"insert","parent":"t","index":0,"node":{"type":"separator","id":"x"}}])") ==
          error::bad_children);
  REQUIRE(t.apply(R"([{"op":"insert","parent":"col","index":0,"node":{"type":"text","id":"t","text":"dup"}}])") ==
          error::duplicate_id);
  REQUIRE(t.apply(R"([{"op":"remove","id":"root"}])") == error::bad_op);
  REQUIRE(t.apply(R"([{"op":"move","id":"t"}])") == error::bad_op);
  REQUIRE(t.apply(R"([{"op":"set","id":"t","props":{"type":"button"}}])") == error::bad_prop);
  REQUIRE(t.apply(R"({"op":"set"})") == error::bad_op);
  REQUIRE(t.json() == before);
  REQUIRE(t.revision() == r1);
  // The root can be replaced whole.
  REQUIRE(t.apply(R"([{"op":"replace","id":"root","node":{"type":"text","id":"only","text":"x"}}])") ==
          error::none);
  REQUIRE(t.root()->id == "only");
}

TEST_CASE("events are checked against the tree before an add-on hears them", "[addon][screens]") {
  using namespace mv::addon::screen;
  tree t;
  REQUIRE(t.set(kEveryType) == error::none);
  REQUIRE(t.check_event("b", ev_click, "") == error::none);
  REQUIRE(t.check_event("b", ev_click, "1") == error::bad_value);
  REQUIRE(t.check_event("tg", ev_change, "true") == error::none);
  REQUIRE(t.find("tg")->props.boolean("value") == true);  // the tree keeps in step
  REQUIRE(t.check_event("tg", ev_change, "\"yes\"") == error::bad_value);
  REQUIRE(t.check_event("ch", ev_change, "\"b\"") == error::none);
  REQUIRE(t.check_event("ch", ev_change, "\"z\"") == error::bad_value);
  REQUIRE(t.check_event("sl", ev_change, "50") == error::none);
  REQUIRE(t.check_event("sl", ev_change, "51") == error::bad_value);
  REQUIRE(t.check_event("sl", ev_change, "0.5") == error::bad_value);
  REQUIRE(t.check_event("nu", ev_change, "9") == error::none);
  REQUIRE(t.check_event("tf", ev_change, "\"Sintra\"") == error::none);
  REQUIRE(t.check_event("tf", ev_submit, "\"Sintra\"") == error::none);
  REQUIRE(t.check_event("l", ev_select, R"(["pt"])") == error::none);
  REQUIRE(t.check_event("l", ev_select, R"(["pt","es"])") == error::bad_value);  // not multi
  REQUIRE(t.check_event("l", ev_select, R"(["xx"])") == error::bad_value);
  REQUIRE(t.check_event("l", ev_activate, R"("es")") == error::none);
  REQUIRE(t.check_event("tb", ev_select, R"(["r1"])") == error::none);
  REQUIRE(t.check_event("grid", ev_select, R"(["/photos/a.jpg","/photos/b.jpg"])") == error::none);
  REQUIRE(t.check_event("grid", ev_activate, R"("/photos/zzz.jpg")") == error::bad_value);
  REQUIRE(t.check_event("t", ev_click, "") == error::not_interactive);
  REQUIRE(t.check_event("sf", ev_click, "") == error::not_interactive);
  REQUIRE(t.check_event("missing", ev_click, "") == error::no_such_node);
  REQUIRE(t.apply(R"([{"op":"set","id":"b","props":{"disabled":true}}])") == error::none);
  REQUIRE(t.check_event("b", ev_click, "") == error::not_interactive);
}

TEST_CASE("an interactive surface takes zoom, pan and double-click", "[addon][screens]") {
  using namespace mv::addon::screen;
  tree t;
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"Test surface","height":120})") == error::none);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":1.2,"x":0.5,"y":0.5})") == error::not_interactive);
  REQUIRE(t.apply(R"([{"op":"set","id":"s","props":{"interactive":true}}])") == error::none);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":1.2,"x":0.5,"y":0.25})") == error::none);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":0.8,"x":0,"y":1})") == error::none);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":0,"x":0.5,"y":0.5})") == error::bad_value);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":1.2,"x":1.5,"y":0.5})") == error::bad_value);
  REQUIRE(t.check_event("s", ev_zoom, R"({"scale":1.2,"x":0.5})") == error::bad_value);
  REQUIRE(t.check_event("s", ev_pan, R"({"dx":-0.1,"dy":0.05})") == error::none);
  REQUIRE(t.check_event("s", ev_pan, R"({"dx":"left","dy":0})") == error::bad_value);
  REQUIRE(t.check_event("s", ev_double, R"({"x":0.3,"y":0.7})") == error::none);
  REQUIRE(t.check_event("s", ev_click, "") == error::bad_value);
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"L","height":40,"interactive":"yes"})") ==
          error::bad_prop);
  // Bounds for the live zoom, and names the chrome draws over the pixels.
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"L","height":40,
                    "interactive":{"min_scale":0.25,"max_scale":8},
                    "marks":[{"x":0.4,"y":0.5,"text":"One"},{"x":0.9,"y":0.1,"text":"Two"}]})") == error::none);
  REQUIRE(t.check_event("s", ev_pan, R"({"dx":0.1,"dy":0})") == error::none);
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"L","height":40,
                    "interactive":{"max_scale":0.5}})") == error::bad_prop);
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"L","height":40,
                    "marks":[{"x":1.5,"y":0.5,"text":"Off"}]})") == error::bad_prop);
  REQUIRE(t.set(R"({"type":"surface","id":"s","surface":"px","label":"L","height":40,"interactive":false})") ==
          error::none);
  REQUIRE(t.check_event("s", ev_pan, R"({"dx":0.1,"dy":0})") == error::not_interactive);
}

TEST_CASE("a 2,000-tile thumbnail grid is one cheap node", "[addon][screens]") {
  std::string json = R"({"type":"thumbnail_grid","id":"g","label":"Photos","tile":128,"items":[)";
  for (int i = 0; i < 2000; ++i) {
    json += (i ? "," : "") + std::string(R"({"path":"/fixtures/tiles/IMG_)") + std::to_string(i) + R"(.JPG"})";
  }
  json += "]}";
  tree t;
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(t.set(json) == error::none);
  const std::string out = t.json();
  REQUIRE(t.apply(R"([{"op":"set","id":"g","props":{"selection":["/fixtures/tiles/IMG_7.JPG"]}}])") ==
          error::none);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(t.find("g")->props.find("items")->a.size() == 2000);
  // Parse, check, write and one diff: the host does this once per post, on the
  // add-on's thread, never per frame. Generous bound for a loaded CI runner.
  if constexpr (kTimed) CHECK(ms < 500);
}

TEST_CASE("selecting every tile of a large grid is checked in one pass", "[addon][screens]") {
  // Select All in a 20,000-tile multi-select grid: the event is checked on the
  // chrome's thread under the registry lock, so it must not scan the items once
  // per selected tile (20,000 x 20,000 compares).
  using namespace mv::addon::screen;
  constexpr int kTiles = 20'000;
  std::string json = R"({"type":"thumbnail_grid","id":"g","label":"Photos","multi":true,"items":[)";
  std::string all = "[";
  for (int i = 0; i < kTiles; ++i) {
    const std::string path = "/fixtures/tiles/IMG_" + std::to_string(i) + ".JPG";
    json += (i ? "," : "") + std::string(R"({"path":")") + path + "\"}";
    all += (i ? ",\"" : "\"") + path + "\"";
  }
  json += "]}";
  all += "]";
  tree t;
  REQUIRE(t.set(json) == error::none);
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(t.check_event("g", ev_select, all) == error::none);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  // One pass is ~10 ms in Release; the quadratic scan this guards against
  // took seconds there.
  if constexpr (kTimed) CHECK(ms < 200);
  REQUIRE(t.check_event("g", ev_select, R"(["/fixtures/tiles/nope.JPG"])") == error::bad_value);
}

TEST_CASE("contribution API 9: a surface's glow and a glowing progress bar", "[addon][screens]") {
  using namespace mv::addon::screen;
  const char* json = R"({"type":"stack","id":"sheet","children":[
   {"type":"surface","id":"preview","surface":"preview","label":"Preview","height":220,"glow":"moving"},
   {"type":"progress","id":"progress","label":"Enhancing","value":0.4,"look":"glow"}]})";
  tree t;
  REQUIRE(t.set(json) == error::none);
  tree again;
  REQUIRE(again.set(t.json()) == error::none);
  CHECK(again.json() == t.json());
  // The add-on holds it still when the run ends: a diff, not a new tree.
  CHECK(t.apply(R"([{"op":"set","id":"preview","props":{"glow":"still"}}])") == error::none);
  CHECK(t.json().find("\"glow\":\"still\"") != std::string::npos);
  // Glow is display only: it sends nothing.
  CHECK(t.check_event("progress", ev_click, "") == error::not_interactive);
  for (const char* bad : {
           R"({"type":"surface","id":"s","surface":"s","label":"S","height":100,"glow":"spinning"})",
           R"({"type":"surface","id":"s","surface":"s","label":"S","height":100,"glow":true})",
           R"({"type":"progress","id":"p","label":"P","value":0.5,"glow":"moving"})",
           R"({"type":"text","id":"x","text":"x","glow":"moving"})"}) {
    INFO(bad);
    tree b;
    CHECK(b.set(bad) == error::bad_prop);
  }
}
