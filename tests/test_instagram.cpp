// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Instagram Post (docs/design/30): the post's rules, the crop arithmetic, the
// scaler, the window's trees through the real vocabulary check, and the engine
// over a fake host.
#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/contrib.h"
#include "addon/manifest.h"
#include "addon/screen.h"
#include "addons/instagram/engine.h"
#include "addons/instagram/pixels.h"
#include "addons/instagram/post.h"
#include "addons/instagram/view.h"
#include "core/json.h"

using namespace mv::instagram;

namespace {

std::string pad(int i) { return std::to_string(1000 + i); }

image solid(std::uint32_t w, std::uint32_t h, std::uint8_t v) {
  image out;
  out.w = w;
  out.h = h;
  out.rgb.assign(static_cast<std::size_t>(w) * h * 3, v);
  return out;
}

struct fake_host {
  std::mutex m;
  std::vector<std::string> trees, diffs;
  std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> surfaces;
  std::vector<gallery_row> folder, library;
  bool library_ok = false;
  std::vector<std::string> hits;
  std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>> stills_written;
  std::vector<std::string> shown;
  std::vector<std::string> busy;
};

engine_port port_of(fake_host& h, bool with_library = true) {
  engine_port p;
  p.post = [&h](std::uint64_t, const std::string& t) {
    std::lock_guard lock(h.m);
    h.trees.push_back(t);
    return true;
  };
  p.diff = [&h](std::uint64_t, const std::string& t) {
    std::lock_guard lock(h.m);
    h.diffs.push_back(t);
    return true;
  };
  p.surface = [&h](std::uint64_t, const std::string& id, const std::vector<std::uint8_t>&, std::uint32_t w, std::uint32_t ht) {
    std::lock_guard lock(h.m);
    h.surfaces[id] = {w, ht};
  };
  p.walk = [&h](const std::string&, const std::function<void(gallery_row)>& add) {
    for (const gallery_row& r : h.folder) add(r);
    return true;
  };
  if (with_library) {
    p.library = [&h](const std::function<void(gallery_row)>& add) {
      for (const gallery_row& r : h.library) add(r);
      return h.library_ok;
    };
  }
  p.preview = [](const std::string&, std::uint32_t, image& out) {
    out = solid(800, 600, 128);
    return true;
  };
  p.search = [&h](const std::string&, const std::string&, std::vector<std::string>& out) {
    out = h.hits;
    return true;
  };
  p.still = [](const std::string&, image& out) {
    out = solid(4000, 3000, 200);
    return true;
  };
  p.write_still = [&h](const image& pic, const std::string& dir, const std::string& stem) {
    std::lock_guard lock(h.m);
    h.stills_written.emplace_back(stem, pic.w, pic.h, dir);
    return dir + "/" + stem + ".jpg";
  };
  p.make_dirs = [](const std::string&) { return true; };
  p.open_list = [&h](std::uint64_t, const std::vector<std::string>& paths) { h.shown = paths; };
  p.busy = [&h](const char* what) {
    std::lock_guard lock(h.m);
    h.busy.emplace_back(what);
  };
  p.library_dir = "/lib";
  p.stamp = [] { return std::string("stamp"); };
  return p;
}

gallery_row row(std::string name, std::int64_t when) {
  gallery_row r;
  r.path = "/f/" + name;
  r.name = std::move(name);
  r.when = when;
  return r;
}

std::string selection_of(std::initializer_list<std::string> paths) {
  mv::json::writer w;
  w.begin_array();
  for (const std::string& p : paths) w.string(p);
  w.end_array();
  return w.take();
}

}  // namespace

TEST_CASE("instagram: a post holds 20 and keeps its order", "[instagram]") {
  post p;
  for (int i = 0; i < 25; ++i) (void)p.add("/a" + pad(i), "n", false);
  REQUIRE(p.items().size() == kMaxChosen);
  REQUIRE(p.items().front().path == "/a" + pad(0));
  REQUIRE(p.items().back().path == "/a" + pad(19));
  REQUIRE_FALSE(p.add("/a" + pad(3), "again", false));
  const std::uint64_t second = p.items()[1].uid;
  REQUIRE(p.move(second, -1));
  REQUIRE(p.items()[0].uid == second);
  REQUIRE_FALSE(p.move(second, -1));
}

TEST_CASE("instagram: a selection appends what is new and removes what went", "[instagram]") {
  post p;
  std::vector<post::described> want{{"/b", "b", false}, {"/a", "a", true}};
  auto c = p.apply_selection(want);
  REQUIRE(c.added == 2);
  REQUIRE(p.items()[0].path == "/b");
  REQUIRE(p.items()[1].video);
  // The host sends a sorted array: "/0" sorts first but was chosen last.
  std::vector<post::described> more{{"/0", "z", false}, {"/a", "a", true}, {"/b", "b", false}};
  c = p.apply_selection(more);
  REQUIRE(c.added == 1);
  REQUIRE(p.items().back().path == "/0");
  REQUIRE(p.items().front().path == "/b");
  std::vector<post::described> fewer{{"/0", "z", false}, {"/b", "b", false}};
  c = p.apply_selection(fewer);
  REQUIRE(c.removed == 1);
  REQUIRE(p.items().size() == 2);
}

TEST_CASE("instagram: a twenty-first choice is refused and said", "[instagram]") {
  post p;
  std::vector<post::described> want;
  for (int i = 0; i < 21; ++i) want.push_back({"/a" + pad(i), "n", false});
  const auto c = p.apply_selection(want);
  REQUIRE(c.added == 20);
  REQUIRE(c.refused == 1);
}

TEST_CASE("instagram: removing the focused tile moves focus to its neighbour", "[instagram]") {
  post p;
  (void)p.add("/1", "1", false);
  (void)p.add("/2", "2", false);
  (void)p.add("/3", "3", false);
  const std::uint64_t middle = p.items()[1].uid;
  const std::uint64_t last = p.items()[2].uid;
  p.set_focus(middle);
  REQUIRE(p.remove(middle));
  REQUIRE(p.focus() == last);
  REQUIRE(p.remove(last));
  REQUIRE(p.focus() == p.items().back().uid);
}

TEST_CASE("instagram: each shape exports at Instagram's size", "[instagram]") {
  REQUIRE(info_of(shape::square).out_h == 1080);
  REQUIRE(info_of(shape::portrait).out_h == 1350);
  REQUIRE(info_of(shape::tall).out_h == 1440);
  REQUIRE(info_of(shape::widescreen).out_h == 608);
  REQUIRE(info_of(shape::wide).out_h == 566);
  for (const shape_info& s : shapes()) {
    REQUIRE(s.out_w == 1080);
    REQUIRE(s.out_h % 2 == 0);  // a clip's encoder needs even sides
  }
}

TEST_CASE("instagram: the crop is the largest of the shape and stays inside", "[instagram]") {
  const double tolerance = 1e-9;
  rect r = crop_of(4000, 3000, 1.0, frame{});
  REQUIRE(std::abs(r.h - 1.0) < tolerance);
  REQUIRE(std::abs(r.w - 0.75) < tolerance);
  REQUIRE(std::abs(r.x - 0.125) < tolerance);
  r = crop_of(3000, 4000, 1.91, frame{});
  REQUIRE(std::abs(r.w - 1.0) < tolerance);
  frame corner;
  corner.cx = 0;
  corner.cy = 0;
  r = crop_of(4000, 3000, 1.0, corner);
  REQUIRE(r.x == 0);
  REQUIRE(r.y == 0);
  frame zoomed_in;
  zoomed_in.zoom = 2;
  r = crop_of(4000, 3000, 1.0, zoomed_in);
  REQUIRE(std::abs(r.h - 0.5) < tolerance);
  REQUIRE(std::abs((r.x + r.w / 2) - 0.5) < tolerance);
}

TEST_CASE("instagram: a frame survives a change of shape", "[instagram]") {
  frame f;
  f.cx = 0.7;
  f.zoom = 1.5;
  const rect square = crop_of(4000, 3000, 1.0, f);
  const rect tall = crop_of(4000, 3000, 0.75, f);
  REQUIRE(std::abs((square.x + square.w / 2) - 0.7) < 1e-9);
  REQUIRE(std::abs((tall.x + tall.w / 2) - 0.7) < 1e-9);
}

TEST_CASE("instagram: dragging the picture right moves the crop left, and a drag past the edge is not banked", "[instagram]") {
  frame f;
  f.zoom = 2;
  const frame g = dragged(f, 1000, 1000, 1.0, 0.1, 0);
  REQUIRE(g.cx < f.cx);
  frame h = f;
  for (int i = 0; i < 40; ++i) h = dragged(h, 1000, 1000, 1.0, 0.5, 0);
  const frame back = dragged(h, 1000, 1000, 1.0, -0.5, 0);
  REQUIRE(back.cx > 0.25);  // moves at once
  REQUIRE(zoomed(frame{}, 100).zoom == kMaxZoom);
  REQUIRE(zoomed(frame{}, 0.01).zoom == 1.0);
}

TEST_CASE("instagram: the scaler keeps a flat picture flat and the size asked", "[instagram]") {
  const image pic = solid(1000, 750, 180);
  std::vector<std::uint8_t> out(108 * 135 * 3);
  scaler(pic.w, pic.h, crop_of(pic.w, pic.h, 0.8, frame{}), 108, 135).run(pic.rgb.data(), out.data());
  for (const std::uint8_t v : out) REQUIRE(std::abs(int(v) - 180) <= 1);
  // Upscaling does not read past the crop.
  std::vector<std::uint8_t> big(1080 * 1350 * 3);
  scaler(pic.w, pic.h, crop_of(pic.w, pic.h, 0.8, frame{}), 1080, 1350).run(pic.rgb.data(), big.data());
  REQUIRE(std::abs(int(big[0]) - 180) <= 1);
}

TEST_CASE("instagram: a quarter turn swaps the sides and carries the pixels", "[instagram]") {
  // 2 x 1: red, blue.
  const std::uint8_t src[6] = {255, 0, 0, 0, 0, 255};
  image out;
  turned(src, 2, 1, 90, out);
  REQUIRE(out.w == 1);
  REQUIRE(out.h == 2);
  REQUIRE(out.rgb[0] == 255);  // clockwise: the left end goes to the top
  REQUIRE(out.rgb[5] == 255);
}

TEST_CASE("instagram: a focused tile carries a ring inside its edge", "[instagram]") {
  const image pic = solid(100, 100, 50);
  const std::vector<std::uint8_t> t = tile_bgra(pic, rect{}, 20, 20, 0x11223344u, 2);
  REQUIRE(t[0] == 0x33);  // B
  REQUIRE(t[1] == 0x22);
  REQUIRE(t[2] == 0x11);
  const std::size_t mid = (10u * 20u + 10u) * 4u;
  REQUIRE(std::abs(int(t[mid]) - 50) <= 1);
}

TEST_CASE("instagram: the window's trees pass the host's checks", "[instagram]") {
  post p;
  (void)p.add("/a.jpg", "a.jpg", false);
  (void)p.add("/b.mov", "b.mov", true);
  p.set_focus(p.items()[0].uid);
  gallery_row a = row("a.jpg", 1);
  gallery_row b = row("b.mov", 2);
  b.video = true;
  std::vector<const gallery_row*> rows{&a, &b};
  view_state v;
  v.p = &p;
  v.rows = &rows;
  v.shown = rows.size();
  v.has_folder = true;
  v.has_library = true;
  mv::addon::screen::tree t;
  REQUIRE(t.set(tree_json(v)) == mv::addon::screen::error::none);
  REQUIRE(t.apply(strip_diff(v)) == mv::addon::screen::error::none);
  REQUIRE(t.apply(gallery_diff(v)) == mv::addon::screen::error::none);
  v.exporting = true;
  v.progress = 0.5;
  v.progress_words = "1 of 2";
  REQUIRE(t.apply(progress_diff(v)) == mv::addon::screen::error::none);
  for (const shape_info& s : shapes()) {
    p.set_shape(s.s);
    REQUIRE(t.apply(strip_diff(v)) == mv::addon::screen::error::none);
  }
}

TEST_CASE("instagram: a full gallery fits the host's bounds", "[instagram]") {
  post p;
  std::vector<gallery_row> storage;
  storage.reserve(kGalleryPage);
  for (std::size_t i = 0; i < kGalleryPage; ++i) {
    storage.push_back(row(std::string(180, 'x') + std::to_string(i) + ".heic", 0));
  }
  std::vector<const gallery_row*> rows;
  for (const gallery_row& r : storage) rows.push_back(&r);
  view_state v;
  v.p = &p;
  v.rows = &rows;
  v.shown = rows.size();
  mv::addon::screen::tree t;
  REQUIRE(t.set(tree_json(v)) == mv::addon::screen::error::none);
}

TEST_CASE("instagram: the manifest is one the host accepts", "[instagram]") {
  std::ifstream in(MV_INSTAGRAM_MANIFEST_JSON);
  REQUIRE(in.good());
  std::stringstream text;
  text << in.rdbuf();
  const auto doc = mv::json::parse(text.str());
  REQUIRE(doc.has_value());
  const auto* contributes = doc->find("contributes");
  REQUIRE(contributes != nullptr);
  const auto* api = contributes->find("api");
  REQUIRE(api != nullptr);
  REQUIRE(api->find("max")->i == MV_ADDON_CONTRIBUTION_API);
  REQUIRE(api->find("min")->i <= mv::addon::kSlotsContributionApi);  // the loader's own bound
}

TEST_CASE("instagram: names filter as you type, newest first, and search leads", "[instagram]") {
  fake_host h;
  h.folder = {row("IMG_1.jpg", 10), row("beach.mov", 30), row("notes.txt", 40), row("IMG_2.jpg", 20)};
  h.hits = {"/f/IMG_1.jpg"};
  engine e(port_of(h, false));
  e.opened({7, "/f/IMG_1.jpg", 0x3478F6FF, 2});
  e.wait_idle();
  {
    std::lock_guard lock(h.m);
    const std::string& g = h.diffs.back();
    REQUIRE(g.find("notes.txt") == std::string::npos);
    REQUIRE(g.find("beach.mov") < g.find("IMG_2.jpg"));  // newest first
    REQUIRE(g.find("\"badge\":\"video\"") != std::string::npos);
  }
  e.change("search", "\"img\"");
  e.wait_idle();
  std::lock_guard lock(h.m);
  const std::string& g = h.diffs.back();
  REQUIRE(g.find("beach.mov") == std::string::npos);
  REQUIRE(g.find("IMG_1.jpg") < g.find("IMG_2.jpg"));  // Local search's answer first
}

TEST_CASE("instagram: the Photos choice shows only when the library answers", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1)};
  {
    engine e(port_of(h));
    e.opened({1, "/f/a.jpg", 0, 2});
    e.wait_idle();
    std::lock_guard lock(h.m);
    REQUIRE(h.diffs.back().find("\"id\":\"source\",\"props\":{\"value\":\"folder\",\"hidden\":true}") != std::string::npos);
  }
  h.diffs.clear();
  h.library_ok = true;
  gallery_row k;
  k.path = "photos:ABC";
  k.name = "IMG_9.HEIC";
  h.library = {k};
  engine e(port_of(h));
  e.opened({1, "photos:ABC", 0, 2});
  e.wait_idle();
  std::lock_guard lock(h.m);
  REQUIRE(h.diffs.back().find("\"value\":\"library\",\"hidden\":false") != std::string::npos);
  REQUIRE(h.diffs.back().find("photos:ABC") != std::string::npos);
}

TEST_CASE("instagram: a choice makes a tile at the shape, and a new shape redraws it", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1), row("b.jpg", 2)};
  engine e(port_of(h, false));
  e.opened({1, "/f/a.jpg", 0x3478F6FF, 2});
  e.wait_idle();
  e.select(selection_of({"/f/a.jpg"}));
  e.wait_idle();
  {
    std::lock_guard lock(h.m);
    REQUIRE(h.surfaces.at("t1").first == 314);  // 157 pt at 2x: 4:5
    REQUIRE(h.surfaces.at("t1").second == 392);
  }
  e.change("shape", "\"1:1\"");
  e.wait_idle();
  std::lock_guard lock(h.m);
  REQUIRE(h.surfaces.at("t1").first == 392);
  REQUIRE(h.diffs.back().find("\"width\":196") != std::string::npos);
}

TEST_CASE("instagram: marked items are the post when the window opens", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1), row("b.mov", 2)};
  engine e(port_of(h, false));
  e.add_and_show({"/f/b.mov"});
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  std::lock_guard lock(h.m);
  bool seen = false;
  for (const std::string& d : h.diffs) seen = seen || d.find("Clip 1: b.mov") != std::string::npos;
  REQUIRE(seen);
}

TEST_CASE("instagram: the open command's lone path says where to choose from", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1)};
  engine e(port_of(h, false));
  e.open_command({"/f/a.jpg"});
  e.opened({1, "", 0, 2});
  e.wait_idle();
  std::lock_guard lock(h.m);
  REQUIRE(h.diffs.back().find("a.jpg") != std::string::npos);
  REQUIRE(h.diffs.back().find("\"id\":\"t") == std::string::npos);  // chose nothing
}

TEST_CASE("instagram: an export writes numbered files at the shape, then offers them", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1), row("b.jpg", 2)};
  engine e(port_of(h, false));
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/b.jpg"}));
  e.select(selection_of({"/f/a.jpg", "/f/b.jpg"}));
  e.wait_idle();
  e.change("shape", "\"1.91:1\"");
  e.click("export");
  e.wait_idle();
  {
    std::lock_guard lock(h.m);
    REQUIRE(h.stills_written.size() == 2);
    REQUIRE(std::get<0>(h.stills_written[0]) == "01");
    REQUIRE(std::get<1>(h.stills_written[0]) == 1080);
    REQUIRE(std::get<2>(h.stills_written[0]) == 566);
    REQUIRE(std::get<3>(h.stills_written[0]) == "/lib/Instagram/stamp");
    REQUIRE(h.busy.size() == 2);
    REQUIRE(h.busy.back().empty());
  }
  REQUIRE(e.written().size() == 2);
  e.click("show");
  std::lock_guard lock(h.m);
  REQUIRE(h.shown.size() == 2);
}

TEST_CASE("instagram: a clip is turned upright before it is cropped", "[instagram]") {
  struct reader final : clip_reader {
    int left = 2;
    std::uint32_t width() const noexcept override { return 1920; }
    std::uint32_t height() const noexcept override { return 1080; }
    std::uint32_t rotation() const noexcept override { return 90; }
    std::int64_t frame_count() const noexcept override { return 2; }
    bool next(std::vector<std::uint8_t>& rgb, std::int64_t& pts, bool&) override {
      if (left-- <= 0) return false;
      rgb.assign(1920u * 1080u * 3u, 90);
      pts = 1000 * left;
      return true;
    }
  };
  struct writer final : clip_writer {
    int* frames;
    explicit writer(int* f) : frames(f) {}
    bool frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t) override {
      REQUIRE(stride == 3240);
      REQUIRE(std::abs(int(rgb[0]) - 90) <= 1);
      ++*frames;
      return true;
    }
    std::string finish() override { return "/lib/01.mov"; }
  };
  fake_host h;
  gallery_row clip = row("c.mov", 1);
  h.folder = {clip};
  int frames = 0;
  std::uint32_t asked_w = 0, asked_h = 0;
  engine_port p = port_of(h, false);
  p.open_clip = [](const std::string&) -> std::unique_ptr<clip_reader> { return std::make_unique<reader>(); };
  p.write_clip = [&](const std::string&, std::uint32_t w, std::uint32_t ht, const std::string&,
                     const std::string&) -> std::unique_ptr<clip_writer> {
    asked_w = w;
    asked_h = ht;
    return std::make_unique<writer>(&frames);
  };
  engine e(std::move(p));
  e.opened({1, "/f/c.mov", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/c.mov"}));
  e.wait_idle();
  e.click("export");
  e.wait_idle();
  REQUIRE(frames == 2);
  REQUIRE(asked_w == 1080);
  REQUIRE(asked_h == 1350);
  REQUIRE(e.written() == std::vector<std::string>{"/lib/01.mov"});
}

TEST_CASE("instagram: adjustments show on the tile and bake into the export, keeping no file", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1)};
  engine_port p = port_of(h, false);
  std::atomic<int> adjusted_draws{0};
  std::atomic<std::uint32_t> last_w{0};
  p.adjust = [&](std::uint8_t*, std::uint32_t w, std::uint32_t, const adjustments& v) {
    REQUIRE(v[0] == 1.5f);
    ++adjusted_draws;
    last_w = w;
    return true;
  };
  engine e(std::move(p));
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/a.jpg"}));
  e.wait_idle();
  e.change("mode", "\"adjust\"");
  e.change("a.0", "50");  // half way up the slider: +1.5 EV
  e.wait_idle();
  REQUIRE(adjusted_draws.load() >= 2);  // the tile and the big preview
  {
    std::lock_guard lock(h.m);
    REQUIRE(h.diffs.back().find("\"id\":\"reset\"") != std::string::npos);  // Reset woke with the slider
    REQUIRE(h.surfaces.count("stage") == 1);
  }
  e.click("export");
  e.wait_idle();
  REQUIRE(last_w.load() == 1080);
  REQUIRE(e.written().size() == 1);
}

TEST_CASE("instagram: Auto fills the sliders and Reset clears them", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1)};
  engine_port p = port_of(h, false);
  p.adjust = [](std::uint8_t*, std::uint32_t, std::uint32_t, const adjustments&) { return true; };
  p.auto_adjust = [](const std::uint8_t*, std::uint32_t, std::uint32_t, adjustments& out) {
    out[1] = 25;
    return true;
  };
  engine e(std::move(p));
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/a.jpg"}));
  e.wait_idle();
  e.change("mode", "\"adjust\"");
  e.click("auto");
  e.wait_idle();
  {
    std::lock_guard lock(h.m);
    REQUIRE(h.diffs.back().find("\"id\":\"a.1\"") != std::string::npos);
    REQUIRE(h.diffs.back().find("\"value\":25.000") != std::string::npos);
  }
  e.click("reset");
  e.wait_idle();
  std::lock_guard lock(h.m);
  REQUIRE(h.diffs.back().find("\"value\":25.000") == std::string::npos);
}

TEST_CASE("instagram: Enhance is drawn here on the focused photo, and what it makes takes that place", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1), row("b.jpg", 2)};
  engine_port p = port_of(h, false);
  std::vector<std::string> opened;
  int closed = 0;
  p.embed_open = [&](std::uint64_t, const std::string& path) {
    opened.push_back(path);
    return true;
  };
  p.embed_close = [&] { ++closed; };
  engine e(std::move(p));
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/a.jpg"}));
  e.select(selection_of({"/f/a.jpg", "/f/b.jpg"}));
  e.wait_idle();
  e.change("mode", "\"enhance\"");
  e.wait_idle();
  REQUIRE(opened == std::vector<std::string>{"/f/b.jpg"});  // the last chosen has focus
  e.item_saved("/f/b.jpg", "/f/b-enhance.jpg");
  e.wait_idle();
  REQUIRE(opened.back() == "/f/b-enhance.jpg");  // the panel follows the new file
  e.item_saved("/not/in/the/post.jpg", "/f/other.jpg");
  e.wait_idle();
  REQUIRE(opened.size() == 2);
  e.change("mode", "\"choose\"");
  REQUIRE(closed == 1);
  e.click("export");
  e.wait_idle();
  REQUIRE(e.written().size() == 2);
}

TEST_CASE("instagram: without Enhance the panel says so, and a clip has no photo tools", "[instagram]") {
  post p;
  (void)p.add("/c.mov", "c.mov", true);
  (void)p.add("/a.jpg", "a.jpg", false);
  p.set_focus(p.items()[1].uid);
  view_state v;
  v.p = &p;
  v.mode = tool_mode::enhance;
  v.enhance_missing = true;
  mv::addon::screen::tree t;
  REQUIRE(t.set(tree_json(v)) == mv::addon::screen::error::none);
  REQUIRE(tree_json(v).find("Add or update Enhance") != std::string::npos);
  v.enhance_missing = false;
  REQUIRE(t.apply(panel_diff(v)) == mv::addon::screen::error::none);
  REQUIRE(panel_diff(v).find("\"type\":\"embed\"") != std::string::npos);
  v.mode = tool_mode::adjust;
  REQUIRE(t.apply(panel_diff(v)) == mv::addon::screen::error::none);
  REQUIRE(panel_diff(v).find("\"id\":\"a.0\"") != std::string::npos);
  p.set_focus(p.items()[0].uid);
  REQUIRE(panel_diff(v).find("These tools are for photos") != std::string::npos);
}

TEST_CASE("instagram: picking while exporting changes nothing", "[instagram]") {
  fake_host h;
  h.folder = {row("a.jpg", 1), row("b.jpg", 2)};
  engine_port p = port_of(h, false);
  std::atomic<bool> release{false};
  p.still = [&release](const std::string&, image& out) {
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    out = solid(100, 100, 1);
    return true;
  };
  engine e(std::move(p));
  e.opened({1, "/f/a.jpg", 0, 2});
  e.wait_idle();
  e.select(selection_of({"/f/a.jpg"}));
  e.wait_idle();
  e.click("export");
  e.select(selection_of({"/f/a.jpg", "/f/b.jpg"}));
  release.store(true);
  e.wait_idle();
  REQUIRE(e.written().size() == 1);
}
