// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297 slices 1 and 2): the engine over a fixture folder and a fake
// transport.
//
// Slice 1: a model's weights live outside the zip. Nothing is requested
// before the person's click; the click fetches only that kind's pins, a plain
// GET of the pinned URL; a cut transfer resumes with Range; a body that misses
// its SHA-256 is never kept; a pin over 5 GB, or a disk without room, is
// refused before any request; a restart resumes what was asked for.
//
// Slice 2: the sheet follows the item. A still, a clip and an audio file each
// show only their own knobs, read from model.json; a clip offers picture,
// sound or both; values persist per kind; a kind with no installed model is
// named, not offered.
#include "catch_compat.h"
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addons/ai/sha256.h"
#include "addons/enhance/diffusion.h"
#include "addons/enhance/remove.h"
#include "addon/keep_ranges.h"
#include "addon/screen.h"
#include "addons/enhance/brighten.h"
#include "addons/enhance/engine.h"
#include "addons/enhance/picture_job.h"
#include "addons/enhance/preview.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/sound_job.h"
#include "addons/enhance/stream_model.h"
#include "addons/enhance/screens.h"
#include "addons/enhance/sniff.h"
#include "addons/enhance/spec.h"
#include "core/json.h"
#include "infer/ort.h"
#include "import_fixture.h"

using namespace mv::enhance;
using mv::test::scratch_dir;
using mv::test::utf8;
using mv::test::write_bytes;
using mv::test::write_text;
using Catch::Approx;

namespace {

namespace fs = std::filesystem;

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 1000; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

std::string hex_of(const std::vector<std::uint8_t>& bytes) {
  mv::ai::sha256 h;
  h.update(bytes);
  return h.finish_hex();
}

// Serves files by URL, from the download's own thread.
struct fake_net final : mv::ai::http_transport {
  struct shared {
    std::mutex m;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::vector<std::pair<std::string, std::uint64_t>> calls;
    // Bytes to send on request n before cutting; negative sends all.
    std::function<long long(std::size_t n)> cut = [](std::size_t) { return -1LL; };
    std::size_t count() {
      std::lock_guard lock(m);
      return calls.size();
    }
  };
  std::shared_ptr<shared> s;
  explicit fake_net(std::shared_ptr<shared> sh) : s(std::move(sh)) {}

  mv::ai::http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int)>& head,
                         const std::function<bool(std::span<const std::uint8_t>)>& body) override {
    std::vector<std::uint8_t> data;
    long long cut = -1;
    {
      std::lock_guard lock(s->m);
      const std::size_t n = s->calls.size();
      s->calls.emplace_back(url, from);
      cut = s->cut(n);
      auto it = s->files.find(url);
      if (it == s->files.end()) {
        mv::ai::http_reply r;
        r.status = 404;
        (void)head(404);
        return r;
      }
      data = it->second;
    }
    const int code = from ? 206 : 200;
    mv::ai::http_reply r;
    r.status = code;
    if (!head(code)) return r;
    std::uint64_t end = data.size();
    if (cut >= 0) end = std::min<std::uint64_t>(end, from + static_cast<std::uint64_t>(cut));
    for (std::uint64_t at = from; at < end; at += 1024) {
      const std::uint64_t k = std::min<std::uint64_t>(1024, end - at);
      if (!body(std::span<const std::uint8_t>(data.data() + at, k))) break;
    }
    return r;
  }
};

// The screens the engine posts, by id, as the host would keep them.
struct fake_host {
  std::mutex m;
  std::map<std::string, std::string> trees;
  std::map<std::string, int> shown;
  std::vector<std::string> logs;
  std::uint64_t free = 1ull << 40;
  mv::enhance::media_facts facts{true, true};
  std::vector<float> brush;  // slice 7: the dabs the host holds over a photo
  std::vector<std::string> recycled;  // S5's Discard: what went to the Bin
  int recycle_answer = 0;             // 0 moved, 1 refused (no bin), -1 failed
  // The preview's last pixels and how many were posted.
  std::uint32_t surface_w = 0, surface_h = 0;
  int surfaces = 0;

  std::string tree(const std::string& screen) {
    std::lock_guard lock(m);
    auto it = trees.find(screen);
    return it == trees.end() ? std::string() : it->second;
  }
};

const std::string kAudioUrl = "https://huggingface.co/mv-test/denoise/resolve/0123abcd/denoise.onnx";
const std::string kVideoUrl = "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx";

const char* kAudioSpec = R"json({
  "kind": "audio", "name": "Speech cleanup", "credit": "Test model (MIT)",
  "weights": ["denoise.onnx"], "run_bytes": 400000000, "cpu": true,
  "knobs": [
    {"id": "noise", "label": "Noise reduction", "type": "slider", "min": 0, "max": 1, "step": 0.05, "default": 0.8},
    {"id": "loudness", "label": "Even out loudness", "type": "toggle", "default": true}
  ]
})json";

const char* kVideoSpec = R"json({
  "kind": "video", "name": "Detail", "credit": "Test model (BSD-3-Clause)",
  "weights": ["up.onnx"], "run_bytes": 2000000000,
  "knobs": [
    {"id": "scale", "label": "Scale", "type": "choice",
     "options": [{"id": "1", "text": "Same size"}, {"id": "2", "text": "2×"}], "default": "2"},
    {"id": "face", "label": "Face detail", "type": "toggle", "default": false, "kinds": ["still"]}
  ]
})json";

struct fixture {
  scratch_dir dir{"enhance"};
  fs::path self = dir / "self";
  fs::path data = dir / "data";
  std::vector<std::uint8_t> audio_weights = mv::test::pattern(50'000, 1);
  std::vector<std::uint8_t> video_weights = mv::test::pattern(80'000, 2);
  std::shared_ptr<fake_net::shared> net = std::make_shared<fake_net::shared>();
  fake_host host;
  std::unique_ptr<engine> eng;

  explicit fixture(std::uint64_t audio_pin_size = 0, std::string audio_sha = "") {
    if (audio_pin_size == 0) audio_pin_size = audio_weights.size();
    if (audio_sha.empty()) audio_sha = hex_of(audio_weights);
    mv::json::writer w;
    w.begin_object().key("id").string("enhance").key("remote").begin_array();
    w.begin_object().key("url").string(kAudioUrl).key("path").string("audio/denoise.onnx")
        .key("size").integer(static_cast<std::int64_t>(audio_pin_size)).key("sha256").string(audio_sha)
        .key("licence").string("MIT").end_object();
    w.begin_object().key("url").string(kVideoUrl).key("path").string("video/up.onnx")
        .key("size").integer(static_cast<std::int64_t>(video_weights.size())).key("sha256").string(hex_of(video_weights))
        .key("licence").string("BSD-3-Clause").end_object();
    w.end_array().end_object();
    write_text(self / "manifest.json", w.take());
    write_text(self / "models/audio/model.json", kAudioSpec);
    write_text(self / "models/video/model.json", kVideoSpec);
    fs::create_directories(data);
    net->files[kAudioUrl] = audio_weights;
    net->files[kVideoUrl] = video_weights;
    start();
  }

  void start() {
    engine_port p;
    p.post = [this](const char* screen, std::uint64_t, const std::string& tree) {
      std::lock_guard lock(host.m);
      host.trees[screen] = tree;
      return true;
    };
    p.surface = [this](const char* screen, const char* surface, std::uint64_t, const std::vector<std::uint8_t>& bgra,
                       std::uint32_t w, std::uint32_t h) {
      std::lock_guard lock(host.m);
      CHECK((std::string(screen) == kSheetScreen || std::string(screen) == kToolScreen));
      CHECK(std::string(surface) == "preview");
      CHECK(bgra.size() == std::size_t{w} * h * 4);
      // The node is in the tree before its pixels (else the host drops them).
      CHECK(host.trees[screen].find("\"surface\":\"preview\"") != std::string::npos);
      host.surface_w = w;
      host.surface_h = h;
      ++host.surfaces;
      return true;
    };
    p.show = [this](const char* screen, bool show) {
      std::lock_guard lock(host.m);
      host.shown[screen] = show ? 1 : 0;
    };
    p.probe = [this](const std::string&) { return host.facts; };
    p.brushed = [this](const std::string&) {
      std::lock_guard lock(host.m);
      return static_cast<std::uint32_t>(host.brush.size() / 3);
    };
    p.recycle = [this](const std::string& path) {
      std::lock_guard lock(host.m);
      host.recycled.push_back(path);
      return host.recycle_answer;
    };
    p.free_disk = [this](const std::string&) -> std::optional<std::uint64_t> { return host.free; };
    auto shared = net;
    p.transport = [shared] { return std::make_unique<fake_net>(shared); };
    p.log = [this](int, const char* msg) {
      std::lock_guard lock(host.m);
      host.logs.emplace_back(msg);
    };
    mv::ai::fetch_options o;
    o.wait = [](double, const std::atomic<bool>& stop) { return !stop.load(); };
    eng = std::make_unique<engine>(std::move(p), utf8(self), utf8(data), o);
  }
  void restart() {
    eng.reset();
    start();
  }
};

// The node with `id` in a posted tree, or null.
const mv::json::value* find_node(const mv::json::value& n, const std::string& id) {
  if (const std::string* nid = n.str("id"); nid && *nid == id) return &n;
  if (const mv::json::value* kids = n.find("children"); kids && kids->k == mv::json::kind::array) {
    for (const mv::json::value& c : kids->a) {
      if (const mv::json::value* f = find_node(c, id)) return f;
    }
  }
  return nullptr;
}

struct parsed {
  std::optional<mv::json::value> doc;
  explicit parsed(const std::string& tree) : doc(mv::json::parse(tree, 64)) {}
  [[nodiscard]] const mv::json::value* node(const std::string& id) const { return doc ? find_node(*doc, id) : nullptr; }
  [[nodiscard]] bool has(const std::string& id) const { return node(id) != nullptr; }
  [[nodiscard]] std::string text(const std::string& id) const {
    const mv::json::value* n = node(id);
    const std::string* t = n ? n->str("text") : nullptr;
    return t ? *t : std::string();
  }
};

std::vector<std::uint8_t> bytes(std::initializer_list<int> b) {
  std::vector<std::uint8_t> out;
  for (int x : b) out.push_back(static_cast<std::uint8_t>(x));
  out.resize(64, 0);
  return out;
}

std::vector<std::uint8_t> ftyp(const char* major, const char* compat = "isom") {
  std::vector<std::uint8_t> h = {0, 0, 0, 24, 'f', 't', 'y', 'p'};
  for (int i = 0; i < 4; ++i) h.push_back(static_cast<std::uint8_t>(major[i]));
  for (int i = 0; i < 4; ++i) h.push_back(0);
  for (int i = 0; i < 4; ++i) h.push_back(static_cast<std::uint8_t>(compat[i]));
  for (int i = 0; i < 4; ++i) h.push_back(static_cast<std::uint8_t>(compat[i]));
  h.resize(64, 0);
  return h;
}

}  // namespace

TEST_CASE("enhance: model.json is read and checked", "[enhance]") {
  auto s = parse_spec(kAudioSpec, model_kind::audio);
  REQUIRE(s);
  CHECK(s->name == "Speech cleanup");
  CHECK(s->weights == std::vector<std::string>{"denoise.onnx"});
  CHECK(s->run_bytes == 400000000u);
  CHECK(s->cpu);
  REQUIRE(s->knobs.size() == 2);
  CHECK(s->knobs[0].kinds == std::vector<model_kind>{model_kind::audio});  // its own kind by default
  CHECK(near(knob_value(s->knobs[0], nullptr).d, 0.8));

  auto v = parse_spec(kVideoSpec, model_kind::video);
  REQUIRE(v);
  CHECK(v->knobs[1].kinds == std::vector<model_kind>{model_kind::still});

  // Another kind's file, and malformed ones, are refused whole.
  CHECK_FALSE(parse_spec(kAudioSpec, model_kind::video));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":[],"run_bytes":1})", model_kind::audio));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":["../a"],"run_bytes":1})", model_kind::audio));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":["a"],"run_bytes":1,
    "knobs":[{"id":"n","label":"N","type":"slider","min":1,"max":1,"default":1}]})", model_kind::audio));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":["a"],"run_bytes":1,
    "knobs":[{"id":"n","label":"N","type":"dial","default":1}]})", model_kind::audio));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":["a"],"run_bytes":1,
    "knobs":[{"id":"n","label":"N","type":"toggle","default":true},{"id":"n","label":"M","type":"toggle","default":true}]})",
                         model_kind::audio));
  CHECK_FALSE(parse_spec(R"({"kind":"audio","name":"x","weights":["a"],"run_bytes":1,
    "knobs":[{"id":"n","label":"N","type":"choice","options":[{"id":"a","text":"A"}],"default":"b"}]})",
                         model_kind::audio));

  // Values the sheet reports are checked against the knob.
  const knob& noise = s->knobs[0];
  CHECK(check_knob_value(noise, *mv::json::parse("0.5")));
  CHECK_FALSE(check_knob_value(noise, *mv::json::parse("1.5")));
  CHECK_FALSE(check_knob_value(noise, *mv::json::parse("true")));
  const knob& scale = v->knobs[0];
  CHECK(check_knob_value(scale, *mv::json::parse("\"1\"")));
  CHECK_FALSE(check_knob_value(scale, *mv::json::parse("\"4\"")));
  // A stored value out of range falls back to the default.
  const auto stale = mv::json::parse("7");
  CHECK(near(knob_value(noise, &*stale).d, 0.8));
}

TEST_CASE("enhance: the item on screen is told by its first bytes", "[enhance]") {
  CHECK(sniff(bytes({0xFF, 0xD8, 0xFF, 0xE1})) == item_kind::still);
  CHECK(sniff(bytes({0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'})) == item_kind::still);
  CHECK(sniff(ftyp("heic")) == item_kind::still);
  CHECK(sniff(ftyp("isom", "mif1")) == item_kind::still);
  CHECK(sniff(ftyp("isom", "mp41")) == item_kind::media);
  CHECK(sniff(ftyp("qt  ", "qt  ")) == item_kind::media);
  CHECK(sniff(ftyp("M4A ", "mp42")) == item_kind::audio);
  CHECK(sniff(ftyp("M4P ", "mp42")) == item_kind::audio);
  CHECK(sniff(bytes({'I', 'D', '3', 4, 0})) == item_kind::audio);
  CHECK(sniff(bytes({0xFF, 0xFB, 0x90, 0x00})) == item_kind::audio);
  CHECK(sniff(bytes({0x1a, 0x45, 0xdf, 0xa3})) == item_kind::media);
  CHECK(sniff(bytes({'%', 'P', 'D', 'F', '-', '1'})) == item_kind::document);
  CHECK(sniff(bytes({'P', 'K', 3, 4})) == item_kind::document);
  CHECK(sniff(bytes({'G', 'I', 'F', '8', '9', 'a'})) == item_kind::animation);
  CHECK(sniff(bytes({1, 2, 3, 4})) == item_kind::unknown);
  CHECK(sniff({}) == item_kind::unknown);
}

TEST_CASE("enhance: prefs keep values per kind, the target and what was asked for", "[enhance]") {
  prefs p;
  p.of(model_kind::audio)["noise"] = *mv::json::parse("0.25");
  p.of(model_kind::video)["scale"] = *mv::json::parse("\"1\"");
  p.target = clip_target::both;
  p.want(model_kind::audio) = true;
  const prefs q = prefs_from(prefs_json(p));
  CHECK(near(q.of(model_kind::audio).at("noise").d, 0.25));
  CHECK(q.of(model_kind::video).at("scale").s == "1");
  CHECK(q.of(model_kind::still).empty());
  CHECK(q.target == clip_target::both);
  CHECK(q.want(model_kind::audio));
  CHECK_FALSE(q.want(model_kind::video));
  // Junk is ignored, never fatal.
  const prefs r = prefs_from(R"({"values":{"audio":{"noise":[1]},"fax":{}},"target":"x","wanted":["fax",3]})");
  CHECK(r.of(model_kind::audio).empty());
  CHECK(r.target == clip_target::picture);
  CHECK_FALSE(r.want(model_kind::audio));
}

TEST_CASE("enhance: sizes read as the install line says them", "[enhance]") {
  CHECK(size_text(12'000'000) == "12 MB");
  CHECK(size_text(4'200'000'000ull) == "4.2 GB");
  CHECK(size_text(300) == "1 MB");
}

TEST_CASE("enhance: nothing is requested before the click, then only that kind's pins", "[enhance]") {
  fixture f;
  f.eng->resume_wanted();
  f.eng->settings_opened(1);
  parsed before(f.host.tree(kSettingsScreen));
  CHECK(before.has("install.audio"));
  CHECK(before.has("install.video"));
  CHECK(before.text("none.still").find("no photo model") != std::string::npos);
  CHECK(before.text("privacy").find("network address") != std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(f.net->count() == 0);

  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::audio).state == dl_state::ready; }));
  {
    std::lock_guard lock(f.net->m);
    REQUIRE(f.net->calls.size() == 1);
    CHECK(f.net->calls[0].first == kAudioUrl);  // the pinned URL, no query
    CHECK(f.net->calls[0].first.find('?') == std::string::npos);
  }
  CHECK(mv::test::read_bytes(f.data / "models/audio/denoise.onnx") == f.audio_weights);
  CHECK(f.eng->download_status(model_kind::video).state == dl_state::absent);
  CHECK(f.eng->current_prefs().want(model_kind::audio));
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSettingsScreen)).has("remove.audio"); }));
  CHECK(parsed(f.host.tree(kSettingsScreen)).text("state.audio").rfind("Installed", 0) == 0);

  // Remove deletes the weights and forgets the choice.
  f.eng->event(kSettingsScreen, "remove.audio", MV_SLOT_EVENT_CLICK, "");
  CHECK_FALSE(fs::exists(f.data / "models/audio/denoise.onnx"));
  CHECK(f.eng->download_status(model_kind::audio).state == dl_state::absent);
  CHECK_FALSE(f.eng->current_prefs().want(model_kind::audio));
}

TEST_CASE("enhance: a cut download resumes with Range, also after a restart", "[enhance]") {
  fixture f;
  // The first request is cut after 20 kB; the second, after the restart, gets the rest.
  f.net->cut = [](std::size_t n) { return n == 0 ? 20'000LL : -1LL; };
  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  // A cut is a wait-and-retry; the retry happens at once with this wait, so
  // let it finish and check the second request asked from byte 20000.
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::audio).state == dl_state::ready; }));
  {
    std::lock_guard lock(f.net->m);
    REQUIRE(f.net->calls.size() == 2);
    CHECK(f.net->calls[1].second == 20'000u);
  }

  // A restart mid-download: the person's click is remembered, the part kept.
  fixture g;
  g.net->cut = [](std::size_t n) { return n == 0 ? 30'000LL : -1LL; };
  // Stop after the cut, as a quit would.
  g.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return g.net->count() >= 1; }));
  g.restart();  // the engine stops its download (the part stays)
  g.net->cut = [](std::size_t) { return -1LL; };
  if (g.eng->download_status(model_kind::video).state != dl_state::ready) {
    CHECK(g.eng->current_prefs().want(model_kind::video));
    g.eng->resume_wanted();
    REQUIRE(eventually([&] { return g.eng->download_status(model_kind::video).state == dl_state::ready; }));
  }
  CHECK(mv::test::read_bytes(g.data / "models/video/up.onnx") == g.video_weights);
}

TEST_CASE("enhance: a restart never fetches a wanted kind with nothing here (a changed pin)", "[enhance]") {
  // S7 moved still's pin from a 4.9 MB file to a 161 MB one: someone who
  // installed the old file must click again, never get the new one unasked.
  fixture f;
  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::audio).state == dl_state::ready; }));
  const std::size_t calls = f.net->count();
  f.restart();
  fs::remove(f.data / "models/audio/denoise.onnx");  // the pinned file is not the one here
  REQUIRE(f.eng->current_prefs().want(model_kind::audio));
  f.eng->resume_wanted();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(f.net->count() == calls);
  CHECK(f.eng->download_status(model_kind::audio).state == dl_state::absent);
}

TEST_CASE("enhance: a body that misses its hash is never kept", "[enhance]") {
  fixture f(0, std::string(64, 'a'));
  f.eng->settings_opened(1);
  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::audio).state == dl_state::failed; }));
  CHECK_FALSE(fs::exists(f.data / "models/audio/denoise.onnx"));
  CHECK_FALSE(fs::exists(f.data / "models/audio/denoise.onnx.part"));
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSettingsScreen)).text("state.audio").find("checksum") != std::string::npos; }));
}

TEST_CASE("enhance: a pin over 5 GB, or a full disk, is refused before any request", "[enhance]") {
  fixture big(kMaxWeightsBytes + 1);
  big.eng->settings_opened(1);
  big.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  CHECK(big.eng->download_status(model_kind::audio).state == dl_state::refused);
  CHECK(big.net->count() == 0);

  fixture full;
  full.host.free = 10'000;
  full.eng->settings_opened(1);
  full.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  const dl_status st = full.eng->download_status(model_kind::audio);
  CHECK(st.state == dl_state::no_space);
  CHECK(st.need_bytes > full.audio_weights.size());
  CHECK(full.net->count() == 0);
  CHECK(parsed(full.host.tree(kSettingsScreen)).text("state.audio").find("Not enough free disk") != std::string::npos);
}

TEST_CASE("enhance: the sheet shows only the item's own knobs", "[enhance]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0001.JPG";
  const fs::path song = f.dir / "song.mp3";
  const fs::path clip = f.dir / "clip.mov";
  const fs::path doc = f.dir / "notes.pdf";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_bytes(song, bytes({'I', 'D', '3', 4}));
  write_bytes(clip, ftyp("qt  ", "qt  "));
  write_bytes(doc, bytes({'%', 'P', 'D', 'F', '-'}));

  // Before any model is installed: named, not offered; Run greyed.
  f.eng->command({utf8(song)});
  CHECK(f.host.shown[kSheetScreen] == 1);
  f.eng->sheet_opened(5, "");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.text("file") == "song.mp3");
    CHECK(t.text("missing.audio").find("not installed") != std::string::npos);
    CHECK_FALSE(t.has("k.audio.noise"));
    CHECK(t.node("run")->boolean("disabled").value_or(false));
    CHECK_FALSE(t.has("target"));
  }

  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  f.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] {
    return f.eng->download_status(model_kind::audio).state == dl_state::ready &&
           f.eng->download_status(model_kind::video).state == dl_state::ready;
  }));

  // An audio file: the audio model's knobs, nothing of the video model's.
  f.eng->sheet_opened(6, utf8(song));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.has("k.audio.noise"));
    CHECK(t.has("k.audio.loudness"));
    CHECK_FALSE(t.has("k.video.scale"));
    CHECK(near(t.node("k.audio.noise")->find("value")->d, 0.8));
  }
  // A changed knob is remembered for audio.
  f.eng->event(kSheetScreen, "k.audio.noise", MV_SLOT_EVENT_CHANGE, "0.35");
  f.eng->event(kSheetScreen, "k.audio.noise", MV_SLOT_EVENT_CHANGE, "9");  // refused: out of range
  CHECK(near(f.eng->current_prefs().of(model_kind::audio).at("noise").d, 0.35));

  // A clip with a picture and sound: picture by default, its own knobs only
  // (the video model's still-only knob is not on a clip's sheet).
  f.eng->sheet_opened(7, utf8(clip));
  {
    parsed t(f.host.tree(kSheetScreen));
    REQUIRE(t.has("target"));
    CHECK(t.node("target")->find("options")->a.size() == 3);
    CHECK(t.has("k.video.scale"));
    CHECK_FALSE(t.has("k.video.face"));
    CHECK_FALSE(t.has("k.audio.noise"));
  }
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"sound\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK_FALSE(t.has("k.video.scale"));
    CHECK(t.has("k.audio.noise"));
    CHECK(near(t.node("k.audio.noise")->find("value")->d, 0.35));  // per kind, across items
  }
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"both\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.has("k.video.scale"));
    CHECK(t.has("k.audio.noise"));
    // No runner in this build: the models are there but Run stays greyed.
    CHECK(t.node("run")->boolean("disabled").value_or(false));
    CHECK(t.text("status").find("cannot run") != std::string::npos);
  }
  CHECK(f.eng->current_prefs().target == clip_target::both);

  // A clip with no sound offers no target choice.
  f.host.facts = {true, false};
  f.eng->sheet_opened(8, utf8(clip));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK_FALSE(t.has("target"));
    CHECK(t.has("k.video.scale"));
    CHECK_FALSE(t.has("k.audio.noise"));
  }

  // A still: this build has no photo model, so it is named and Run greyed.
  f.eng->sheet_opened(9, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.text("missing.still").find("no photo model") != std::string::npos);
    CHECK_FALSE(t.has("k.video.face"));
    CHECK(t.node("run")->boolean("disabled").value_or(false));
  }

  // A document: always greyed.
  f.eng->sheet_opened(10, utf8(doc));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.text("note").find("not on documents") != std::string::npos);
    CHECK(t.node("run")->boolean("disabled").value_or(false));
  }

  // Values survive a restart, per kind.
  f.restart();
  CHECK(near(f.eng->current_prefs().of(model_kind::audio).at("noise").d, 0.35));
  CHECK(f.eng->current_prefs().target == clip_target::both);
}

// ---- slice 4: sound -------------------------------------------------------------

namespace {

// A model whose output is its input `lag` samples late, times `gain`: what a
// streaming denoiser looks like to the pipeline, minus the denoising.
struct lag_model final : frame_model {
  std::size_t f, lag;
  float gain;
  lag_model(std::size_t frame_, std::size_t lag_, float gain_) : f(frame_), lag(lag_), gain(gain_) {}
  std::size_t frame() const noexcept override { return f; }
  std::size_t delay() const noexcept override { return lag; }
  std::vector<float> initial_state() const override { return std::vector<float>(lag, 0.0f); }
  mv::expected process(std::span<const float> in, std::vector<float>& state, std::span<float> out) override {
    std::vector<float> line(state);
    line.insert(line.end(), in.begin(), in.end());
    for (std::size_t i = 0; i < f; ++i) out[i] = gain * line[i];
    state.assign(line.end() - static_cast<std::ptrdiff_t>(lag), line.end());
    return {};
  }
};

std::vector<float> ramp(std::size_t frames, std::uint32_t ch) {
  std::vector<float> v(frames * ch);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i % 997) / 997.0f - 0.5f;
  return v;
}

sound_source source_of(const std::vector<float>& pcm, std::uint32_t ch, std::size_t* at) {
  sound_source s;
  s.channels = ch;
  s.total_frames = static_cast<std::int64_t>(pcm.size() / ch);
  s.read = [&pcm, ch, at](std::size_t max_frames) -> mv::result<std::vector<float>> {
    const std::size_t n = std::min(max_frames * ch, pcm.size() - *at);
    std::vector<float> out(pcm.begin() + static_cast<std::ptrdiff_t>(*at),
                           pcm.begin() + static_cast<std::ptrdiff_t>(*at + n));
    *at += n;
    return out;
  };
  return s;
}

}  // namespace

TEST_CASE("enhance: the sound lines up with the original, sample for sample", "[enhance][sound]") {
  for (const std::uint32_t ch : {1u, 2u}) {
    for (const std::size_t frames : {std::size_t{0}, std::size_t{100}, std::size_t{4801}, std::size_t{48000}}) {
      const auto pcm = ramp(frames, ch);
      std::size_t at = 0;
      lag_model m(480, 1440, 0.5f);
      std::vector<float> got;
      sound_sink_port sink;
      sink.write = [&](std::span<const float> v) -> mv::expected {
        got.insert(got.end(), v.begin(), v.end());
        return {};
      };
      std::vector<double> progress;
      sound_job_options o;
      o.progress = [&](double f) { progress.push_back(f); };
      REQUIRE(run_sound_job(source_of(pcm, ch, &at), m, sink, o));
      // The lag is gone: the model's half-level copy of the input, in place.
      REQUIRE(got.size() == pcm.size());
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (!near(got[i], 0.5 * pcm[i])) FAIL("sample " << i << " of " << pcm.size() << ", " << ch << " channel(s)");
      }
      if (frames > 0) CHECK(near(progress.back(), 1.0));
    }
  }
}

TEST_CASE("enhance: strength mixes the model with the original", "[enhance][sound]") {
  const auto pcm = ramp(10'000, 2);
  for (const double strength : {0.0, 0.25, 1.0}) {
    std::size_t at = 0;
    lag_model m(480, 1440, 0.0f);  // a model that removes everything
    std::vector<float> got;
    sound_sink_port sink;
    sink.write = [&](std::span<const float> v) -> mv::expected {
      got.insert(got.end(), v.begin(), v.end());
      return {};
    };
    sound_job_options o;
    o.strength = strength;
    REQUIRE(run_sound_job(source_of(pcm, 2, &at), m, sink, o));
    REQUIRE(got.size() == pcm.size());
    CHECK(near(got[5001], (1.0 - strength) * pcm[5001]));
  }
}

TEST_CASE("enhance: a cancelled sound job stops", "[enhance][sound]") {
  const auto pcm = ramp(48'000, 1);
  std::size_t at = 0;
  lag_model m(480, 0, 1.0f);
  std::atomic<bool> cancel{false};
  sound_sink_port sink;
  sink.write = [&](std::span<const float>) -> mv::expected {
    cancel = true;  // the person pressed Cancel while it wrote
    return {};
  };
  sound_job_options o;
  o.cancel = &cancel;
  const auto r = run_sound_job(source_of(pcm, 1, &at), m, sink, o);
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::cancelled);
}

TEST_CASE("enhance: Enhance on an audio file runs the model and names the new file", "[enhance][sound]") {
  fixture f;
  const fs::path song = f.dir / "song.mp3";
  write_bytes(song, bytes({'I', 'D', '3', 4}));
  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::audio).state == dl_state::ready; }));

  // The audio model of the fixture is not a streaming one: give it the io
  // section the shipped spec has, through a spec the runner can read.
  const auto pcm = ramp(48'000, 2);
  std::vector<float> written;
  std::atomic<bool> finished{false};
  std::atomic<std::uint64_t> free_memory{8ull << 30};
  run_port port;
  port.open_sound = [&](const std::string& path, std::uint32_t rate, std::span<const std::int64_t>) -> mv::result<run_port::reader> {
    CHECK(path == utf8(song));
    CHECK(rate == 48000u);
    run_port::reader r;
    r.channels = 2;
    r.total_frames = 48'000;
    auto at = std::make_shared<std::size_t>(0);
    r.read = [&pcm, at](std::size_t max_frames) -> mv::result<std::vector<float>> {
      const std::size_t n = std::min(max_frames * 2, pcm.size() - *at);
      std::vector<float> out(pcm.begin() + static_cast<std::ptrdiff_t>(*at),
                             pcm.begin() + static_cast<std::ptrdiff_t>(*at + n));
      *at += n;
      return out;
    };
    return r;
  };
  port.create = [&](const std::string& source, std::uint32_t, std::uint32_t channels) -> mv::result<run_port::writer> {
    CHECK(source == utf8(song));
    CHECK(channels == 2u);
    run_port::writer w;
    w.write = [&](std::span<const float> v) -> mv::expected {
      written.insert(written.end(), v.begin(), v.end());
      return {};
    };
    w.finish = [&]() -> mv::result<std::string> {
      finished = true;
      return utf8(f.dir / "song-enhance.m4a");
    };
    return w;
  };
  port.load_model = [](const model_spec&, const std::string& dir) -> mv::result<std::unique_ptr<frame_model>> {
    CHECK(dir.find("models/audio") != std::string::npos);
    return std::unique_ptr<frame_model>(new lag_model(480, 1440, 1.0f));
  };
  port.free_memory = [&]() -> std::optional<std::uint64_t> { return free_memory.load(); };

  // A model.json the runner can run: the fixture's knobs and weights, the
  // shipped io section.
  write_text(f.self / "models/audio/model.json", R"json({
    "kind": "audio", "name": "Speech cleanup", "weights": ["denoise.onnx"], "run_bytes": 150000000, "cpu": true,
    "knobs": [{"id": "strength", "label": "Noise reduction", "type": "slider", "min": 0, "max": 1, "default": 1}],
    "io": {"runner": "stream", "rate": 48000, "frame": 480, "state": 45304, "delay": 1440}})json");
  f.restart();
  f.eng->set_runner(std::make_unique<job_runner>(port));

  f.eng->sheet_opened(1, utf8(song));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.has("k.audio.strength"));
    CHECK_FALSE(t.node("run")->boolean("disabled").value_or(false));
  }
  // The run gate: too little memory, and it says so without starting.
  free_memory = 10'000'000;
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  CHECK(parsed(f.host.tree(kSheetScreen)).text("status").find("free memory") != std::string::npos);
  CHECK(written.empty());

  free_memory = 8ull << 30;
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(finished);
  CHECK(parsed(f.host.tree(kSheetScreen)).text("status") == "Saved as song-enhance.m4a.");
  REQUIRE(written.size() == pcm.size());
  CHECK(near(written[1234], pcm[1234]));
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).node("run")->boolean("disabled").value_or(false));

  // A clip with both: Run is greyed until its picture can be enhanced too.
  f.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::video).state == dl_state::ready; }));
  const fs::path clip = f.dir / "clip.mov";
  write_bytes(clip, ftyp("qt  ", "qt  "));
  f.eng->sheet_opened(2, utf8(clip));
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"both\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.node("run")->boolean("disabled").value_or(false));
    CHECK(t.text("status").find("clip's picture") != std::string::npos);
  }
  // Its sound alone runs.
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"sound\"");
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).node("run")->boolean("disabled").value_or(false));
}

// The shipped model on ONNX Runtime, when it is here: MV_ENHANCE_DFN3 names the
// downloaded denoiser_model.onnx; ONNX Runtime is the one staged beside
// mv_enhance (MV_ENHANCE_RUNTIME_DIR). A tone under noise comes out with less
// of the noise, in place.
TEST_CASE("enhance: DeepFilterNet3 cleans a noisy tone", "[.enhance-model]") {
  const char* model = std::getenv("MV_ENHANCE_DFN3");
  const char* runtime = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  if (!model || !runtime) SKIP("MV_ENHANCE_DFN3 / MV_ENHANCE_RUNTIME_DIR not set");
  const auto text = mv::test::read_bytes(MV_ENHANCE_AUDIO_SPEC);
  auto spec = parse_spec(std::string(text.begin(), text.end()), model_kind::audio);
  REQUIRE(spec);
  const fs::path file(model);
  runtime_holder rt(runtime);
  auto m = rt.open(*spec, utf8(file.parent_path()));
  REQUIRE(m);
  // Speech-band harmonics under white noise at about 5 dB SNR, 3 s mono.
  const std::size_t n = 3 * 48000;
  std::vector<float> clean(n), noisy(n);
  std::uint32_t seed = 7;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / 48000;
    const double env = 0.5 + 0.5 * std::sin(2 * 3.14159265 * 3 * t);
    clean[i] = static_cast<float>(0.2 * env * (std::sin(2 * 3.14159265 * 220 * t) + 0.5 * std::sin(2 * 3.14159265 * 440 * t)));
    seed = seed * 1664525u + 1013904223u;
    noisy[i] = clean[i] + 0.06f * (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 3.46f;
  }
  std::size_t at = 0;
  std::vector<float> out;
  sound_sink_port sink;
  sink.write = [&](std::span<const float> v) -> mv::expected {
    out.insert(out.end(), v.begin(), v.end());
    return {};
  };
  REQUIRE(run_sound_job(source_of(noisy, 1, &at), **m, sink, {}));
  REQUIRE(out.size() == n);
  double before = 0, after = 0;
  for (std::size_t i = 4800; i < n - 4800; ++i) {
    before += (noisy[i] - clean[i]) * (noisy[i] - clean[i]);
    after += (out[i] - clean[i]) * (out[i] - clean[i]);
  }
  INFO("error energy " << before << " -> " << after);
  CHECK(after < before);
}

// ---- slice 5: a clip's picture -----------------------------------------------------

namespace {

// Nearest-neighbour 4x: an upscaler with no opinions, so the tiling is all
// that can be wrong.
struct nearest4 final : tile_model {
  std::uint32_t t;
  int calls = 0;
  explicit nearest4(std::uint32_t tile_) : t(tile_) {}
  std::uint32_t tile() const noexcept override { return t; }
  std::uint32_t scale() const noexcept override { return 4; }
  mv::expected run(std::span<const float> in, std::vector<float>& out) override {
    ++calls;
    const std::uint32_t o = t * 4;
    out.assign(std::size_t{3} * o * o, 0);
    for (int c = 0; c < 3; ++c) {
      for (std::uint32_t y = 0; y < o; ++y) {
        for (std::uint32_t x = 0; x < o; ++x) {
          out[std::size_t(c) * o * o + std::size_t{y} * o + x] = in[std::size_t(c) * t * t + std::size_t{y / 4} * t + x / 4];
        }
      }
    }
    return {};
  }
};

std::vector<std::uint8_t> noise_rgb(std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> v(std::size_t{w} * h * 3);
  std::uint32_t s = 99;
  for (auto& b : v) {
    s = s * 1664525u + 1013904223u;
    b = static_cast<std::uint8_t>(s >> 24);
  }
  return v;
}

}  // namespace

TEST_CASE("enhance: tiles join without a seam, at every size", "[enhance][picture]") {
  for (const auto& [w, h] : {std::pair{70u, 45u}, std::pair{32u, 32u}, std::pair{101u, 7u}}) {
    const auto rgb = noise_rgb(w, h);
    for (const std::uint32_t s : {1u, 2u, 4u}) {
      nearest4 m(32);
      picture_options o;
      o.out_scale = s;
      o.overlap = 4;
      std::vector<std::uint8_t> out;
      REQUIRE(enhance_picture(rgb, w, h, m, o, out));
      REQUIRE(out.size() == std::size_t{w} * s * h * s * 3);
      CHECK(static_cast<std::uint64_t>(m.calls) == tiles_for(w, h, 32, 4));
      bool same = true;
      for (std::uint32_t y = 0; y < h * s && same; ++y) {
        for (std::uint32_t x = 0; x < w * s && same; ++x) {
          for (int c = 0; c < 3; ++c) {
            same = same && out[(std::size_t{y} * w * s + x) * 3 + c] == rgb[(std::size_t{y / s} * w + x / s) * 3 + c];
          }
        }
      }
      INFO(w << "x" << h << " at " << s << "x");
      CHECK(same);
    }
  }
  // Strength 0 is the original, enlarged; at the same size, untouched.
  const auto rgb = noise_rgb(40, 30);
  nearest4 m(32);
  picture_options o;
  o.out_scale = 1;
  o.overlap = 4;
  o.strength = 0;
  std::vector<std::uint8_t> out;
  REQUIRE(enhance_picture(rgb, 40, 30, m, o, out));
  CHECK(out == rgb);
  // A scale the model cannot give, or a tile with no centre, is refused.
  o.out_scale = 3;
  CHECK_FALSE(enhance_picture(rgb, 40, 30, m, o, out));
  o.out_scale = 2;
  o.overlap = 16;
  CHECK_FALSE(enhance_picture(rgb, 40, 30, m, o, out));
}

TEST_CASE("enhance: the time left reads as a person would say it", "[enhance][picture]") {
  CHECK(time_left(0.01, 100).empty());  // too early to say
  CHECK(time_left(0.5, 20) == "Less than a minute left");
  CHECK(time_left(0.25, 600) == "About 30 minutes left");
  CHECK(time_left(0.1, 3600) == "About 9.0 hours left");
}

TEST_CASE("enhance: a clip's picture, and its sound too, through the runner", "[enhance][picture]") {
  fixture f;
  const fs::path clip = f.dir / "clip.mov";
  write_bytes(clip, ftyp("qt  ", "qt  "));
  write_text(f.self / "models/audio/model.json", R"json({
    "kind": "audio", "name": "Speech cleanup", "weights": ["denoise.onnx"], "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "strength", "label": "Noise reduction", "type": "slider", "min": 0, "max": 1, "default": 1}],
    "io": {"runner": "stream", "rate": 48000, "frame": 480, "state": 1440, "delay": 1440}})json");
  write_text(f.self / "models/video/model.json", R"json({
    "kind": "video", "name": "Detail", "weights": ["up.onnx"], "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}],
               "default": "2"},
              {"id": "strength", "label": "Strength", "type": "slider", "min": 0, "max": 1, "default": 1}],
    "io": {"runner": "tiles", "scale": 4, "tile": 32, "overlap": 4, "dims": ["h", "w"]}})json");
  f.restart();
  f.eng->event(kSettingsScreen, "install.audio", MV_SLOT_EVENT_CLICK, "");
  f.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] {
    return f.eng->download_status(model_kind::audio).state == dl_state::ready &&
           f.eng->download_status(model_kind::video).state == dl_state::ready;
  }));

  // Three 40x30 frames a tenth of a second apart, and 0.3 s of stereo sound.
  std::uint32_t frame_w = 40, frame_h = 30;
  bool hdr = false;
  const auto pcm = ramp(14'400, 2);
  struct written_frame {
    std::size_t bytes;
    std::uint32_t stride;
    std::int64_t pts;
    std::uint64_t sound_before;  // sound frames written before it
  };
  std::vector<written_frame> frames;
  std::vector<float> sound;
  std::uint32_t clip_w = 0, clip_h = 0, clip_rate = 0;
  std::vector<std::int64_t> sound_keep, frames_keep;
  run_port port;
  port.open_sound = [&](const std::string&, std::uint32_t, std::span<const std::int64_t> keep) -> mv::result<run_port::reader> {
    sound_keep.assign(keep.begin(), keep.end());
    run_port::reader r;
    r.channels = 2;
    r.total_frames = 14'400;
    auto at = std::make_shared<std::size_t>(0);
    r.read = [&pcm, at](std::size_t max_frames) -> mv::result<std::vector<float>> {
      const std::size_t n = std::min(max_frames * 2, pcm.size() - *at);
      std::vector<float> out(pcm.begin() + static_cast<std::ptrdiff_t>(*at),
                             pcm.begin() + static_cast<std::ptrdiff_t>(*at + n));
      *at += n;
      return out;
    };
    return r;
  };
  port.create = [](const std::string&, std::uint32_t, std::uint32_t) -> mv::result<run_port::writer> {
    return mv::err(mv::status::internal);  // a clip's job never writes a sound-only file
  };
  port.load_model = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<frame_model>> {
    return std::unique_ptr<frame_model>(new lag_model(480, 1440, 1.0f));
  };
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.open_frames = [&](const std::string&, std::span<const std::int64_t> keep) -> mv::result<run_port::picture_reader> {
    frames_keep.assign(keep.begin(), keep.end());
    run_port::picture_reader r;
    r.width = frame_w;
    r.height = frame_h;
    r.hdr = hdr;
    r.total_frames = 3;
    auto n = std::make_shared<int>(0);
    r.next = [n, &frame_w, &frame_h](std::vector<std::uint8_t>& rgb, std::int64_t& pts) -> mv::result<bool> {
      if (*n == 3) return false;
      rgb = noise_rgb(frame_w, frame_h);
      pts = 100'000LL * (*n)++;
      return true;
    };
    return r;
  };
  port.create_clip = [&](const std::string&, std::uint32_t w, std::uint32_t h, std::uint32_t rate,
                         std::uint32_t) -> mv::result<run_port::clip_writer> {
    clip_w = w;
    clip_h = h;
    clip_rate = rate;
    run_port::clip_writer cw;
    cw.frame = [&](const std::uint8_t*, std::uint32_t stride, std::int64_t pts) -> mv::expected {
      frames.push_back({std::size_t{stride} * clip_h, stride, pts, sound.size() / 2});
      return {};
    };
    cw.sound = [&](std::span<const float> v) -> mv::expected {
      sound.insert(sound.end(), v.begin(), v.end());
      return {};
    };
    cw.finish = [&]() -> mv::result<std::string> { return utf8(f.dir / "clip-enhance.mov"); };
    return cw;
  };
  port.load_tiles = [](const model_spec&, const std::string& dir) -> mv::result<std::unique_ptr<tile_model>> {
    CHECK(dir.find("models/video") != std::string::npos);
    return std::unique_ptr<tile_model>(new nearest4(32));
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));

  // The picture alone, at twice the size: the source's sound is copied.
  f.eng->sheet_opened(1, utf8(clip));
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"picture\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.has("k.video.scale"));
    CHECK_FALSE(t.node("run")->boolean("disabled").value_or(false));
  }
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(parsed(f.host.tree(kSheetScreen)).text("status") == "Saved as clip-enhance.mov.");
  CHECK(clip_w == 80u);
  CHECK(clip_h == 60u);
  CHECK(clip_rate == 0u);
  REQUIRE(frames.size() == 3u);
  CHECK(frames[2].pts == 200'000);
  CHECK(frames[0].stride == 80u * 3);
  CHECK(sound.empty());

  // Picture and sound: the sound is enhanced too and goes in ahead of each frame.
  frames.clear();
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"both\"");
  f.eng->event(kSheetScreen, "k.video.scale", MV_SLOT_EVENT_CHANGE, "\"1\"");
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(clip_w == 40u);
  CHECK(clip_rate == 48000u);
  REQUIRE(frames.size() == 3u);
  CHECK(frames[1].sound_before >= 4800u);  // 0.1 s of sound before the frame at 0.1 s
  CHECK(frames[2].sound_before >= 9600u);
  REQUIRE(sound.size() == pcm.size());
  CHECK(near(sound[777], pcm[777]));  // the stand-in model's lag is gone

  // An HDR clip and a 4K-busting size are refused, saying why.
  hdr = true;
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("HDR") != std::string::npos; }));
  hdr = false;
  frame_w = 1920;
  frame_h = 1080;
  f.eng->event(kSheetScreen, "k.video.scale", MV_SLOT_EVENT_CHANGE, "\"4\"");
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("larger than 4K") != std::string::npos; }));

  // From the Video Editor: the edit's kept ranges reach both readers, and the
  // open sheet moves to the edit at once.
  frame_w = 40;
  frame_h = 30;
  f.eng->event(kSheetScreen, "k.video.scale", MV_SLOT_EVENT_CHANGE, "\"1\"");
  const std::vector<std::int64_t> edit{0, 150'000'000, 250'000'000, 300'000'000};
  f.eng->command({utf8(clip)}, edit);
  CHECK(parsed(f.host.tree(kSheetScreen)).text("edit") ==
        "The edit from the Video Editor: 2 pieces, 0:00 in all. The new clip is the edit, enhanced; the original is "
        "not changed.");
  // The picture alone: the cut clip's own sound cannot be copied whole, so it
  // is read through the cut and written again, as it was.
  frames.clear();
  sound.clear();
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"picture\"");
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(frames_keep == edit);
  CHECK(sound_keep == edit);
  CHECK(clip_rate == 48000u);
  REQUIRE(sound.size() == pcm.size());
  CHECK(sound[777] == pcm[777]);  // not run through the sound model
  // The sound alone: its picture is written again, unchanged, at its own size.
  frames.clear();
  sound.clear();
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"sound\"");
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(clip_w == 40u);
  CHECK(frames.size() == 3u);
  REQUIRE(sound.size() == pcm.size());
  CHECK(near(sound[777], pcm[777]));
  // The context entry again: the whole clip, no edit.
  f.eng->command({utf8(clip)});
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).has("edit"));
}

// ---- the Video Editor's cut and Brighten dark scenes (2026-11-02) ------------------

TEST_CASE("enhance: the Video Editor's kept ranges, on frames and samples", "[enhance][edit]") {
  mv::addon::keep_ranges k;
  CHECK_FALSE(k.set(std::vector<std::int64_t>{0, 10, 5}));         // odd
  CHECK_FALSE(k.set(std::vector<std::int64_t>{0, 10, 5, 20}));     // overlapping
  CHECK_FALSE(k.set(std::vector<std::int64_t>{10, 10}));           // empty
  CHECK_FALSE(k.active());
  REQUIRE(k.set(std::vector<std::int64_t>{1'000'000'000, 2'000'000'000, 5'000'000'000, 5'500'000'000}));
  CHECK(k.kept_ns() == 1'500'000'000);
  CHECK_FALSE(k.program_of(999'999'999));
  CHECK(k.program_of(1'000'000'000) == 0);
  CHECK(k.program_of(1'999'999'999) == 999'999'999);
  CHECK_FALSE(k.program_of(2'000'000'000));  // out is exclusive
  CHECK(k.program_of(5'250'000'000) == 1'250'000'000);
  CHECK_FALSE(k.past_end(5'499'999'999));
  CHECK(k.past_end(5'500'000'000));

  // Sound at 1 kHz from 0 s, one channel: sample i is i / 1000 s, and holds i.
  std::vector<float> pcm(6000);
  for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(i);
  std::vector<float> out;
  // In two reads, split inside the first range: nothing is lost or repeated.
  k.keep_sound(std::span<const float>(pcm).first(1500), 0, 0, 1000, 1, out);
  k.keep_sound(std::span<const float>(pcm).subspan(1500), 1500, 0, 1000, 1, out);
  REQUIRE(out.size() == 1500u);
  CHECK(out[0] == 1000.0f);
  CHECK(out[999] == 1999.0f);
  CHECK(out[1000] == 5000.0f);
  CHECK(out[1499] == 5499.0f);

  // Sound that starts after the first range does: the gap is silence, so the
  // edit's sound still lines up with its pictures.
  REQUIRE(k.set(std::vector<std::int64_t>{0, 1'000'000'000}));
  out.clear();
  k.keep_sound(std::span<const float>(pcm).first(2000), 0, 200'000'000, 1000, 1, out);
  REQUIRE(out.size() == 1000u);
  CHECK(out[199] == 0.0f);
  CHECK(out[200] == 0.0f);  // the source's sample 0
  CHECK(out[201] == 1.0f);
  CHECK(out[999] == 799.0f);

  // Stereo, and a range edge between samples (ceil: the first sample at or after it).
  REQUIRE(k.set(std::vector<std::int64_t>{1'500'000, 3'000'000}));
  std::vector<float> st{0, 0, 1, -1, 2, -2, 3, -3, 4, -4};
  out.clear();
  k.keep_sound(st, 0, 0, 1000, 2, out);
  CHECK(out == std::vector<float>{2, -2});
}

namespace {
std::vector<std::uint8_t> flat_rgb(std::uint32_t w, std::uint32_t h, std::uint8_t v) {
  return std::vector<std::uint8_t>(std::size_t{w} * h * 3, v);
}
}  // namespace

TEST_CASE("enhance: Brighten dark scenes lifts dark ones, leaves lit ones, and follows the scenes", "[enhance][edit]") {
  // The curve: a lift of g at black, never past white, monotonic.
  CHECK(brighten_curve(0.0, 3.0) == 0.0);
  CHECK(near(brighten_curve(1.0, 3.0), 1.0));
  CHECK(std::fabs(brighten_curve(0.01, 3.0) - 0.0294) < 1e-3);
  for (double x = 0; x < 1; x += 0.01) CHECK(brighten_curve(x + 0.01, 4.0) > brighten_curve(x, 4.0));
  CHECK(brighten_of("some") == brighten_amount::some);
  CHECK(brighten_of("more") == brighten_amount::more);
  CHECK(brighten_of("bright") == brighten_amount::off);

  const std::uint32_t w = 64, h = 36;
  std::vector<std::uint8_t> out;
  {
    // Off: the frame as it was.
    brightener off(brighten_amount::off);
    const auto dark = flat_rgb(w, h, 20);
    (void)off.apply(dark, w, h, 0, out);
    CHECK(out == dark);
  }
  {
    // A lit scene (sRGB 140, about 0.26 linear): left alone.
    brightener b(brighten_amount::more);
    const auto lit = flat_rgb(w, h, 140);
    const brighten_fit fit = b.apply(lit, w, h, 0, out);
    CHECK(near(fit.gain, 1.0));
    CHECK(out == lit);
  }
  {
    // A night scene (sRGB 30, about 0.013 linear): lifted, more for "more",
    // capped, and never clipped.
    brightener some(brighten_amount::some), more(brighten_amount::more);
    const auto night = flat_rgb(w, h, 30);
    const brighten_fit fs = some.apply(night, w, h, 0, out);
    const std::uint8_t lifted_some = out[0];
    const brighten_fit fm = more.apply(night, w, h, 0, out);
    CHECK(fs.cut);  // the first frame sets the gain at once
    CHECK(near(fs.gain, 3.0));
    CHECK(near(fm.gain, 6.0));
    CHECK(lifted_some > 30);
    CHECK(out[0] > lifted_some);
    CHECK(out[0] < 255);
  }
  {
    // Within one scene the gain eases (no jump when a light passes); at a cut
    // it moves at once.
    brightener b(brighten_amount::more);
    std::vector<std::uint8_t> frame = flat_rgb(w, h, 30);
    (void)b.apply(frame, w, h, 0, out);
    // A light in a quarter of the frame: the scene is brighter, the same scene.
    for (std::uint32_t y = 0; y < h / 2; ++y) {
      for (std::uint32_t x = 0; x < w / 2; ++x) {
        for (int c = 0; c < 3; ++c) frame[(std::size_t{y} * w + x) * 3 + c] = 200;
      }
    }
    const brighten_fit eased = b.apply(frame, w, h, 33'333, out);
    CHECK_FALSE(eased.cut);
    CHECK(eased.target < 6.0);
    CHECK(eased.gain > eased.target);  // still most of the way back at the old gain
    CHECK(eased.gain < 6.0);
    // Half a second later of the same frame: most of the way there.
    const brighten_fit later = b.apply(frame, w, h, 533'333, out);
    CHECK(std::abs(std::log(later.gain / later.target)) < std::abs(std::log(eased.gain / eased.target)) * 0.5);
    // A cut to a lit scene: at once.
    const brighten_fit cut = b.apply(flat_rgb(w, h, 150), w, h, 566'666, out);
    CHECK(cut.cut);
    CHECK(near(cut.gain, 1.0));
  }
}

// ---- slice 3: a photo --------------------------------------------------------------

TEST_CASE("enhance: a photo through the sheet and the runner, within its limits", "[enhance][still]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0001.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/still/model.json", R"json({
    "kind": "still", "name": "Detail", "weights": ["up.onnx"], "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}],
               "default": "2"}],
    "io": {"runner": "tiles", "scale": 4, "tile": 32, "overlap": 4, "dims": ["h", "w"]}})json");
  // The fixture's manifest pins video/up.onnx; a still model needs its own pin.
  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pin = R"json({"url": "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx",
      "path": "still/up.onnx", "size": )json" + std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" +
                            hex_of(f.video_weights) + R"json(", "licence": "BSD-3-Clause"},)json";
    m.insert(m.find("[") + 1, pin);
    write_text(f.self / "manifest.json", m);
  }
  f.restart();
  f.eng->event(kSettingsScreen, "install.still", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::still).state == dl_state::ready; }));

  std::uint32_t w = 40, h = 30;
  std::uint64_t free = 8ull << 30;
  std::uint32_t written_w = 0, written_h = 0;
  run_port port;
  port.free_memory = [&]() -> std::optional<std::uint64_t> { return free; };
  port.load_tiles = [](const model_spec&, const std::string& dir) -> mv::result<std::unique_ptr<tile_model>> {
    CHECK(dir.find("models/still") != std::string::npos);
    return std::unique_ptr<tile_model>(new nearest4(32));
  };
  port.open_still = [&](const std::string& path) -> mv::result<run_port::still> {
    CHECK(path == utf8(photo));
    run_port::still s;
    s.width = w;
    s.height = h;
    s.rgb = noise_rgb(w, h);
    return s;
  };
  port.write_still = [&](const std::string&, const run_port::still& pic) -> mv::result<std::string> {
    written_w = pic.width;
    written_h = pic.height;
    CHECK(pic.rgb.size() == std::size_t{pic.width} * pic.height * 3);
    return utf8(f.dir / "IMG_0001-enhance.jpg");
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));

  f.eng->sheet_opened(1, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.has("k.still.scale"));
    CHECK_FALSE(t.node("run")->boolean("disabled").value_or(false));
  }
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(parsed(f.host.tree(kSheetScreen)).text("status") == "Saved as IMG_0001-enhance.jpg.");
  CHECK(written_w == 80u);
  CHECK(written_h == 60u);

  // Past 100 megapixels at this size: refused before any tile runs.
  w = 6000;
  h = 4500;
  f.eng->event(kSheetScreen, "k.still.scale", MV_SLOT_EVENT_CHANGE, "\"4\"");
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("megapixels") != std::string::npos; }));
  // Not enough memory for the picture in and out: says so.
  f.eng->event(kSheetScreen, "k.still.scale", MV_SLOT_EVENT_CHANGE, "\"1\"");
  free = 50'000'000;
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("free memory") != std::string::npos; }));
}

// Slice 7: Remove. A stand-in fills what is brushed with one grey.
struct grey_fill final : inpaint_filler {
  std::uint32_t side() const noexcept override { return 64; }
  mv::result<rgb8> fill(const rgb8& image, const mask8& mask) override {
    CHECK(image.width == 64);
    CHECK(mask.width == 64);
    CHECK(std::count(mask.a.begin(), mask.a.end(), 255) > 0);
    rgb8 out = image;
    for (std::size_t i = 0; i < mask.a.size(); ++i) {
      if (mask.a[i] == 255) out.rgb[i * 3] = out.rgb[i * 3 + 1] = out.rgb[i * 3 + 2] = 128;
    }
    return out;
  }
};

TEST_CASE("remove: offered on a brushed photo, the brushed area filled and the rest the photo's own",
          "[enhance][remove]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0003.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/remove/model.json", R"json({
    "kind": "remove", "name": "Remove", "weights": ["r.onnx"], "run_bytes": 1000, "cpu": true, "knobs": [],
    "io": {"runner": "inpaint", "side": 64}})json");
  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pin = R"json({"url": "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx",
      "path": "remove/r.onnx", "size": )json" + std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" +
                            hex_of(f.video_weights) + R"json(", "licence": "Apache-2.0"},)json";
    m.insert(m.find("[") + 1, pin);
    write_text(f.self / "manifest.json", m);
  }
  f.restart();
  // Nothing brushed: the photo's sheet has no Remove, only Extend (the same model).
  f.eng->sheet_opened(1, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    REQUIRE(t.has("photo"));
    CHECK(t.node("photo")->find("options")->a.size() == 2);  // More detail, Extend
  }

  // A dab at the centre of a 400 x 300 photo, radius 5% of 400 = 20 px.
  {
    std::lock_guard lock(f.host.m);
    f.host.brush = {0.5f, 0.5f, 0.05f};
  }
  f.eng->sheet_opened(2, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    REQUIRE(t.has("photo"));
    CHECK(t.node("photo")->find("options")->a.size() == 3);  // More detail, Remove, Extend
    CHECK(*t.node("photo")->str("value") == "remove");       // Remove… opened it
    CHECK(t.text("missing.remove").find("not installed") != std::string::npos);
  }
  f.eng->event(kSettingsScreen, "install.remove", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::remove).state == dl_state::ready; }));

  const std::uint32_t w = 400, h = 300;
  const std::vector<std::uint8_t> original = noise_rgb(w, h);
  std::vector<std::uint8_t> written;
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [](const model_spec&, const std::string& dir) -> mv::result<std::unique_ptr<inpaint_filler>> {
    CHECK(dir.find("models/remove") != std::string::npos);
    return std::unique_ptr<inpaint_filler>(new grey_fill);
  };
  port.still_mask = [&](const std::string& path) {
    CHECK(path == utf8(photo));
    std::lock_guard lock(f.host.m);
    return f.host.brush;
  };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = w;
    s.height = h;
    s.rgb = original;
    return s;
  };
  port.write_still = [&](const std::string&, const run_port::still& pic) -> mv::result<std::string> {
    CHECK(pic.width == w);
    CHECK(pic.height == h);
    written = pic.rgb;
    return utf8(f.dir / "IMG_0003-enhance.jpg");
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));
  f.eng->sheet_opened(3, utf8(photo));
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  REQUIRE(written.size() == original.size());
  // The centre is the fill; a pixel well away from the dab is the photo's own.
  CHECK(written[(150 * w + 200) * 3] == 128);
  std::size_t changed_far = 0;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - 200.0f, dy = static_cast<float>(y) + 0.5f - 150.0f;
      if (dx * dx + dy * dy < 60.0f * 60.0f) continue;  // the dab, its feather and the resize's reach
      const std::size_t i = (std::size_t{y} * w + x) * 3;
      changed_far += written[i] != original[i] || written[i + 1] != original[i + 1] || written[i + 2] != original[i + 2];
    }
  }
  CHECK(changed_far == 0);

  // The brush cleared in Edit: refused before anything runs.
  {
    std::lock_guard lock(f.host.m);
    f.host.brush.clear();
  }
  job_runner jr(port);
  job_request req;
  req.path = utf8(photo);
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::remove;
  part.spec = f.eng->spec(model_kind::remove);
  req.parts.push_back(part);
  CHECK(jr.check(req).find("Nothing is brushed") != std::string::npos);
}

// docs/plans/edit-refresh.md S5 (owner call B): the tool panel docked in
// Edit's Enhance tab. One tool, no photo choice; Remove reads the brush again
// when the workspace asks it to run; the result row names the new file beside
// the original, and Discard sends only that file to the Bin.
TEST_CASE("enhance: the docked tool panel runs one tool and offers the result", "[enhance][tool]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0007.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/remove/model.json", R"json({
    "kind": "remove", "name": "Remove", "weights": ["r.onnx"], "run_bytes": 1000, "cpu": true, "knobs": [],
    "io": {"runner": "inpaint", "side": 64}})json");
  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pin = R"json({"url": "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx",
      "path": "remove/r.onnx", "size": )json" + std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" +
                            hex_of(f.video_weights) + R"json(", "licence": "Apache-2.0"},)json";
    m.insert(m.find("[") + 1, pin);
    write_text(f.self / "manifest.json", m);
  }
  f.restart();
  f.eng->event(kSettingsScreen, "install.remove", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::remove).state == dl_state::ready; }));

  // A command before the panel is open does nothing.
  f.eng->tool_command(R"({"photo": "remove"})");
  CHECK(f.host.tree(kToolScreen).empty());

  // The workspace opens it on the photo and chooses Remove: nothing brushed
  // yet, so the panel says to brush and its Remove is greyed. No photo choice.
  f.eng->tool_opened(5, utf8(photo), 340, 2);
  f.eng->tool_command(R"({"photo": "remove"})");
  {
    parsed t(f.host.tree(kToolScreen));
    INFO(f.host.tree(kToolScreen));
    REQUIRE(t.has("tool.remove"));
    CHECK_FALSE(t.has("photo"));
    CHECK_FALSE(t.has("close"));
    CHECK(t.has("sec.remove"));
    CHECK(t.text("note").find("Brush over what to remove") != std::string::npos);
    // The card's Remove (or Return in the brush) runs it: no dead button here.
    CHECK_FALSE(t.has("run"));
    CHECK_FALSE(t.has("result"));
  }
  CHECK(f.eng->current_prefs().photo == photo_target::remove);
  // Another tool: its own root, its own run label.
  f.eng->tool_command(R"({"photo": "extend"})");
  CHECK(parsed(f.host.tree(kToolScreen)).has("tool.extend"));
  CHECK(*parsed(f.host.tree(kToolScreen)).node("run")->str("label") == "Extend");
  // Malformed commands change nothing.
  f.eng->tool_command("[\"remove\"]");
  f.eng->tool_command(R"({"photo": "sharpen"})");
  CHECK(parsed(f.host.tree(kToolScreen)).has("tool.extend"));

  const std::uint32_t w = 120, h = 90;
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    return std::unique_ptr<inpaint_filler>(new grey_fill);
  };
  port.still_mask = [&](const std::string&) {
    std::lock_guard lock(f.host.m);
    return f.host.brush;
  };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = w;
    s.height = h;
    s.rgb = noise_rgb(w, h);
    return s;
  };
  const fs::path made = f.dir / "IMG_0007-removed.jpg";
  port.write_still = [&](const std::string&, const run_port::still&) -> mv::result<std::string> {
    write_bytes(made, bytes({0xFF, 0xD8, 0xFF, 0xE0}));
    return utf8(made);
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));

  // Brushed in Edit, then Return: the host hands the dabs over and asks for a
  // run in one command. Remove reads the brush again before it runs.
  {
    std::lock_guard lock(f.host.m);
    f.host.brush = {0.5f, 0.5f, 0.05f};
  }
  f.eng->tool_command(R"({"photo": "remove", "run": true})");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kToolScreen)).has("result"); }));
  {
    parsed t(f.host.tree(kToolScreen));
    INFO(f.host.tree(kToolScreen));
    REQUIRE(t.has("result.thumb"));
    CHECK(*t.node("result.thumb")->str("path") == utf8(made));
    CHECK(t.node("result.thumb")->find("hidden") == nullptr);
    CHECK(t.text("result.title") == "Saved as IMG_0007-removed.jpg");
    CHECK(t.has("result.show"));
    CHECK(t.has("result.keep"));
    CHECK(t.has("result.discard"));
    // The card above carries Remove (it hands the strokes on screen over):
    // no second Remove in the panel, before or after a result.
    CHECK_FALSE(t.has("run"));
    CHECK_FALSE(t.has("cancel"));
  }
  // The sheet was never open: nothing posted there. The panel's row says
  // what was written; no second "Saved as" line under it.
  CHECK(f.host.tree(kSheetScreen).empty());
  CHECK_FALSE(parsed(f.host.tree(kToolScreen)).has("status"));
  // A sheet opened now on the same photo still says where the copy went.
  f.eng->sheet_opened(6, utf8(photo), 420, 2);
  CHECK(parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as IMG_0007-removed.jpg") != std::string::npos);
  f.eng->closed(kSheetScreen);

  // Show and Keep editing are the host's: the add-on leaves the row alone.
  f.eng->event(kToolScreen, "result.show", MV_SLOT_EVENT_CLICK, "");
  CHECK(parsed(f.host.tree(kToolScreen)).has("result"));

  // A place with no Bin: nothing is removed and the row stays.
  f.host.recycle_answer = 1;
  f.eng->event(kToolScreen, "result.discard", MV_SLOT_EVENT_CLICK, "");
  CHECK(parsed(f.host.tree(kToolScreen)).has("result"));
  CHECK(parsed(f.host.tree(kToolScreen)).text("result.note").find("was kept") != std::string::npos);
  // Discard: the new file only, never the original.
  f.host.recycle_answer = 0;
  f.eng->event(kToolScreen, "result.discard", MV_SLOT_EVENT_CLICK, "");
  {
    std::lock_guard lock(f.host.m);
    REQUIRE(f.host.recycled.size() == 2);
    CHECK(f.host.recycled[0] == utf8(made));
    CHECK(f.host.recycled[1] == utf8(made));
  }
  {
    parsed t(f.host.tree(kToolScreen));
    CHECK_FALSE(t.has("result"));
    CHECK(t.text("status").find("Moved IMG_0007-removed.jpg to") != std::string::npos);
  }
  // A second Discard has nothing to send.
  f.eng->event(kToolScreen, "result.discard", MV_SLOT_EVENT_CLICK, "");
  {
    std::lock_guard lock(f.host.m);
    CHECK(f.host.recycled.size() == 2);
  }

  // Closing the panel forgets it: later commands post nothing.
  f.eng->closed(kToolScreen);
  {
    std::lock_guard lock(f.host.m);
    f.host.trees.erase(kToolScreen);
  }
  f.eng->tool_command(R"({"photo": "detail"})");
  CHECK(f.host.tree(kToolScreen).empty());
}

#ifdef MV_ENHANCE_FACE_SPEC
// The S5 + S10 merge: Restore faces runs docked too, under the Faces card
// (root "tool.faces"), with its own run label; without the face model's spec
// the panel falls back to Enhance image, as the sheet does.
TEST_CASE("enhance: Restore faces docks as the \"faces\" tool", "[enhance][tool][faces]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0009.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  f.eng->tool_opened(7, utf8(photo), 340, 2);
  f.eng->tool_command(R"({"photo": "faces"})");
  CHECK(parsed(f.host.tree(kToolScreen)).has("tool.detail"));  // no face model here

  const auto spec = mv::test::read_bytes(MV_ENHANCE_FACE_SPEC);
  write_text(f.self / "models/face/model.json", std::string(spec.begin(), spec.end()));
  f.restart();
  f.eng->tool_opened(8, utf8(photo), 340, 2);
  f.eng->tool_command(R"({"photo": "faces"})");
  parsed t(f.host.tree(kToolScreen));
  INFO(f.host.tree(kToolScreen));
  REQUIRE(t.has("tool.faces"));
  CHECK(t.has("sec.face"));
  CHECK_FALSE(t.has("photo"));
  CHECK(*t.node("run")->str("label") == "Restore faces");
  CHECK(f.eng->current_prefs().photo == photo_target::faces);
}
#endif

TEST_CASE("enhance: a library photo's result row has no Discard and no visible tile", "[enhance][tool]") {
  sheet_view v;
  v.item = item_kind::still;
  v.photo = photo_target::detail;
  v.library = true;
  v.result_path = "photos:ABC-123";
  const std::string tree = tool_tree(v);
  parsed t(tree);
  INFO(tree);
  REQUIRE(t.has("tool.detail"));
  REQUIRE(t.has("result.thumb"));
  CHECK(*t.node("result.thumb")->str("path") == "photos:ABC-123");
  CHECK(t.node("result.thumb")->find("hidden") != nullptr);
  CHECK(t.text("result.title") == "Added to your Photos library");
  CHECK(t.has("result.show"));
  CHECK_FALSE(t.has("result.discard"));
  // A tool other than Remove offers another go with other values.
  REQUIRE(t.has("run"));
  CHECK(*t.node("run")->str("label") == "Enhance again");
  // Its key is never shown as text (rule 6).
  CHECK(tree.find("\"text\":\"photos:") == std::string::npos);
  // The tree is one the hosts accept.
  mv::addon::screen::tree st;
  CHECK(st.set(tree) == mv::addon::screen::error::none);
}

TEST_CASE("extend: new edges filled tile by tile, the photo's own pixels kept", "[enhance][extend]") {
  // The planner: Wider adds an eighth each side; tiles cover every margin pixel.
  const extend_margins m = extend_preset("wider", 400, 300);
  CHECK(m.left == 50);
  CHECK(m.right == 50);
  CHECK(m.top == 0);
  CHECK(extend_preset("square", 400, 300).top + extend_preset("square", 400, 300).bottom == 100);
  CHECK(extend_preset("16x9", 1600, 900).empty());  // already 16:9
  rgb8 photo;
  photo.width = 400;
  photo.height = 300;
  photo.rgb = noise_rgb(400, 300);
  rgb8 out;
  mask8 mask;
  extend_canvas(photo, m, out, mask);
  CHECK(out.width == 500);
  CHECK(out.height == 300);
  std::vector<std::uint8_t> covered(std::size_t{out.width} * out.height, 0);
  for (const pixel_box& b : extend_tiles(out.width, out.height, m)) {
    CHECK(b.x + b.width <= out.width);
    CHECK(b.y + b.height <= out.height);
    for (std::uint32_t y = b.y; y < b.y + b.height; ++y) {
      for (std::uint32_t x = b.x; x < b.x + b.width; ++x) covered[std::size_t{y} * out.width + x] = 1;
    }
  }
  for (std::size_t i = 0; i < covered.size(); ++i) {
    if (mask.a[i] > 127) REQUIRE(covered[i] == 1);
  }

  // The run, with the stand-in that fills with grey.
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    return std::unique_ptr<inpaint_filler>(new grey_fill);
  };
  port.still_mask = [](const std::string&) { return std::vector<float>{}; };  // nothing brushed: fine for Extend
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still st;
    st.width = photo.width;
    st.height = photo.height;
    st.rgb = photo.rgb;
    return st;
  };
  run_port::still written;
  port.write_still = [&](const std::string&, const run_port::still& pic) -> mv::result<std::string> {
    written = pic;
    return std::string("/tmp/photo-enhance.jpg");
  };
  model_spec spec;
  spec.kind = model_kind::remove;
  job_runner jr(port);
  job_request req;
  req.path = "photo.jpg";
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::remove;
  part.spec = &spec;
  part.values["mode"] = *mv::json::parse("\"extend\"");
  part.values["extend"] = *mv::json::parse("\"wider\"");
  req.parts.push_back(part);
  CHECK(jr.check(req).empty());  // no brush needed
  std::atomic<bool> done{false};
  bool ok = false;
  job_events ev;
  ev.done = [&](bool k, const std::string&, const std::string&) {
    ok = k;
    done = true;
  };
  jr.start(req, ev);
  REQUIRE(eventually([&] { return done.load(); }));
  REQUIRE(ok);
  CHECK(written.width == 500);
  CHECK(written.height == 300);
  // The photo's middle is its own; the new edge is the fill.
  std::size_t changed = 0;
  for (std::uint32_t y = 0; y < 300; ++y) {
    for (std::uint32_t x = 8; x < 392; ++x) {
      const std::size_t a = (std::size_t{y} * 400 + x) * 3, b = (std::size_t{y} * 500 + x + 50) * 3;
      changed += photo.rgb[a] != written.rgb[b];
    }
  }
  CHECK(changed == 0);
  CHECK(written.rgb[(std::size_t{150} * 500 + 10) * 3] == 128);
}

// ---- the live preview and its glow (contribution API 10) ------------------------------

TEST_CASE("enhance: the preview is the frame split, letterboxed to the sheet", "[enhance][preview]") {
  // A 400 x 100 picture, red; its enhanced twin, blue, twice the size.
  preview_image before = shrink(std::vector<std::uint8_t>(400 * 100 * 3, 0), 400, 100, 200);
  CHECK(before.width == 200u);
  CHECK(before.height == 50u);
  for (std::size_t i = 0; i < before.rgb.size(); i += 3) before.rgb[i] = 220;
  preview_image after;
  after.width = 400;
  after.height = 100;
  after.rgb.assign(400 * 100 * 3, 0);
  for (std::size_t i = 2; i < after.rgb.size(); i += 3) after.rgb[i] = 220;
  const auto px = compose_preview(before, after, 840, 440);
  REQUIRE(px.size() == 840u * 440u * 4);
  const auto at = [&](std::uint32_t x, std::uint32_t y) { return &px[(std::size_t{y} * 840 + x) * 4]; };
  // Letterboxed: 840 x 210 in the middle, the ground above and below.
  CHECK(at(420, 10)[2] < 40);
  CHECK(at(420, 10)[3] == 255);
  CHECK(at(100, 220)[2] == 220);  // left: the original (BGRA: R at 2)
  CHECK(at(740, 220)[0] == 220);  // right: the enhanced picture (B at 0)
  CHECK(at(420, 220)[1] > 150);   // the divider, light
  // One side alone fills the picture.
  const auto solo = compose_preview(before, {}, 840, 440);
  CHECK(solo[(std::size_t{220} * 840 + 740) * 4 + 2] == 220);
  // Shrinking leaves what already fits alone.
  CHECK(shrink(before.rgb, 200, 50, 960).width == 200u);
}

TEST_CASE("enhance: a clip's run shows a glowing preview, held still once it ends", "[enhance][preview]") {
  fixture f;
  const fs::path clip = f.dir / "clip.mov";
  write_bytes(clip, ftyp("qt  ", "qt  "));
  write_text(f.self / "models/video/model.json", R"json({
    "kind": "video", "name": "Detail", "weights": ["up.onnx"], "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}], "default": "2"}],
    "io": {"runner": "tiles", "scale": 4, "tile": 32, "overlap": 4, "dims": ["h", "w"]}})json");
  f.restart();
  f.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::video).state == dl_state::ready; }));
  f.host.facts = {true, false};  // a picture, no sound
  // The frames wait for the test, so the tree can be read mid-run.
  std::atomic<int> allowed{1};
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.open_frames = [&](const std::string&, std::span<const std::int64_t>) -> mv::result<run_port::picture_reader> {
    run_port::picture_reader r;
    r.width = 40;
    r.height = 30;
    r.total_frames = 3;
    auto n = std::make_shared<int>(0);
    r.next = [n, &allowed](std::vector<std::uint8_t>& rgb, std::int64_t& pts) -> mv::result<bool> {
      if (*n == 3) return false;
      for (int i = 0; i < 2500 && *n >= allowed.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      rgb = noise_rgb(40, 30);
      pts = 100'000LL * (*n)++;
      return true;
    };
    return r;
  };
  port.create_clip = [&](const std::string&, std::uint32_t, std::uint32_t, std::uint32_t,
                         std::uint32_t) -> mv::result<run_port::clip_writer> {
    run_port::clip_writer cw;
    cw.frame = [](const std::uint8_t*, std::uint32_t, std::int64_t) -> mv::expected { return {}; };
    cw.sound = [](std::span<const float>) -> mv::expected { return {}; };
    cw.finish = [&]() -> mv::result<std::string> { return utf8(f.dir / "clip-enhance.mov"); };
    return cw;
  };
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest4(32));
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));
  f.eng->sheet_opened(1, utf8(clip), 430, 2);
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).has("preview"));  // nothing to show before a run

  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).has("preview"); }));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(*t.node("preview")->str("glow") == "moving");
    CHECK(*t.node("progress")->str("look") == "glow");
    CHECK(t.node("preview")->find("marks")->a.size() == 2u);
  }
  {
    std::lock_guard lock(f.host.m);
    CHECK(f.host.surface_w == 860u);  // the sheet's width at its backing scale
    CHECK(f.host.surface_h == kPreviewHeight * 2);
  }
  allowed = 3;
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(*t.node("preview")->str("glow") == "still");  // the last frame stays, still
    CHECK_FALSE(t.has("progress"));
  }
  // Another item: no preview from a run that was not its own.
  f.eng->sheet_opened(2, utf8(f.dir / "other.mov"), 430, 2);
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).has("preview"));
}

// ---- Restyle (docs/plans/edit-adjust.md slice 5b) ----------------------------------

TEST_CASE("restyle: the Euler-ancestral schedule matches the reference pipeline's", "[enhance][restyle]") {
  // diffusers 0.42 EulerAncestralDiscreteScheduler, InstructPix2Pix's config, 20 steps.
  const euler_schedule s = make_euler_schedule(20);
  REQUIRE(s.timesteps.size() == 20);
  REQUIRE(s.sigmas.size() == 21);
  CHECK(s.init == Approx(14.6146f).epsilon(1e-4));
  CHECK(s.timesteps[0] == 999.0f);
  CHECK(s.timesteps[1] == Approx(946.421f).epsilon(1e-5));
  CHECK(s.timesteps[3] == Approx(841.263f).epsilon(1e-5));
  CHECK(s.timesteps[19] == 0.0f);
  CHECK(s.sigmas[1] == Approx(10.7469f).epsilon(1e-4));
  CHECK(s.sigmas[3] == Approx(6.205f).epsilon(1e-3));
  CHECK(s.sigmas[17] == Approx(0.35555f).epsilon(1e-3));
  CHECK(s.sigmas[19] == Approx(0.02917f).epsilon(2e-3));
  CHECK(s.sigmas[20] == 0.0f);
  CHECK(make_euler_schedule(0).sigmas.empty());
}

TEST_CASE("restyle: sizes on eights, and a change carried back keeps the photo's detail", "[enhance][restyle]") {
  std::uint32_t w = 0, h = 0;
  restyle_size(4032, 3024, 512, w, h);
  CHECK(w == 512);
  CHECK(h == 384);
  restyle_size(1000, 333, 512, w, h);
  CHECK(w % 64 == 0);
  CHECK(h % 64 == 0);
  restyle_size(1600, 900, 512, w, h);
  CHECK(w == 512);
  CHECK(h == 320);  // 288 would not halve three times in the latent
  // A full photo with fine detail (a checker), its small copy, and an "edit"
  // that warmed the small copy: the result is warmer, and the checker is
  // still there at full size.
  rgb8 full;
  full.width = 256;
  full.height = 128;
  full.rgb.resize(256 * 128 * 3);
  for (std::uint32_t y = 0; y < 128; ++y) {
    for (std::uint32_t x = 0; x < 256; ++x) {
      const std::uint8_t v = ((x / 2 + y / 2) % 2) ? 160 : 100;
      std::uint8_t* p = &full.rgb[(std::size_t{y} * 256 + x) * 3];
      p[0] = p[1] = p[2] = v;
    }
  }
  const rgb8 before = resize_rgb(full, 64, 32);
  rgb8 after = before;
  for (std::size_t i = 0; i < after.rgb.size(); i += 3) {
    after.rgb[i] = static_cast<std::uint8_t>(std::min(255, after.rgb[i] + 40));
    after.rgb[i + 2] = static_cast<std::uint8_t>(std::max(0, after.rgb[i + 2] - 40));
  }
  const rgb8 out = carry_back(full, before, after, 1.0f);
  REQUIRE(out.width == 256);
  const std::uint8_t* a = &out.rgb[(std::size_t{64} * 256 + 100) * 3];
  const std::uint8_t* b = &out.rgb[(std::size_t{64} * 256 + 102) * 3];
  CHECK(a[0] > a[2]);                    // warmer
  CHECK(std::abs(int(a[1]) - int(b[1])) > 30);  // the checker survived
  // Strength 0 is the photo itself.
  CHECK(carry_back(full, before, after, 0.0f).rgb == full.rgb);
}

// Hidden: the exported model end to end (MV_RESTYLE_DIR: the ONNX files,
// MV_ENHANCE_RUNTIME_DIR: libonnxruntime, MV_RESTYLE_PHOTO: a JPEG or PNG
// decoded by the test's caller into MV_RESTYLE_RGB, a raw 8-bit RGB file of
// MV_RESTYLE_W x MV_RESTYLE_H).
TEST_CASE("restyle: the quality picks its size and the memory it needs", "[enhance][restyle]") {
  const auto spec = parse_spec(R"json({
    "kind": "restyle", "name": "Restyle", "weights": ["r.onnx"], "run_bytes": 8000, "cpu": false,
    "knobs": [{"id": "prompt", "label": "Change", "type": "text", "default": ""},
              {"id": "quality", "label": "Quality", "type": "choice", "default": "standard",
               "options": [{"id": "standard", "text": "Standard"}, {"id": "best", "text": "Best"},
                           {"id": "odd", "text": "Odd"}]}],
    "io": {"runner": "diffusion", "edge": 512, "steps": 20,
           "qualities": {"standard": {"edge": 512, "run_bytes": 8000}, "best": {"edge": 1024, "run_bytes": 24000},
                         "odd": {"edge": 700}}}})json",
                               model_kind::restyle);
  REQUIRE(spec);
  std::uint64_t free = 10000;
  run_port port;
  port.open_still = [](const std::string&) -> mv::result<run_port::still> { return mv::err(mv::status::io); };
  port.write_still = [](const std::string&, const run_port::still&) -> mv::result<std::string> { return std::string(); };
  port.load_restyle = [](const model_spec&, const std::string&, int) -> mv::result<std::unique_ptr<restyle_model>> {
    return mv::err(mv::status::unsupported_format);
  };
  port.free_memory = [&]() -> std::optional<std::uint64_t> { return free; };
  job_runner jr(port);
  job_request req;
  req.path = "IMG_0002.JPG";
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::restyle;
  part.spec = &*spec;
  part.values["prompt"] = *mv::json::parse("\"make it winter\"");
  part.values["quality"] = *mv::json::parse("\"standard\"");
  req.parts.push_back(part);
  CHECK(jr.check(req).empty());
  // The performance level bounds the quality (tier.h): Default stops at High
  // and says which level Best needs; Potato keeps Standard.
  req.parts[0].values["quality"] = *mv::json::parse("\"best\"");
  req.policy = mv::enhance::policy_of(mv::enhance::tier::standard);
  CHECK(jr.check(req).find("Maxxed performance level") != std::string::npos);
  req.policy = mv::enhance::policy_of(mv::enhance::tier::maxxed);
  CHECK(mv::enhance::policy_of(mv::enhance::tier::potato).max_quality == 0);
  CHECK(mv::enhance::policy_of(mv::enhance::tier::potato).restyle_steps <
        mv::enhance::policy_of(mv::enhance::tier::maxxed).restyle_steps);
  // Best needs its own memory, and says how to get under it.
  const std::string refused = jr.check(req);
  CHECK(refused.find("at this quality") != std::string::npos);
  CHECK(refused.find("lower quality") != std::string::npos);
  free = 30000;
  CHECK(jr.check(req).empty());
  // A quality with no memory of its own falls back to the spec's run_bytes.
  free = 9000;
  req.parts[0].values["quality"] = *mv::json::parse("\"odd\"");
  CHECK(jr.check(req).empty());
}

// The model.json Enhance ships parses, and every quality it offers has an
// edge the UNet can take and memory of its own.
TEST_CASE("enhance: the performance level is chosen in Settings and kept", "[enhance][tier]") {
  fixture f;
  f.eng->settings_opened(1);
  {
    parsed t(f.host.tree(kSettingsScreen));
    REQUIRE(t.has("tier"));
    CHECK(*t.node("tier")->str("value") == "auto");
    CHECK(t.node("tier")->find("options")->a.size() == 5);  // Automatic, Potato, Intel, Default, Maxxed
  }
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"potato\"");
  CHECK(f.eng->effective_tier() == mv::enhance::tier::potato);
  CHECK(*parsed(f.host.tree(kSettingsScreen)).node("tier")->str("value") == "potato");
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"fast\"");  // not a level: ignored
  CHECK(f.eng->effective_tier() == mv::enhance::tier::potato);
  f.restart();
  CHECK(f.eng->effective_tier() == mv::enhance::tier::potato);  // kept in enhance.json
  // The recommendation, by Local search's rule.
  constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;
  CHECK(mv::enhance::recommend_tier(8, 8 * GiB, true) == mv::enhance::tier::potato);
  CHECK(mv::enhance::recommend_tier(8, 16 * GiB, false) == mv::enhance::tier::intel);
  CHECK(mv::enhance::recommend_tier(10, 24 * GiB, true) == mv::enhance::tier::standard);
  CHECK(mv::enhance::recommend_tier(12, 32 * GiB, true) == mv::enhance::tier::maxxed);
}

// docs/plans/edit-refresh.md S9: quality over speed by level, the level's
// chip text, and Restyle's qualities above the level shown disabled with the
// level they need.
TEST_CASE("enhance: each level's Restyle steps, its best quality, and the qualities it gates", "[enhance][tier]") {
  using mv::enhance::tier;
  // Steps by level (owner 2026-11-04): Potato 20, Intel 25, Default 40, Maxxed 50.
  CHECK(policy_of(tier::potato).restyle_steps == 20);
  CHECK(policy_of(tier::intel).restyle_steps == 25);
  CHECK(policy_of(tier::standard).restyle_steps == 40);
  CHECK(policy_of(tier::maxxed).restyle_steps == 50);
  CHECK(policy_of(tier::potato).max_quality == 0);
  CHECK(policy_of(tier::intel).max_quality == 0);
  CHECK(policy_of(tier::standard).max_quality == 1);
  CHECK(policy_of(tier::maxxed).max_quality == 2);
  CHECK(restyle_params{}.text_guidance == 7.5f);  // the words' guidance does not move with the level
  CHECK(job_request{}.policy.restyle_steps == 40);  // a request with no level runs as Default
  CHECK(tier_for_quality(quality_rank("standard")) == tier::potato);
  CHECK(tier_for_quality(quality_rank("high")) == tier::standard);
  CHECK(tier_for_quality(quality_rank("best")) == tier::maxxed);
  CHECK(level_words(tier::standard) == "Default level");

  // The quality a section starts at: the best the level allows, unless the
  // person chose one it allows.
  const auto text = mv::test::read_bytes(MV_ENHANCE_RESTYLE_SPEC);
  const auto spec = parse_spec(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()),
                               model_kind::restyle);
  REQUIRE(spec);
  const auto start = [&](tier t, const char* chosen) {
    std::map<std::string, mv::json::value> values;
    if (chosen) values["quality"] = *mv::json::parse(std::string("\"") + chosen + "\"");
    level_quality(values, *spec, t);
    return values.at("quality").s;
  };
  CHECK(start(tier::potato, nullptr) == "standard");
  CHECK(start(tier::intel, nullptr) == "standard");
  CHECK(start(tier::standard, nullptr) == "high");  // Default -> High
  CHECK(start(tier::maxxed, nullptr) == "best");
  CHECK(start(tier::maxxed, "standard") == "standard");  // a choice the level allows is kept
  CHECK(start(tier::standard, "best") == "high");        // one above it is the level's best
  CHECK(start(tier::potato, "high") == "standard");
  CHECK(start(tier::standard, "nonsense") == "high");
  {
    std::map<std::string, mv::json::value> values;  // another kind is left alone
    model_spec still = *spec;
    still.kind = model_kind::still;
    level_quality(values, still, tier::maxxed);
    CHECK(values.empty());
  }

  // The sheet: options above the level are there, disabled, with the level they need.
  const auto options = [&](tier t) {
    sheet_view v;
    v.item = item_kind::still;
    v.offers_restyle = true;
    v.photo = photo_target::restyle;
    v.level = t;
    sheet_section sec;
    sec.kind = model_kind::restyle;
    sec.spec = &*spec;
    level_quality(sec.values, *spec, t);
    v.sections.push_back(sec);
    parsed tree(sheet_tree(v));
    const mv::json::value* q = tree.node("k.restyle.quality");
    REQUIRE(q);
    std::vector<std::string> out;
    for (const mv::json::value& o : q->find("options")->a) {
      const mv::json::value* d = o.find("disabled");
      out.push_back(*o.str("text") + (d && d->b ? " [off]" : ""));
    }
    out.push_back("= " + *q->str("value"));
    return out;
  };
  CHECK(options(tier::potato) == std::vector<std::string>{"Standard", "High (needs Default level) [off]",
                                                          "Best (needs Maxxed level) [off]", "= standard"});
  CHECK(options(tier::intel) == options(tier::potato));
  CHECK(options(tier::standard) ==
        std::vector<std::string>{"Standard", "High", "Best (needs Maxxed level) [off]", "= high"});
  CHECK(options(tier::maxxed) == std::vector<std::string>{"Standard", "High", "Best", "= best"});

  // Settings names the level in effect, as the Edit pane's chip shows it.
  tier_view tv;
  tv.recommended = tier::maxxed;
  CHECK(parsed(settings_tree({}, tv)).text("tierlevel") == "Maxxed level");  // Automatic
  tv.chosen = "potato";
  CHECK(parsed(settings_tree({}, tv)).text("tierlevel") == "Potato level");
  tv.chosen = "default";
  CHECK(parsed(settings_tree({}, tv)).text("tierlevel") == "Default level");
}

TEST_CASE("enhance: the sheet's Restyle quality follows the level", "[enhance][tier]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0003.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/restyle/model.json", R"json({
    "kind": "restyle", "name": "Restyle", "weights": ["r.onnx"], "run_bytes": 1000, "cpu": false,
    "knobs": [{"id": "prompt", "label": "Change", "type": "text", "default": ""},
              {"id": "quality", "label": "Quality", "type": "choice", "default": "standard",
               "options": [{"id": "standard", "text": "Standard"}, {"id": "high", "text": "High"},
                           {"id": "best", "text": "Best"}]}],
    "io": {"runner": "diffusion", "edge": 512, "steps": 20}})json");
  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pin = R"json({"url": "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx",
      "path": "restyle/r.onnx", "size": )json" + std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" +
                            hex_of(f.video_weights) + R"json(", "licence": "CreativeML OpenRAIL-M"},)json";
    m.insert(m.find("[") + 1, pin);
    write_text(f.self / "manifest.json", m);
  }
  f.restart();
  f.eng->event(kSettingsScreen, "install.restyle", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::restyle).state == dl_state::ready; }));
  f.eng->settings_opened(1);
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"potato\"");
  CHECK(parsed(f.host.tree(kSettingsScreen)).text("tierlevel") == "Potato level");
  f.eng->sheet_opened(2, utf8(photo));
  f.eng->event(kSheetScreen, "photo", MV_SLOT_EVENT_CHANGE, "\"restyle\"");
  const auto quality = [&] {
    const parsed t(f.host.tree(kSheetScreen));
    const mv::json::value* q = t.node("k.restyle.quality");
    return q ? *q->str("value") : std::string();
  };
  CHECK(quality() == "standard");  // Potato shows Restyle with Standard only
  // Best above the level is not kept.
  f.eng->event(kSheetScreen, "k.restyle.quality", MV_SLOT_EVENT_CHANGE, "\"best\"");
  CHECK(f.eng->current_prefs().of(model_kind::restyle).count("quality") == 0);
  CHECK(quality() == "standard");
  // Default starts at High; a quality the person chose is theirs while the level allows it.
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"default\"");
  f.eng->sheet_opened(3, utf8(photo));
  CHECK(quality() == "high");
  f.eng->event(kSheetScreen, "k.restyle.quality", MV_SLOT_EVENT_CHANGE, "\"standard\"");
  f.eng->sheet_opened(4, utf8(photo));
  CHECK(quality() == "standard");
  f.eng->event(kSheetScreen, "k.restyle.quality", MV_SLOT_EVENT_CHANGE, "\"best\"");
  CHECK(f.eng->current_prefs().of(model_kind::restyle).at("quality").s == "standard");
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"maxxed\"");
  f.eng->event(kSheetScreen, "k.restyle.quality", MV_SLOT_EVENT_CHANGE, "\"best\"");
  f.eng->sheet_opened(5, utf8(photo));
  CHECK(quality() == "best");
  // Back down to Potato: the kept Best shows as the level's best, and stays kept.
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"potato\"");
  f.eng->sheet_opened(6, utf8(photo));
  CHECK(quality() == "standard");
  CHECK(f.eng->current_prefs().of(model_kind::restyle).at("quality").s == "best");
  // The level changed in Settings while the sheet is open: the sheet follows
  // at once, its options and the quality it shows (and runs).
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"maxxed\"");
  CHECK(quality() == "best");
  f.eng->event(kSettingsScreen, "tier", MV_SLOT_EVENT_CHANGE, "\"intel\"");
  CHECK(quality() == "standard");
  {
    const parsed t(f.host.tree(kSheetScreen));
    const mv::json::value* best = nullptr;
    for (const mv::json::value& o : t.node("k.restyle.quality")->find("options")->a) {
      if (*o.str("id") == "best") best = &o;
    }
    REQUIRE(best);
    REQUIRE(best->find("disabled"));
    CHECK(best->find("disabled")->b);
  }
}

TEST_CASE("restyle: the shipped model's qualities", "[enhance][restyle]") {
  const auto text = mv::test::read_bytes(MV_ENHANCE_RESTYLE_SPEC);
  const auto spec = parse_spec(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()),
                               model_kind::restyle);
  REQUIRE(spec);
  const knob* q = spec->find("quality");
  REQUIRE(q);
  CHECK(q->def_choice == "standard");
  const mv::json::value* qualities = spec->io.find("qualities");
  REQUIRE(qualities);
  std::int64_t last = 0;
  for (const knob_option& o : q->options) {
    const mv::json::value* one = qualities->find(o.id);
    REQUIRE(one);
    const auto edge = one->integer("edge");
    REQUIRE(edge);
    CHECK(*edge % 64 == 0);
    CHECK(*edge > last);
    last = *edge;
    CHECK(one->integer("run_bytes").value_or(0) > 0);
  }
}

TEST_CASE("remove: the box around a stroke, the crop, and only brushed pixels pasted back", "[enhance][remove]") {
  mask8 mask;
  mask.width = 1200;
  mask.height = 800;
  mask.a.assign(std::size_t{mask.width} * mask.height, 0);
  CHECK_FALSE(remove_box(mask));
  // A 40 x 20 stroke at (600, 400).
  for (std::uint32_t y = 400; y < 420; ++y) {
    for (std::uint32_t x = 600; x < 640; ++x) mask.a[std::size_t{y} * mask.width + x] = 255;
  }
  auto box = remove_box(mask);
  REQUIRE(box);
  // Context of at least kRemoveMinContext each side, square, inside the photo.
  CHECK(box->width == box->height);
  CHECK(box->x + kRemoveMinContext <= 600);
  CHECK(box->x + box->width >= 640 + kRemoveMinContext);
  CHECK(box->y + kRemoveMinContext <= 400);
  CHECK(box->y + box->height >= 420 + kRemoveMinContext);

  // At the photo's corner: shifted inside, still square where it fits.
  mask8 corner = mask;
  std::fill(corner.a.begin(), corner.a.end(), 0);
  for (std::uint32_t y = 0; y < 30; ++y) {
    for (std::uint32_t x = 1170; x < 1200; ++x) corner.a[std::size_t{y} * corner.width + x] = 255;
  }
  const auto edge = remove_box(corner);
  REQUIRE(edge);
  CHECK(edge->x + edge->width == 1200);
  CHECK(edge->y == 0);
  CHECK(edge->width == edge->height);
  // A stroke taller than the photo allows square: as tall as the photo.
  mask8 tall = mask;
  std::fill(tall.a.begin(), tall.a.end(), 0);
  for (std::uint32_t y = 10; y < 790; ++y) tall.a[std::size_t{y} * tall.width + 100] = 255;
  const auto t = remove_box(tall);
  REQUIRE(t);
  CHECK(t->y == 0);
  CHECK(t->height == 800);

  rgb8 photo;
  photo.width = mask.width;
  photo.height = mask.height;
  photo.rgb.resize(std::size_t{photo.width} * photo.height * 3);
  for (std::size_t i = 0; i < photo.rgb.size(); ++i) photo.rgb[i] = static_cast<std::uint8_t>(i * 7 % 251);
  const rgb8 original = photo;
  const rgb8 crop = crop_rgb(photo, *box);
  CHECK(crop.rgb[0] == photo.rgb[(std::size_t{box->y} * photo.width + box->x) * 3]);
  const mask8 small = crop_mask(mask, *box, 512, 512);
  CHECK(small.width == 512);
  CHECK(std::count(small.a.begin(), small.a.end(), 255) > 0);
  CHECK(std::all_of(small.a.begin(), small.a.end(), [](std::uint8_t v) { return v == 0 || v == 255; }));

  rgb8 filled = crop;
  std::fill(filled.rgb.begin(), filled.rgb.end(), std::uint8_t{200});
  const std::uint32_t feather = 4;
  paste_back(photo, *box, filled, mask, feather);
  std::size_t changed_far = 0;
  for (std::uint32_t y = 0; y < photo.height; ++y) {
    for (std::uint32_t x = 0; x < photo.width; ++x) {
      const std::size_t at = (std::size_t{y} * photo.width + x) * 3;
      const bool brushed = mask.a[std::size_t{y} * photo.width + x] > 127;
      const bool near = x + 2 * feather >= 600 && x < 640 + 2 * feather && y + 2 * feather >= 400 &&
                        y < 420 + 2 * feather;
      if (brushed) {
        CHECK(photo.rgb[at] == 200);
      } else if (!near) {
        changed_far += photo.rgb[at] != original.rgb[at] || photo.rgb[at + 1] != original.rgb[at + 1] ||
                       photo.rgb[at + 2] != original.rgb[at + 2];
      }
    }
  }
  CHECK(changed_far == 0);
  // The feather: just outside the stroke is a blend, not the fill.
  const std::size_t just_out = (std::size_t{410} * photo.width + 642) * 3;
  CHECK(photo.rgb[just_out] != 200);
  CHECK(photo.rgb[just_out] != original.rgb[just_out]);
}

namespace {

void brush_rect(mask8& m, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    for (std::uint32_t x = x0; x < x0 + w; ++x) m.a[std::size_t{y} * m.width + x] = 255;
  }
}

mask8 blank_mask(std::uint32_t w, std::uint32_t h) {
  mask8 m;
  m.width = w;
  m.height = h;
  m.a.assign(std::size_t{w} * h, 0);
  return m;
}

std::uint64_t box_area(const pixel_box& b) { return std::uint64_t{b.width} * b.height; }

bool inside(const pixel_box& b, std::uint32_t x, std::uint32_t y) {
  return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

// The grey stand-in, counting its fills in `fills`; `on_fill` runs after
// each, and `freed` is set when the runner lets it go.
struct counting_fill final : inpaint_filler {
  grey_fill grey;
  std::atomic<int>& fills;
  std::atomic<bool>& freed;
  std::function<void()> on_fill;
  counting_fill(std::atomic<int>& n, std::atomic<bool>& f, std::function<void()> hook)
      : fills(n), freed(f), on_fill(std::move(hook)) {}
  ~counting_fill() override { freed = true; }
  std::uint32_t side() const noexcept override { return grey.side(); }
  mv::result<rgb8> fill(const rgb8& image, const mask8& mask) override {
    auto out = grey.fill(image, mask);
    ++fills;
    if (on_fill) on_fill();
    return out;
  }
};

}  // namespace

TEST_CASE("remove: things brushed apart are filled in their own boxes", "[enhance][remove]") {
  CHECK(remove_boxes(blank_mask(800, 600)).boxes.empty());

  // Two far-apart strokes on a 2400 x 800 photo: two boxes, each its blob's
  // own grown box (remove_box of that blob alone), disjoint.
  mask8 two = blank_mask(2400, 800);
  brush_rect(two, 300, 400, 40, 20);
  brush_rect(two, 2000, 300, 40, 40);
  const remove_plan plan = remove_boxes(two);
  REQUIRE(plan.boxes.size() == 2);
  mask8 a = blank_mask(2400, 800), b = blank_mask(2400, 800);
  brush_rect(a, 300, 400, 40, 20);
  brush_rect(b, 2000, 300, 40, 40);
  const auto alone_a = remove_box(a), alone_b = remove_box(b);
  REQUIRE(alone_a);
  REQUIRE(alone_b);
  // Fill order is top to bottom: b's box starts higher.
  const pixel_box& pb = plan.boxes[0];
  const pixel_box& pa = plan.boxes[1];
  CHECK(box_area(pa) <= 2 * box_area(*alone_a));
  CHECK(box_area(pb) <= 2 * box_area(*alone_b));
  CHECK(inside(pa, 300, 400));
  CHECK(inside(pa, 339, 419));
  CHECK(inside(pb, 2000, 300));
  CHECK(inside(pb, 2039, 339));
  CHECK((pa.x + pa.width <= pb.x || pb.x + pb.width <= pa.x));
  // Each brushed pixel belongs to the box around it; box masks split them.
  CHECK(plan.owner_at(310, 405) == 2);
  CHECK(plan.owner_at(2010, 310) == 1);
  const mask8 own_b = box_mask(two, plan, 0, false), open_b = box_mask(two, plan, 0, true);
  const mask8 own_a = box_mask(two, plan, 1, false);
  CHECK(std::count(own_b.a.begin(), own_b.a.end(), 255) == 40 * 40);
  CHECK(std::count(open_b.a.begin(), open_b.a.end(), 255) == 40 * 40);
  CHECK(std::count(own_a.a.begin(), own_a.a.end(), 255) == 40 * 20);

  // Near neighbours (separate blobs, overlapping boxes): one box.
  mask8 near = blank_mask(2400, 800);
  brush_rect(near, 600, 400, 40, 20);
  brush_rect(near, 720, 400, 40, 20);
  const remove_plan joined = remove_boxes(near);
  REQUIRE(joined.boxes.size() == 1);
  CHECK(inside(joined.boxes[0], 600, 400));
  CHECK(inside(joined.boxes[0], 759, 419));
  const mask8 own = box_mask(near, joined, 0, false);
  CHECK(std::count(own.a.begin(), own.a.end(), 255) == 2 * 40 * 20);

  // More than kRemoveMaxBoxes far apart: one box over everything, as before.
  mask8 many = blank_mask(6000, 4000);
  for (std::uint32_t gy = 0; gy < 3; ++gy) {
    for (std::uint32_t gx = 0; gx < 3; ++gx) brush_rect(many, 500 + gx * 2400, 500 + gy * 1400, 20, 20);
  }
  const remove_plan fallback = remove_boxes(many);
  REQUIRE(fallback.boxes.size() == 1);
  const auto whole = remove_box(many);
  REQUIRE(whole);
  CHECK(fallback.boxes[0].x == whole->x);
  CHECK(fallback.boxes[0].y == whole->y);
  CHECK(fallback.boxes[0].width == whole->width);
  CHECK(fallback.boxes[0].height == whole->height);
  CHECK(fallback.owner_at(510, 510) == 1);

  // Random dabs: every brushed pixel is filled by exactly one box, the one
  // around it, and the boxes are inside the photo (a brushed pixel no box
  // owns would be left in the result).
  std::uint32_t seed = 12345;
  const auto next = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
  };
  for (int trial = 0; trial < 40; ++trial) {
    const std::uint32_t tw = 300 + static_cast<std::uint32_t>(next() * 1700.0f);
    const std::uint32_t th = 200 + static_cast<std::uint32_t>(next() * 1300.0f);
    std::vector<float> random_dabs;
    const int count = 1 + static_cast<int>(next() * 12.0f);
    for (int k = 0; k < count; ++k) {
      random_dabs.push_back(next());
      random_dabs.push_back(next());
      random_dabs.push_back(0.002f + next() * 0.02f);
    }
    const mask8 m = mask_from_dabs(random_dabs, tw, th);
    const remove_plan rp = remove_boxes(m);
    REQUIRE(!rp.boxes.empty());
    REQUIRE(rp.boxes.size() <= kRemoveMaxBoxes);
    for (const pixel_box& pb2 : rp.boxes) {
      CHECK(pb2.width > 0);
      CHECK(pb2.height > 0);
      CHECK(pb2.x + pb2.width <= tw);
      CHECK(pb2.y + pb2.height <= th);
    }
    std::size_t orphans = 0, brushed = 0, owned = 0;
    for (std::uint32_t y = 0; y < th; ++y) {
      for (std::uint32_t x = 0; x < tw; ++x) {
        if (m.a[std::size_t{y} * tw + x] <= 127) continue;
        ++brushed;
        const std::uint8_t who = rp.owner_at(x, y);
        if (who == 0 || std::size_t{who} > rp.boxes.size() || !inside(rp.boxes[who - 1u], x, y)) ++orphans;
      }
    }
    for (std::size_t k = 0; k < rp.boxes.size(); ++k) {
      const mask8 own_k = box_mask(m, rp, k, false);
      owned += static_cast<std::size_t>(std::count(own_k.a.begin(), own_k.a.end(), 255));
    }
    CHECK(orphans == 0);
    CHECK(owned == brushed);
  }

  // The run: two dabs at either end of a 1600 x 600 photo, each filled in its
  // own box (two fills); every pixel outside both boxes is the photo's own.
  const std::uint32_t w = 1600, h = 600;
  const std::vector<std::uint8_t> original = noise_rgb(w, h);
  const std::vector<float> dabs = {0.1f, 0.5f, 0.01f, 0.9f, 0.5f, 0.01f};  // radius 16 px
  const remove_plan run_plan = remove_boxes(mask_from_dabs(dabs, w, h));
  REQUIRE(run_plan.boxes.size() == 2);
  // The stand-in's side is 64, so each box (over 1.5x it) is filled again on
  // one 64-px tile at full resolution: four fills in all.
  const mask8 run_mask = mask_from_dabs(dabs, w, h);
  const std::size_t second_pass = refine_tiles(run_mask, run_plan, 0, 64).size() + refine_tiles(run_mask, run_plan, 1, 64).size();
  CHECK(second_pass == 2);
  std::atomic<int> fills{0};
  std::atomic<bool> freed{false};
  std::function<void()> after_fill;  // read when the model loads
  std::vector<std::uint8_t> written;
  std::atomic<int> writes{0};
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [&](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    return std::unique_ptr<inpaint_filler>(new counting_fill(fills, freed, after_fill));
  };
  port.still_mask = [&](const std::string&) { return dabs; };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = w;
    s.height = h;
    s.rgb = original;
    return s;
  };
  port.write_still = [&](const std::string&, const run_port::still& pic) -> mv::result<std::string> {
    ++writes;
    written = pic.rgb;
    return std::string("/tmp/photo-removed.jpg");
  };
  model_spec spec;
  spec.kind = model_kind::remove;
  job_request req;
  req.path = "photo.jpg";
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::remove;
  part.spec = &spec;
  req.parts.push_back(part);
  {
    job_runner jr(port);
    CHECK(jr.check(req).empty());
    std::atomic<bool> done{false};
    bool ok = false;
    job_events ev;
    ev.done = [&](bool k, const std::string&, const std::string&) {
      ok = k;
      done = true;
    };
    jr.start(req, ev);
    REQUIRE(eventually([&] { return done.load(); }));
    REQUIRE(ok);
  }
  CHECK(fills.load() == static_cast<int>(2 + second_pass));
  CHECK(writes.load() == 1);
  REQUIRE(written.size() == original.size());
  CHECK(written[(std::size_t{300} * w + 160) * 3] == 128);   // the first dab's centre
  CHECK(written[(std::size_t{300} * w + 1440) * 3] == 128);  // the second's
  std::size_t changed_outside = 0;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      if (inside(run_plan.boxes[0], x, y) || inside(run_plan.boxes[1], x, y)) continue;
      const std::size_t i = (std::size_t{y} * w + x) * 3;
      if (written[i] != original[i] || written[i + 1] != original[i + 1] || written[i + 2] != original[i + 2]) {
        ++changed_outside;
      }
    }
  }
  CHECK(changed_outside == 0);

  // Cancel during the first box: the second is never filled, nothing written.
  fills = 0;
  writes = 0;
  freed = false;
  {
    job_runner jr(port);
    after_fill = [&] { jr.cancel(); };  // from the job's thread, as a Cancel landing mid-fill
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    job_events ev;
    ev.done = [&](bool k, const std::string&, const std::string&) {
      ok = k;
      done = true;
    };
    jr.start(req, ev);
    REQUIRE(eventually([&] { return done.load(); }));
    CHECK_FALSE(ok.load());
    REQUIRE(eventually([&] { return freed.load(); }));  // the run has let the model go
  }
  CHECK(fills.load() == 1);
  CHECK(writes.load() == 0);
}

// ---- Quality over speed (owner 2026-11-04): Remove's second pass, Extend's tiles ----

namespace {

// Gives back the picture it was shown, its holes included (a stand-in can
// peek at what LaMa never sees), with two things added in the holes: an
// offset that alternates call by call (+16, -16: neighbouring tiles disagree
// by 32, as two real fills of the same hole disagree on shape and colour),
// and a fine checker (+-12, the grain a full-resolution fill has).
struct offset_fill final : inpaint_filler {
  std::uint32_t n;
  int calls = 0;
  double least_known = 1.0;
  explicit offset_fill(std::uint32_t side) : n(side) {}
  std::uint32_t side() const noexcept override { return n; }
  mv::result<rgb8> fill(const rgb8& image, const mask8& mask) override {
    REQUIRE(image.width == n);
    REQUIRE(mask.width == n);
    const auto known = static_cast<double>(std::count(mask.a.begin(), mask.a.end(), 0)) / static_cast<double>(mask.a.size());
    least_known = std::min(least_known, known);
    const int off = calls % 2 == 0 ? 16 : -16;
    rgb8 out = image;
    for (std::uint32_t y = 0; y < n; ++y) {
      for (std::uint32_t x = 0; x < n; ++x) {
        const std::size_t i = std::size_t{y} * n + x;
        if (mask.a[i] <= 127) continue;
        const int grain = (x + y) % 2 == 0 ? 12 : -12;
        for (int c = 0; c < 3; ++c) out.rgb[i * 3 + c] = static_cast<std::uint8_t>(std::clamp(image.rgb[i * 3 + c] + off + grain, 0, 255));
      }
    }
    ++calls;
    return out;
  }
};

rgb8 flat_picture(std::uint32_t w, std::uint32_t h, std::uint8_t v) {
  rgb8 r;
  r.width = w;
  r.height = h;
  r.rgb.assign(std::size_t{w} * h * 3, v);
  return r;
}

}  // namespace

TEST_CASE("remove: a large box is filled again at full resolution on model-side tiles", "[enhance][remove]") {
  CHECK_FALSE(needs_refine(pixel_box{0, 0, 768, 700}, 512));  // 1.5x the side: the first pass alone
  CHECK(needs_refine(pixel_box{0, 0, 769, 300}, 512));
  CHECK(remove_feather(pixel_box{0, 0, 300, 300}) == 2);
  CHECK(remove_feather(pixel_box{0, 0, 900, 2000}) == 7);
  CHECK(remove_feather(pixel_box{0, 0, 9000, 10}) == 16);

  // A small stroke: its box is under 1.5x the model's side, no second pass.
  mask8 small = blank_mask(3000, 2000);
  brush_rect(small, 1000, 800, 100, 100);
  const remove_plan small_plan = remove_boxes(small);
  REQUIRE(small_plan.boxes.size() == 1);
  CHECK(refine_tiles(small, small_plan, 0, 512).empty());

  // A 600 x 400 brush: tiles of the model's side, inside the photo, over every
  // pixel the paste can change (the brush and its feather's reach), reaching
  // a quarter tile past it for context.
  mask8 big = blank_mask(3000, 2000);
  brush_rect(big, 1000, 800, 600, 400);
  const remove_plan plan = remove_boxes(big);
  REQUIRE(plan.boxes.size() == 1);
  const pixel_box box = plan.boxes[0];
  REQUIRE(needs_refine(box, 512));
  const std::vector<pixel_box> tiles = refine_tiles(big, plan, 0, 512);
  CHECK(tiles.size() == 6);  // (600 + 256) / 256 across, (400 + 256) / 256 down, half overlapping
  std::vector<std::uint8_t> cover(std::size_t{3000} * 2000, 0);
  for (const pixel_box& t : tiles) {
    CHECK(t.width == 512);
    CHECK(t.height == 512);
    CHECK(t.x + t.width <= 3000);
    CHECK(t.y + t.height <= 2000);
    for (std::uint32_t y = t.y; y < t.y + t.height; ++y) {
      for (std::uint32_t x = t.x; x < t.x + t.width; ++x) cover[std::size_t{y} * 3000 + x] = 1;
    }
  }
  const std::uint32_t reach = 2 * remove_feather(box) + 1;
  std::size_t uncovered = 0;
  for (std::uint32_t y = 800 - reach; y < 1200 + reach; ++y) {
    for (std::uint32_t x = 1000 - reach; x < 1600 + reach; ++x) {
      if (inside(box, x, y)) uncovered += cover[std::size_t{y} * 3000 + x] == 0;
    }
  }
  CHECK(uncovered == 0);
  CHECK(cover[std::size_t{800 - 128} * 3000 + 1000 - 128] == 1);  // context past the brush

  // A strip of a photo, shorter than the model's side: tiles as tall as it.
  mask8 thin = blank_mask(4000, 300);
  brush_rect(thin, 1500, 100, 900, 100);
  const remove_plan thin_plan = remove_boxes(thin);
  REQUIRE(thin_plan.boxes.size() == 1);
  const std::vector<pixel_box> thin_tiles = refine_tiles(thin, thin_plan, 0, 512);
  REQUIRE_FALSE(thin_tiles.empty());
  for (const pixel_box& t : thin_tiles) {
    CHECK(t.width == 512);
    CHECK(t.height == 300);
    CHECK(t.x + t.width <= 4000);
  }

  // The fill keeps the first pass's shape and colour and takes the tiles'
  // detail. A photo that is a gentle ramp, a 400 x 120 stroke (an 800-px box)
  // and a stand-in at 256 px: inside the stroke the result is the first pass
  // (here the ramp itself) plus the stand-in's grain, its per-tile offsets
  // gone (they are not detail), so tiles that disagree leave no seam; just
  // outside, the photo's own.
  rgb8 photo;
  photo.width = 1200;
  photo.height = 900;
  photo.rgb.resize(std::size_t{1200} * 900 * 3);
  const auto ramp = [](std::uint32_t x, std::uint32_t y) { return static_cast<int>(40 + x / 10 + y / 10); };
  for (std::uint32_t y = 0; y < 900; ++y) {
    for (std::uint32_t x = 0; x < 1200; ++x) {
      for (int c = 0; c < 3; ++c) photo.rgb[(std::size_t{y} * 1200 + x) * 3 + c] = static_cast<std::uint8_t>(ramp(x, y));
    }
  }
  const auto first_pass = [&](const pixel_box& b) { return crop_rgb(photo, b); };
  mask8 stroke = blank_mask(1200, 900);
  brush_rect(stroke, 400, 300, 400, 120);
  const remove_plan sp = remove_boxes(stroke);
  REQUIRE(sp.boxes.size() == 1);
  const pixel_box sb = sp.boxes[0];
  const std::vector<pixel_box> st = refine_tiles(stroke, sp, 0, 256);
  REQUIRE(st.size() >= 3);
  offset_fill grainy(256);
  rgb8 filled = first_pass(sb);
  std::size_t asked = 0;
  REQUIRE(refine_fill(photo, stroke, sp, 0, st, grainy, filled, [&] { return ++asked, true; }));
  CHECK(asked == st.size());
  CHECK(grainy.calls == static_cast<int>(st.size()));
  int most_off = 0;
  for (std::uint32_t y = 332; y < 388; ++y) {
    for (std::uint32_t x = 432; x < 768; ++x) {
      const std::size_t at = (std::size_t{y - sb.y} * sb.width + (x - sb.x)) * 3;
      const int grain = (x + y) % 2 == 0 ? 12 : -12;
      most_off = std::max(most_off, std::abs(filled.rgb[at] - ramp(x, y) - grain));
    }
  }
  INFO("most off the first pass plus the grain: " << most_off);
  CHECK(most_off <= 4);
  const std::size_t ring = (std::size_t{296 - sb.y} * sb.width + (600 - sb.x)) * 3;
  CHECK(std::abs(filled.rgb[ring] - ramp(600, 296)) <= 2);

  // A hole 400 x 300 under 64-px tiles: the tiles deep in it would see
  // nothing, so they are not among the tiles (none a tenth known or less, so
  // the run's progress counts only fills), and there the first pass stays
  // exactly.
  mask8 hole = blank_mask(1200, 900);
  brush_rect(hole, 400, 300, 400, 300);
  const remove_plan hp = remove_boxes(hole);
  REQUIRE(hp.boxes.size() == 1);
  const pixel_box hb = hp.boxes[0];
  const std::vector<pixel_box> ht = refine_tiles(hole, hp, 0, 64);
  REQUIRE(ht.size() > 20);  // the ring around the hole
  offset_fill model(64);
  rgb8 deep = flat_picture(hb.width, hb.height, 200);
  REQUIRE(refine_fill(photo, hole, hp, 0, ht, model, deep, nullptr));
  CHECK(model.calls == static_cast<int>(ht.size()));
  CHECK(model.least_known > 0.1);
  for (const pixel_box& t : ht) CHECK_FALSE(inside(t, 600, 450));
  CHECK(deep.rgb[(std::size_t{450 - hb.y} * hb.width + (600 - hb.x)) * 3] == 200);

  // Cancelled before a tile: nothing more is filled.
  offset_fill again(64);
  rgb8 f2 = flat_picture(hb.width, hb.height, 200);
  int left = 3;
  const mv::expected stopped = refine_fill(photo, hole, hp, 0, ht, again, f2, [&] { return left-- > 0; });
  CHECK_FALSE(stopped);
  CHECK(again.calls <= 3);
}

TEST_CASE("remove: the second pass runs in the job with progress per tile, and cancels between tiles",
          "[enhance][remove]") {
  const std::uint32_t w = 1600, h = 600;
  const std::vector<std::uint8_t> original = noise_rgb(w, h);
  const std::vector<float> dabs = {0.1f, 0.5f, 0.01f, 0.9f, 0.5f, 0.01f};
  const mask8 mask = mask_from_dabs(dabs, w, h);
  const remove_plan plan = remove_boxes(mask);
  REQUIRE(plan.boxes.size() == 2);
  const std::size_t units = 2 + refine_tiles(mask, plan, 0, 64).size() + refine_tiles(mask, plan, 1, 64).size();
  std::atomic<int> fills{0};
  std::atomic<bool> freed{false};
  std::function<void()> after_fill;
  std::atomic<int> writes{0};
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [&](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    return std::unique_ptr<inpaint_filler>(new counting_fill(fills, freed, after_fill));
  };
  port.still_mask = [&](const std::string&) { return dabs; };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = w;
    s.height = h;
    s.rgb = original;
    return s;
  };
  port.write_still = [&](const std::string&, const run_port::still&) -> mv::result<std::string> {
    ++writes;
    return std::string("/tmp/photo-removed.jpg");
  };
  model_spec spec;
  spec.kind = model_kind::remove;
  job_request req;
  req.path = "photo.jpg";
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::remove;
  part.spec = &spec;
  req.parts.push_back(part);
  std::mutex m;
  std::vector<double> steps;
  {
    job_runner jr(port);
    std::atomic<bool> done{false};
    bool ok = false;
    job_events ev;
    ev.progress = [&](double f) {
      std::lock_guard lock(m);
      steps.push_back(f);
    };
    ev.done = [&](bool k, const std::string&, const std::string&) {
      ok = k;
      done = true;
    };
    jr.start(req, ev);
    REQUIRE(eventually([&] { return done.load(); }));
    REQUIRE(ok);
  }
  CHECK(fills.load() == static_cast<int>(units));
  {
    // One report a fill (and the start's "working" and the end's), only forward.
    std::lock_guard lock(m);
    std::vector<double> known;
    for (const double f : steps) {
      if (f >= 0) known.push_back(f);
    }
    CHECK(known.size() >= units + 1);
    CHECK(std::is_sorted(known.begin(), known.end()));
    CHECK(known.back() < 1.0);
  }

  // A cancel during the first box's second pass: no later tile or box runs,
  // nothing is written.
  fills = 0;
  writes = 0;
  freed = false;
  {
    job_runner jr(port);
    after_fill = [&] {
      if (fills.load() == 2) jr.cancel();  // the first pass, then the first tile
    };
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    job_events ev;
    ev.done = [&](bool k, const std::string&, const std::string&) {
      ok = k;
      done = true;
    };
    jr.start(req, ev);
    REQUIRE(eventually([&] { return done.load(); }));
    CHECK_FALSE(ok.load());
    REQUIRE(eventually([&] { return freed.load(); }));
  }
  CHECK(fills.load() == 2);
  CHECK(writes.load() == 0);
}

TEST_CASE("extend: tiles of 768 or more, two-thirds overlapping, every margin pixel in three", "[enhance][extend]") {
  // Every margin pixel of the extended picture in at least kExtendCover
  // tiles, each inside it: the presets on photos of many shapes, and
  // margins of any depth on any side (the host's drag, up to the photo's own
  // size).
  const auto check = [](std::uint32_t pw, std::uint32_t ph, const extend_margins& m) {
    const std::uint32_t w = pw + m.left + m.right, h = ph + m.top + m.bottom, shorter = std::min(w, h);
    const std::vector<pixel_box> tiles = extend_tiles(w, h, m);
    // Counts by a 2-D difference array.
    std::vector<std::int32_t> d(std::size_t{w + 1} * (h + 1), 0);
    std::uint32_t big_enough = 0;
    for (const pixel_box& b : tiles) {
      REQUIRE(b.width == b.height);  // square: never squashed into the model's square
      REQUIRE(b.width > 0);
      REQUIRE(b.x + b.width <= w);
      REQUIRE(b.y + b.height <= h);
      CHECK(b.width * 2 >= std::min(kExtendMinTile, shorter));
      big_enough += b.width >= std::min(kExtendMinTile, shorter) ? 1u : 0u;
      d[std::size_t{b.y} * (w + 1) + b.x] += 1;
      d[std::size_t{b.y} * (w + 1) + b.x + b.width] -= 1;
      d[std::size_t{b.y + b.height} * (w + 1) + b.x] -= 1;
      d[std::size_t{b.y + b.height} * (w + 1) + b.x + b.width] += 1;
    }
    CHECK(big_enough > 0);  // the side is the smaller ends' only where the picture is too small for larger
    for (std::uint32_t y = 0; y <= h; ++y) {
      for (std::uint32_t x = 0; x <= w; ++x) {
        std::int32_t& v = d[std::size_t{y} * (w + 1) + x];
        if (x > 0) v += d[std::size_t{y} * (w + 1) + x - 1];
        if (y > 0) v += d[std::size_t{y - 1} * (w + 1) + x];
        if (x > 0 && y > 0) v -= d[std::size_t{y - 1} * (w + 1) + x - 1];
      }
    }
    std::size_t thin = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const bool photo = x >= m.left && x < m.left + pw && y >= m.top && y < m.top + ph;
        if (!photo && d[std::size_t{y} * (w + 1) + x] < static_cast<std::int32_t>(kExtendCover)) ++thin;
      }
    }
    INFO(pw << "x" << ph << " margins " << m.left << " " << m.top << " " << m.right << " " << m.bottom);
    CHECK(thin == 0);
    return tiles;
  };
  for (const char* preset : {"wider", "taller", "all", "square", "16x9", "4x5"}) {
    for (const auto [pw, ph] : {std::pair{400u, 300u}, std::pair{1200u, 800u}, std::pair{2000u, 1500u},
                                std::pair{900u, 1600u}, std::pair{2400u, 300u}, std::pair{300u, 2000u}}) {
      const extend_margins m = extend_preset(preset, pw, ph);
      if (!m.empty()) (void)check(pw, ph, m);
    }
  }
  std::uint32_t seed = 4242;
  const auto next = [&seed](std::uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  for (int trial = 0; trial < 40; ++trial) {
    const std::uint32_t pw = 64 + next(1600), ph = 64 + next(1600);
    extend_margins m;
    m.left = next(3) == 0 ? 0 : next(pw / 2 + 1);
    m.right = next(3) == 0 ? 0 : next(pw / 2 + 1);
    m.top = next(3) == 0 ? 0 : next(ph / 2 + 1);
    m.bottom = next(3) == 0 ? 0 : next(ph / 2 + 1);
    if (!m.empty()) (void)check(pw, ph, m);
  }
  // On a 4000 x 3000 photo every tile is at least 768, and a tile overlaps
  // its neighbour along the margin by two-thirds.
  const extend_margins wide = extend_preset("wider", 4000, 3000);
  const std::vector<pixel_box> tiles = check(4000, 3000, wide);
  for (const pixel_box& b : tiles) CHECK(b.width >= kExtendMinTile);
  CHECK(tiles[1].y - tiles[0].y == tiles[0].width / 3);
}

// The real LaMa (MV_REMOVE_MODEL: lama_fp32.onnx) on a photo
// (MV_RESTYLE_RGB / _W / _H, raw RGB): a hole brushed over its middle is
// filled with something like its surroundings, and everything away from the
// brush is the photo's own. MV_REMOVE_OUT keeps the result as raw RGB.
TEST_CASE("remove: LaMa through ONNX Runtime", "[.remove-model]") {
  const char* file = std::getenv("MV_REMOVE_MODEL");
  const char* ort = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  const char* raw = std::getenv("MV_RESTYLE_RGB");
  const char* ws = std::getenv("MV_RESTYLE_W");
  const char* hs = std::getenv("MV_RESTYLE_H");
  if (!file || !ort || !raw || !ws || !hs) SKIP("MV_REMOVE_MODEL / MV_ENHANCE_RUNTIME_DIR / MV_RESTYLE_RGB not set");
  rgb8 full;
  full.width = static_cast<std::uint32_t>(std::atoi(ws));
  full.height = static_cast<std::uint32_t>(std::atoi(hs));
  full.rgb = mv::test::read_bytes(raw);
  REQUIRE(full.rgb.size() == std::size_t{full.width} * full.height * 3);
  auto rt = mv::infer::runtime::load(ort);
  REQUIRE(rt);
  auto model = inpaint_model::open(**rt, file, inpaint_io{});
  INFO(mv::infer::session::last_error());
  REQUIRE(model);
  // A stroke across the middle, radius 3% of the long side.
  std::vector<float> dabs;
  for (float u = 0.4f; u <= 0.6f; u += 0.005f) dabs.insert(dabs.end(), {u, 0.5f, 0.03f});
  const mask8 mask = mask_from_dabs(dabs, full.width, full.height);
  const auto box = remove_box(mask);
  REQUIRE(box);
  const std::uint32_t side = (*model)->side();
  const rgb8 crop = crop_rgb(full, *box);
  const auto t0 = std::chrono::steady_clock::now();
  auto filled = (*model)->fill(resize_rgb(crop, side, side), crop_mask(mask, *box, side, side));
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(filled);
  rgb8 out = full;
  paste_back(out, *box, resize_rgb(*filled, box->width, box->height), mask, 4);
  // The fill's mean against the ring just outside the brush: close, and
  // nothing like white or black.
  double in_sum = 0, ring_sum = 0;
  std::size_t in_n = 0, ring_n = 0, far_changed = 0;
  const mask8 grown = mask_from_dabs([&] {
    std::vector<float> g = dabs;
    for (std::size_t i = 2; i < g.size(); i += 3) g[i] = 0.06f;
    return g;
  }(), full.width, full.height);
  const mask8 far = mask_from_dabs([&] {
    std::vector<float> g = dabs;
    for (std::size_t i = 2; i < g.size(); i += 3) g[i] = 0.045f;
    return g;
  }(), full.width, full.height);
  for (std::size_t i = 0; i < mask.a.size(); ++i) {
    const double l = (out.rgb[i * 3] + out.rgb[i * 3 + 1] + out.rgb[i * 3 + 2]) / 3.0;
    if (mask.a[i]) {
      in_sum += l;
      ++in_n;
    } else if (grown.a[i]) {
      ring_sum += l;
      ++ring_n;
    }
    if (!far.a[i]) {
      far_changed += out.rgb[i * 3] != full.rgb[i * 3] || out.rgb[i * 3 + 1] != full.rgb[i * 3 + 1] ||
                     out.rgb[i * 3 + 2] != full.rgb[i * 3 + 2];
    }
  }
  REQUIRE(in_n > 0);
  REQUIRE(ring_n > 0);
  const double in_mean = in_sum / in_n, ring_mean = ring_sum / ring_n;
  std::printf("remove: %ux%u box filled at %u px in %.2f s on CPU; fill mean %.1f, surroundings %.1f\n", box->width,
              box->height, side, s, in_mean, ring_mean);
  CHECK(std::abs(in_mean - ring_mean) < 30.0);
  CHECK(far_changed == 0);
  if (const char* keep = std::getenv("MV_REMOVE_OUT")) write_bytes(std::string(keep), out.rgb);

  // Extend (owner 2026-11-04): the photo grown a tenth all round, the real
  // model filling the margins tile by tile; MV_EXTEND_OUT keeps it (raw RGB,
  // the size printed).
  const extend_margins em = extend_preset("all", full.width, full.height);
  rgb8 bigger;
  mask8 gmask;
  extend_canvas(full, em, bigger, gmask);
  const auto e0 = std::chrono::steady_clock::now();
  REQUIRE(extend_fill(bigger, gmask, extend_tiles(bigger.width, bigger.height, em), **model, nullptr));
  for (std::uint32_t y = 0; y < full.height; ++y) {
    std::copy_n(full.rgb.data() + std::size_t{y} * full.width * 3, std::size_t{full.width} * 3,
                bigger.rgb.data() + (std::size_t{y + em.top} * bigger.width + em.left) * 3);
  }
  std::printf("extend: %ux%u -> %ux%u in %.2f s on CPU\n", full.width, full.height, bigger.width, bigger.height,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - e0).count());
  if (const char* keep = std::getenv("MV_EXTEND_OUT")) write_bytes(std::string(keep), bigger.rgb);
}

// The quality defaults ("Quality over speed", owner 2026-11-04) timed with the
// real LaMa, through the runner as the app runs them (progress, cancel
// checks, the second pass): MV_REMOVE_MODEL, MV_ENHANCE_RUNTIME_DIR and a
// photo (MV_RESTYLE_RGB / _W / _H, raw RGB). MV_REMOVE_DABS ("u,v,r u,v,r
// ...", the host's brush) says what to take out (default: a block of dabs
// over the middle sixth of the picture each way); MV_EXTEND_PRESET the Extend (default
// "all"); MV_REMOVE_OUT_DIR keeps both results as raw RGB; MV_QUALITY_ONLY
// ("remove" or "extend") runs one.
TEST_CASE("remove and extend: the quality defaults timed with the real LaMa", "[.remove-quality]") {
  const char* file = std::getenv("MV_REMOVE_MODEL");
  const char* ort = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  const char* raw = std::getenv("MV_RESTYLE_RGB");
  const char* ws = std::getenv("MV_RESTYLE_W");
  const char* hs = std::getenv("MV_RESTYLE_H");
  if (!file || !ort || !raw || !ws || !hs) SKIP("MV_REMOVE_MODEL / MV_ENHANCE_RUNTIME_DIR / MV_RESTYLE_RGB not set");
  run_port::still photo;
  photo.width = static_cast<std::uint32_t>(std::atoi(ws));
  photo.height = static_cast<std::uint32_t>(std::atoi(hs));
  photo.rgb = mv::test::read_bytes(raw);
  REQUIRE(photo.rgb.size() == std::size_t{photo.width} * photo.height * 3);
  auto rt = mv::infer::runtime::load(ort);
  REQUIRE(rt);
  std::vector<float> dabs;
  if (const char* d = std::getenv("MV_REMOVE_DABS")) {
    std::string s(d);
    for (char& c : s) c = c == ',' ? ' ' : c;
    std::istringstream in(s);
    for (float v; in >> v;) dabs.push_back(v);
  } else {
    for (float v = 0.42f; v <= 0.58f; v += 0.01f) {
      for (float u = 0.42f; u <= 0.58f; u += 0.01f) dabs.insert(dabs.end(), {u, v, 0.02f});
    }
  }
  const char* out_dir = std::getenv("MV_REMOVE_OUT_DIR");
  run_port port;
  port.free_memory = []() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_inpaint = [&](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    auto m = inpaint_model::open(**rt, file, inpaint_io{});
    if (!m) return mv::err(m.error());
    return std::unique_ptr<inpaint_filler>(std::move(*m));
  };
  port.still_mask = [&](const std::string&) { return dabs; };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> { return photo; };
  std::string written_name;
  port.write_still = [&](const std::string& source, const run_port::still& pic) -> mv::result<std::string> {
    std::printf("  wrote %ux%u\n", pic.width, pic.height);
    if (out_dir) write_bytes(fs::path(out_dir) / (source + ".rgb"), pic.rgb);
    return source + ".jpg";
  };
  model_spec spec;
  spec.kind = model_kind::remove;
  const auto run = [&](const char* name, bool extend) {
    job_request req;
    req.path = name;
    req.item = item_kind::still;
    job_request::part part;
    part.kind = model_kind::remove;
    part.spec = &spec;
    if (extend) {
      part.values["mode"] = *mv::json::parse("\"extend\"");
      const char* preset = std::getenv("MV_EXTEND_PRESET");
      part.values["extend"] = *mv::json::parse("\"" + std::string(preset ? preset : "all") + "\"");
    }
    req.parts.push_back(part);
    job_runner jr(port);
    std::atomic<bool> done{false};
    bool ok = false;
    std::string message;
    std::mutex m;
    std::vector<double> steps;
    job_events ev;
    ev.progress = [&](double f) {
      std::lock_guard lock(m);
      steps.push_back(f);
    };
    ev.done = [&](bool k, const std::string&, const std::string& why) {
      ok = k;
      message = why;
      done = true;
    };
    const auto t0 = std::chrono::steady_clock::now();
    jr.start(req, ev);
    while (!done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(message);
    REQUIRE(ok);
    std::lock_guard lock(m);
    std::printf("%s: %ux%u in %.2f s, %zu progress reports\n", name, photo.width, photo.height, s, steps.size());
    // Progress only goes forward once it has started (time left reads it).
    for (std::size_t i = 1; i < steps.size(); ++i) {
      if (steps[i - 1] >= 0) CHECK(steps[i] >= steps[i - 1]);
    }
  };
  // MV_QUALITY_ONLY=remove or extend runs only that one.
  const char* only = std::getenv("MV_QUALITY_ONLY");
  if (!only || std::string_view(only) == "remove") run("remove", false);
  if (!only || std::string_view(only) == "extend") run("extend", true);
}

TEST_CASE("restyle: InstructPix2Pix through ONNX Runtime", "[.restyle-model]") {
  const char* dir = std::getenv("MV_RESTYLE_DIR");
  const char* ort = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  const char* raw = std::getenv("MV_RESTYLE_RGB");
  const char* ws = std::getenv("MV_RESTYLE_W");
  const char* hs = std::getenv("MV_RESTYLE_H");
  if (!dir || !ort || !raw || !ws || !hs) SKIP("MV_RESTYLE_DIR / MV_ENHANCE_RUNTIME_DIR / MV_RESTYLE_RGB not set");
  rgb8 full;
  full.width = static_cast<std::uint32_t>(std::atoi(ws));
  full.height = static_cast<std::uint32_t>(std::atoi(hs));
  full.rgb = mv::test::read_bytes(raw);
  REQUIRE(full.rgb.size() == std::size_t{full.width} * full.height * 3);
  auto rt = mv::infer::runtime::load(ort);
  REQUIRE(rt);
  scratch_dir cache("restyle_cache");
  auto t0 = std::chrono::steady_clock::now();
  // The shipped spec's tensor names (the pinned export's), unless
  // MV_RESTYLE_OURS asks for restyle-export.py's own.
  restyle_io io;
  if (!std::getenv("MV_RESTYLE_OURS")) {
    const auto text = mv::test::read_bytes(MV_ENHANCE_RESTYLE_SPEC);
    const auto shipped = parse_spec(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()),
                                    model_kind::restyle);
    REQUIRE(shipped);
    io = restyle_io_of(*shipped);
  }
  auto model = restyle_model::open(**rt, dir, utf8(cache.root()), io);
  REQUIRE(model);
  // MV_RESTYLE_EDGE: a quality's long edge (768 High, 1024 Best), to measure it.
  const char* edge_env = std::getenv("MV_RESTYLE_EDGE");
  const std::uint32_t edge = edge_env ? static_cast<std::uint32_t>(std::atoi(edge_env)) : kRestyleEdge;
  std::uint32_t w = 0, h = 0;
  restyle_size(full.width, full.height, edge, w, h);
  const rgb8 small = resize_rgb(full, w, h);
  for (const char* prompt : {"make it black and white", "make it a snowy winter day"}) {
    restyle_params p;
    p.prompt = prompt;
    t0 = std::chrono::steady_clock::now();
    int last = -1;
    auto out = (*model)->run(small, p, [&](int i, int) { last = i; return true; });
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO("status " << mv::status_name(out ? mv::status::ok : out.error()) << ": " << mv::infer::session::last_error());
    REQUIRE(out);
    CHECK(last == 20);
    double sat = 0, sat0 = 0;
    for (std::size_t i = 0; i < out->rgb.size(); i += 3) {
      auto spread = [](const std::uint8_t* q) { return std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]}); };
      sat += spread(&out->rgb[i]);
      sat0 += spread(&small.rgb[i]);
    }
    std::printf("restyle: \"%s\" at %ux%u in %.1f s on %s, colourfulness %.1f -> %.1f\n", prompt, w, h, s,
                (*model)->on_gpu() ? "Core ML" : "CPU", sat0 / (out->rgb.size() / 3), sat / (out->rgb.size() / 3));
    if (std::string(prompt).find("black") != std::string::npos) CHECK(sat < sat0 * 0.5);
    const rgb8 back = carry_back(full, small, *out, 1.0f);
    if (const char* keep = std::getenv("MV_RESTYLE_OUT")) {
      const std::string base = std::string(keep) + "/" + (prompt[7] == 'i' ? "bw" : "snow");
      std::vector<std::uint8_t> a(out->rgb), b(back.rgb);
      write_bytes(base + "_small.rgb", a);
      write_bytes(base + "_full.rgb", b);
    }
  }

  // The same through the job runner, as the sheet's Run starts it (one
  // model in memory at a time, so a peak measured is one run's).
  model->reset();
  run_port port;
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = full.width;
    s.height = full.height;
    s.rgb = full.rgb;
    return s;
  };
  run_port::still written;
  port.write_still = [&](const std::string&, const run_port::still& s) -> mv::result<std::string> {
    written = s;
    return std::string("/tmp/photo-enhance.jpg");
  };
  port.load_restyle = [&](const model_spec&, const std::string&, int) -> mv::result<std::unique_ptr<restyle_model>> {
    return restyle_model::open(**rt, dir, utf8(cache.root()), io);
  };
  job_runner jr(port);
  model_spec spec;
  spec.kind = model_kind::restyle;
  // The pinned export runs on the CPU (its spec's cpu true); our own needs the GPU.
  spec.cpu = std::getenv("MV_RESTYLE_OURS") == nullptr;
  // The runner at the edge measured above, so it reuses that compile.
  spec.io = *mv::json::parse("{\"edge\": " + std::to_string(edge) + "}");
  job_request req;
  req.path = "photo.jpg";
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::restyle;
  part.spec = &spec;
  part.values["prompt"] = *mv::json::parse("\"make it black and white\"");
  req.parts.push_back(part);
  std::atomic<bool> done{false};
  bool ok = false;
  std::string name, message;
  double last_progress = -2;
  job_events ev;
  ev.progress = [&](double f) { last_progress = f; };
  ev.done = [&](bool k, const std::string& n, const std::string& m) {
    ok = k;
    name = n;
    message = m;
    done = true;
  };
  jr.start(req, ev);
  for (int i = 0; i < 6000 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  INFO(message);
  REQUIRE(done);
  CHECK(ok);
  CHECK(name == "photo-enhance.jpg");
  CHECK(written.width == full.width);
  CHECK(last_progress > 0.5);
}

TEST_CASE("restyle: offered on a photo once pinned, with a text knob, and an empty request refused",
          "[enhance][restyle]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0002.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/restyle/model.json", R"json({
    "kind": "restyle", "name": "Restyle", "weights": ["r.onnx"], "run_bytes": 1000, "cpu": false,
    "knobs": [{"id": "prompt", "label": "Change", "type": "text", "default": "", "placeholder": "make it winter"},
              {"id": "keep", "label": "Keep the photo", "type": "slider", "min": 1, "max": 2.5, "default": 1.5}],
    "io": {"runner": "diffusion", "edge": 512, "steps": 20}})json");
  f.restart();
  // Its model.json alone is not a model anyone can download: not offered.
  f.eng->sheet_opened(3, utf8(photo));
  CHECK_FALSE(parsed(f.host.tree(kSheetScreen)).has("photo"));

  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pin = R"json({"url": "https://huggingface.co/mv-test/upscale/resolve/0123abcd/up.onnx",
      "path": "restyle/r.onnx", "size": )json" + std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" +
                            hex_of(f.video_weights) + R"json(", "licence": "CreativeML OpenRAIL-M"},)json";
    m.insert(m.find("[") + 1, pin);
    write_text(f.self / "manifest.json", m);
  }
  f.restart();
  f.eng->sheet_opened(4, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    REQUIRE(t.has("photo"));
    CHECK(t.node("photo")->find("options")->a.size() == 2);
    CHECK_FALSE(t.has("k.restyle.prompt"));  // more detail by default
  }
  f.eng->event(kSheetScreen, "photo", MV_SLOT_EVENT_CHANGE, "\"restyle\"");
  CHECK(parsed(f.host.tree(kSheetScreen)).text("missing.restyle").find("not installed") != std::string::npos);
  f.eng->event(kSettingsScreen, "install.restyle", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::restyle).state == dl_state::ready; }));
  f.eng->sheet_opened(5, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    REQUIRE(t.has("k.restyle.prompt"));
    CHECK(*t.node("k.restyle.prompt")->str("type") == "text_field");
    CHECK(*t.node("k.restyle.prompt")->str("placeholder") == "make it winter");
    CHECK(t.has("k.restyle.keep"));
  }
  // The words are remembered; a request too long is refused.
  f.eng->event(kSheetScreen, "k.restyle.prompt", MV_SLOT_EVENT_CHANGE, "\"make it a snowy winter day\"");
  CHECK(f.eng->current_prefs().of(model_kind::restyle).at("prompt").s == "make it a snowy winter day");
  f.eng->event(kSheetScreen, "k.restyle.prompt", MV_SLOT_EVENT_CHANGE, "\"" + std::string(300, 'x') + "\"");
  CHECK(f.eng->current_prefs().of(model_kind::restyle).at("prompt").s == "make it a snowy winter day");
  CHECK(f.eng->current_prefs().photo == photo_target::restyle);

  // The runner: Restyle runs on a still once it can load the model, and
  // an empty request is refused before anything starts.
  run_port port;
  port.open_still = [](const std::string&) -> mv::result<run_port::still> { return mv::err(mv::status::io); };
  port.write_still = [](const std::string&, const run_port::still&) -> mv::result<std::string> { return std::string(); };
  CHECK_FALSE(job_runner(port).supports(model_kind::restyle, item_kind::still));
  port.load_restyle = [](const model_spec&, const std::string&, int) -> mv::result<std::unique_ptr<restyle_model>> {
    return mv::err(mv::status::unsupported_format);
  };
  job_runner jr(port);
  CHECK(jr.supports(model_kind::restyle, item_kind::still));
  CHECK_FALSE(jr.supports(model_kind::restyle, item_kind::media));
  const model_spec* spec = f.eng->spec(model_kind::restyle);
  REQUIRE(spec);
  job_request req;
  req.path = utf8(photo);
  req.item = item_kind::still;
  job_request::part part;
  part.kind = model_kind::restyle;
  part.spec = spec;
  part.values["prompt"] = *mv::json::parse("\"   \"");
  req.parts.push_back(part);
  CHECK(jr.check(req).find("Type what to change") != std::string::npos);
  req.parts[0].values["prompt"] = *mv::json::parse("\"make it winter\"");
  CHECK(jr.check(req).empty());
}

// ---- S7: Enhance image (docs/plans/edit-refresh.md, "Models") ----------------------

namespace {

// The same size back, plus `bias` on every other tile: a seam maker, so only
// the blending can hide where tiles meet.
struct same1 final : tile_model {
  std::uint32_t t;
  float bias;
  int calls = 0;
  same1(std::uint32_t tile_, float bias_ = 0) : t(tile_), bias(bias_) {}
  std::uint32_t tile() const noexcept override { return t; }
  std::uint32_t scale() const noexcept override { return 1; }
  mv::expected run(std::span<const float> in, std::vector<float>& out) override {
    out.assign(in.begin(), in.end());
    if (calls++ % 2 == 1) {
      for (float& v : out) v += bias;
    }
    return {};
  }
};

}  // namespace

TEST_CASE("enhance image: tiles start inside the photo and overlap", "[enhance][still]") {
  CHECK(tile_starts(0, 32, 8).empty());
  CHECK(tile_starts(20, 32, 8) == std::vector<std::uint32_t>{0});  // smaller than a tile: one, padded
  CHECK(tile_starts(32, 32, 8) == std::vector<std::uint32_t>{0});
  CHECK(tile_starts(100, 32, 8) == std::vector<std::uint32_t>{0, 24, 48, 68});  // the last ends at the edge
  for (const std::uint32_t n : {33u, 57u, 192u, 193u, 1000u, 4032u}) {
    for (const auto& [t, o] : {std::pair{32u, 8u}, std::pair{192u, 64u}, std::pair{512u, 128u}}) {
      const auto s = tile_starts(n, t, o);
      INFO(n << " " << t << " " << o);
      REQUIRE_FALSE(s.empty());
      CHECK(s.front() == 0);
      if (n >= t) CHECK(s.back() + t == n);
      for (std::size_t i = 1; i < s.size(); ++i) {
        CHECK(s[i] > s[i - 1]);
        CHECK(s[i - 1] + t >= s[i] + o);  // neighbours share at least the overlap
        CHECK(s[i] + t <= n);
      }
    }
  }
  CHECK(restore_tiles(100, 40, 32, 8) == 4u * 2u);
  CHECK(restore_tiles(10, 10, 192, 64) == 1u);
  // 12 MP at Real-HAT-GAN's 192 with a third shared: 744 tiles, about 2.2x
  // the 336 of no overlap at all.
  CHECK(restore_tiles(4032, 3024, 192, 64) == 31u * 24u);
  CHECK(restore_tiles(4032, 3024, 192, 0) == 21u * 16u);

  // The tent: flat at the photo's own edges, rising and falling across the
  // overlap toward a neighbour, never 0.
  const auto flat = tent_weights(16, 4, true, true);
  CHECK(std::all_of(flat.begin(), flat.end(), [](float v) { return v == 1.0f; }));
  const auto mid = tent_weights(16, 4, false, false);
  CHECK(mid[0] > 0.0f);
  CHECK(mid[0] < mid[1]);
  CHECK(mid[1] < mid[2]);
  CHECK(mid[4] == 1.0f);
  CHECK(mid[11] == 1.0f);
  CHECK(mid[15] == Approx(mid[0]));
  const auto left = tent_weights(16, 4, true, false);
  CHECK(left[0] == 1.0f);
  CHECK(left[15] < 1.0f);
  // A ramp and its neighbour's add to one across the overlap (a crossfade).
  for (std::uint32_t i = 0; i < 4; ++i) CHECK(mid[12 + i] + mid[i] == Approx(1.0f).margin(1e-5));
}

TEST_CASE("enhance image: tiles blend into one picture at every size", "[enhance][still]") {
  // A flat photo stays flat at 1x, 2x and 4x, however the tiles fall; every
  // tile runs once.
  for (const auto& [w, h] : {std::pair{70u, 45u}, std::pair{20u, 10u}, std::pair{101u, 7u}}) {
    const auto rgb = flat_rgb(w, h, 77);
    for (const std::uint32_t s : {1u, 2u, 4u}) {
      nearest4 m(32);
      restore_options o;
      o.out_scale = s;
      o.overlap = 8;
      std::vector<std::uint8_t> out;
      REQUIRE(restore_picture(rgb, w, h, m, o, out));
      INFO(w << "x" << h << " at " << s << "x");
      REQUIRE(out.size() == std::size_t{w} * s * h * s * 3);
      CHECK(static_cast<std::uint64_t>(m.calls) == restore_tiles(w, h, 32, 8));
      CHECK(std::all_of(out.begin(), out.end(), [](std::uint8_t v) { return v == 77; }));
    }
  }
  // At the model's own 4x every pixel is the model's, wherever tiles overlap.
  {
    const auto rgb = noise_rgb(70, 45);
    nearest4 m(32);
    restore_options o;
    o.out_scale = 4;
    o.overlap = 8;
    std::vector<std::uint8_t> out;
    REQUIRE(restore_picture(rgb, 70, 45, m, o, out));
    bool same = true;
    for (std::uint32_t y = 0; y < 180 && same; ++y) {
      for (std::uint32_t x = 0; x < 280 && same; ++x) {
        for (int c = 0; c < 3; ++c) same = same && out[(std::size_t{y} * 280 + x) * 3 + c] == rgb[(std::size_t{y / 4} * 70 + x / 4) * 3 + c];
      }
    }
    CHECK(same);
  }
  // At the same size, 4x then down: a ramp comes back a ramp (the Lanczos
  // downscale of the model's pixels, not a nearest pick).
  {
    std::vector<std::uint8_t> ramp(std::size_t{96} * 64 * 3);
    for (std::uint32_t y = 0; y < 64; ++y) {
      for (std::uint32_t x = 0; x < 96; ++x) {
        for (int c = 0; c < 3; ++c) ramp[(std::size_t{y} * 96 + x) * 3 + c] = static_cast<std::uint8_t>(60 + x + y / 2);
      }
    }
    nearest4 m(32);
    restore_options o;
    o.overlap = 8;
    std::vector<std::uint8_t> out;
    REQUIRE(restore_picture(ramp, 96, 64, m, o, out));
    int worst = 0;
    for (std::size_t i = 0; i < out.size(); ++i) worst = std::max(worst, std::abs(int(out[i]) - int(ramp[i])));
    CHECK(worst <= 1);
  }
  // A model that is the photo itself gives the photo back, bit for bit.
  {
    const auto rgb = noise_rgb(90, 50);
    same1 m(32);
    restore_options o;
    o.overlap = 12;
    std::vector<std::uint8_t> out;
    REQUIRE(restore_picture(rgb, 90, 50, m, o, out));
    CHECK(out == rgb);
  }
  // Tiles that disagree (every other one 25 levels brighter) cross-fade:
  // no step between neighbouring pixels is more than the ramp's.
  {
    const std::uint32_t w = 220, h = 64;
    const auto rgb = flat_rgb(w, h, 100);
    same1 m(64, 0.1f);
    restore_options o;
    o.overlap = 16;
    std::vector<std::uint8_t> out;
    REQUIRE(restore_picture(rgb, w, h, m, o, out));
    int step = 0, lo = 255, hi = 0;
    for (std::uint32_t x = 1; x < w; ++x) {
      step = std::max(step, std::abs(int(out[x * 3]) - int(out[(x - 1) * 3])));
      lo = std::min(lo, int(out[x * 3]));
      hi = std::max(hi, int(out[x * 3]));
    }
    INFO("levels " << lo << ".." << hi);
    CHECK(hi - lo >= 20);  // the tiles did differ
    CHECK(step <= 3);      // 25 levels over a 16-pixel ramp
  }
  // Strength 0 is the photo (at the same size) or its plain enlargement.
  {
    const auto rgb = noise_rgb(40, 30);
    nearest4 m(32);
    restore_options o;
    o.overlap = 8;
    o.strength = 0;
    std::vector<std::uint8_t> out;
    REQUIRE(restore_picture(rgb, 40, 30, m, o, out));
    CHECK(out == rgb);
    const auto flat = flat_rgb(40, 30, 50);
    o.out_scale = 2;
    REQUIRE(restore_picture(flat, 40, 30, m, o, out));
    CHECK(std::all_of(out.begin(), out.end(), [](std::uint8_t v) { return v == 50; }));
  }
  // Sizes the model cannot give, and an overlap with no centre, are refused.
  {
    const auto rgb = noise_rgb(40, 30);
    nearest4 m(32);
    same1 one(32);
    restore_options o;
    std::vector<std::uint8_t> out;
    o.out_scale = 3;
    CHECK_FALSE(restore_picture(rgb, 40, 30, m, o, out));
    o.out_scale = 2;
    CHECK_FALSE(restore_picture(rgb, 40, 30, one, o, out));  // a same-size model enlarges nothing
    o.out_scale = 1;
    o.overlap = 32;
    CHECK_FALSE(restore_picture(rgb, 40, 30, m, o, out));
  }
}

TEST_CASE("enhance image: progress per tile and a cancel between tiles", "[enhance][still]") {
  const auto rgb = noise_rgb(100, 70);
  same1 m(32);
  restore_options o;
  o.overlap = 8;
  std::vector<std::pair<std::size_t, std::size_t>> seen;
  o.progress = [&](std::size_t done, std::size_t total) { seen.emplace_back(done, total); };
  std::vector<std::uint8_t> out;
  REQUIRE(restore_picture(rgb, 100, 70, m, o, out));
  const std::size_t total = restore_tiles(100, 70, 32, 8);
  REQUIRE(seen.size() == total + 1);
  for (std::size_t i = 0; i <= total; ++i) CHECK(seen[i] == std::pair{i, total});

  std::atomic<bool> stop{false};
  same1 m2(32);
  o.cancel = &stop;
  o.progress = [&](std::size_t done, std::size_t) {
    if (done == 2) stop = true;
  };
  const auto r = restore_picture(rgb, 100, 70, m2, o, out);
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::cancelled);
  CHECK(m2.calls == 2);
}

namespace {

// The photo back, or wild values (thousands, as NAFNet's exports give on some
// tiles): always, or only when the tile's top-left is brighter than its
// top-right (so a mirrored view comes back sound).
struct wild1 final : tile_model {
  std::uint32_t t;
  bool always;
  int calls = 0;
  wild1(std::uint32_t tile_, bool always_) : t(tile_), always(always_) {}
  std::uint32_t tile() const noexcept override { return t; }
  std::uint32_t scale() const noexcept override { return 1; }
  mv::expected run(std::span<const float> in, std::vector<float>& out) override {
    ++calls;
    out.assign(in.begin(), in.end());
    if (always || in[0] > in[t - 1]) {
      for (std::size_t i = 0; i < out.size(); ++i) out[i] = (i % 2 ? 4000.0f : -3000.0f);
    }
    return {};
  }
};

}  // namespace

TEST_CASE("enhance image: a tile the model blows up is asked again, else left as the photo", "[enhance][still]") {
  const auto rgb = noise_rgb(90, 50);
  const std::uint64_t tiles = restore_tiles(90, 50, 32, 12);
  restore_options o;
  o.overlap = 12;
  std::size_t unstable = 99;
  o.unstable = &unstable;
  std::vector<std::uint8_t> out;
  // Sound when mirrored: every tile restored (here: the photo), none given up.
  wild1 some(32, false);
  REQUIRE(restore_picture(rgb, 90, 50, some, o, out));
  CHECK(out == rgb);
  CHECK(unstable == 0);
  CHECK(static_cast<std::uint64_t>(some.calls) > tiles);
  // Never sound: each tile asked four ways, then the photo's own pixels.
  wild1 never(32, true);
  REQUIRE(restore_picture(rgb, 90, 50, never, o, out));
  CHECK(out == rgb);
  CHECK(unstable == tiles);
  CHECK(static_cast<std::uint64_t>(never.calls) == 4 * tiles);
}

namespace {

// The photo back with `every`-th sample pushed to 1.4: in range, but past a
// sound restorer's overshoot (a half-broken tile speckles like this).
struct speckle1 final : tile_model {
  std::uint32_t t;
  std::size_t every;
  int calls = 0;
  speckle1(std::uint32_t tile_, std::size_t every_) : t(tile_), every(every_) {}
  std::uint32_t tile() const noexcept override { return t; }
  std::uint32_t scale() const noexcept override { return 1; }
  mv::expected run(std::span<const float> in, std::vector<float>& out) override {
    ++calls;
    out.assign(in.begin(), in.end());
    for (std::size_t i = 0; i < out.size(); i += every) out[i] = 1.4f;
    return {};
  }
};

}  // namespace

TEST_CASE("enhance image: a speckled tile is not a picture, an edge's overshoot is", "[enhance][still]") {
  const auto rgb = noise_rgb(90, 50);
  const std::uint64_t tiles = restore_tiles(90, 50, 32, 12);
  restore_options o;
  o.overlap = 12;
  std::size_t unstable = 99;
  o.unstable = &unstable;
  std::vector<std::uint8_t> out;
  // 1 % of samples past the range (NAFNet-GoPro's half-broken tiles): given up.
  speckle1 speckled(32, 100);
  REQUIRE(restore_picture(rgb, 90, 50, speckled, o, out));
  CHECK(out == rgb);
  CHECK(unstable == tiles);
  CHECK(static_cast<std::uint64_t>(speckled.calls) == 4 * tiles);
  // One sample a tile (0.03 %), as a sharpener's ringing: kept, asked once.
  speckle1 ringing(32, 32 * 32 * 3);
  REQUIRE(restore_picture(rgb, 90, 50, ringing, o, out));
  CHECK(unstable == 0);
  CHECK(static_cast<std::uint64_t>(ringing.calls) == tiles);
}

TEST_CASE("enhance image: five choices, each model fetched only when chosen", "[enhance][still]") {
  fixture f;
  const fs::path photo = f.dir / "IMG_0002.JPG";
  write_bytes(photo, bytes({0xFF, 0xD8, 0xFF, 0xE1}));
  write_text(f.self / "models/still/model.json", R"json({
    "kind": "still", "name": "Sharpen & restore", "credit": "Test (Apache-2.0)", "weights": ["up.onnx"],
    "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}],
               "default": "1"}],
    "io": {"runner": "tiles", "scale": 4, "tile": 32, "overlap": 8, "dims": []}})json");
  write_text(f.self / "models/denoise/model.json", R"json({
    "kind": "denoise", "name": "Remove noise", "credit": "Test (Apache-2.0)", "weights": ["dn.onnx"],
    "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "strength", "label": "Strength", "type": "slider", "min": 0, "max": 1, "step": 0.05, "default": 1}],
    "io": {"runner": "tiles", "scale": 1, "tile": 32, "overlap": 8, "dims": ["a", "b"], "fixed": {"n": 1},
           "output": "output", "backend": "cpu"}})json");
  const std::string kStillUrl = "https://huggingface.co/mv-test/hat/resolve/0123abcd/up.onnx";
  const std::string kDenoiseUrl = "https://huggingface.co/mv-test/scunet/resolve/0123abcd/dn.onnx";
  const auto dn_weights = mv::test::pattern(30'000, 3);
  {
    const auto text = mv::test::read_bytes(f.self / "manifest.json");
    std::string m(text.begin(), text.end());
    const std::string pins =
        R"json({"url": ")json" + kStillUrl + R"json(", "path": "still/up.onnx", "size": )json" +
        std::to_string(f.video_weights.size()) + R"json(, "sha256": ")json" + hex_of(f.video_weights) +
        R"json(", "licence": "Apache-2.0"}, {"url": ")json" + kDenoiseUrl +
        R"json(", "path": "denoise/dn.onnx", "size": )json" + std::to_string(dn_weights.size()) +
        R"json(, "sha256": ")json" + hex_of(dn_weights) + R"json(", "licence": "Apache-2.0"},)json";
    m.insert(m.find("[") + 1, pins);
    write_text(f.self / "manifest.json", m);
  }
  f.net->files[kStillUrl] = f.video_weights;
  f.net->files[kDenoiseUrl] = dn_weights;
  f.restart();
  CHECK(f.net->count() == 0);
  f.eng->event(kSettingsScreen, "install.still", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::still).state == dl_state::ready; }));
  const std::size_t after_still = f.net->count();

  std::uint32_t written_w = 0, written_h = 0;
  std::string loaded_from;
  run_port port;
  port.free_memory = [&]() -> std::optional<std::uint64_t> { return 8ull << 30; };
  port.load_tiles = [&](const model_spec& s, const std::string& dir) -> mv::result<std::unique_ptr<tile_model>> {
    loaded_from = dir;
    if (s.kind == model_kind::denoise) return std::unique_ptr<tile_model>(new same1(32));
    return std::unique_ptr<tile_model>(new nearest4(32));
  };
  port.open_still = [&](const std::string&) -> mv::result<run_port::still> {
    run_port::still s;
    s.width = 40;
    s.height = 30;
    s.rgb = noise_rgb(40, 30);
    return s;
  };
  port.write_still = [&](const std::string&, const run_port::still& pic) -> mv::result<std::string> {
    written_w = pic.width;
    written_h = pic.height;
    return utf8(f.dir / "IMG_0002-enhance.jpg");
  };
  f.eng->set_runner(std::make_unique<job_runner>(port));

  f.eng->sheet_opened(1, utf8(photo));
  {
    parsed t(f.host.tree(kSheetScreen));
    const mv::json::value* d = t.node("detail");
    REQUIRE(d);
    CHECK(*d->str("value") == "still");
    const mv::json::value* opts = d->find("options");
    REQUIRE(opts);
    REQUIRE(opts->a.size() == 2);  // the kinds this build has a spec and a pin for, in kStillKinds order
    CHECK(*opts->a[0].str("id") == "still");
    CHECK(*opts->a[1].str("id") == "denoise");
    CHECK(*opts->a[1].str("text") == "Remove noise");
    CHECK(t.has("k.still.scale"));
    CHECK(t.text("credit.still") == "Test (Apache-2.0)");
  }
  // Same size by default: 4x restored, then brought back.
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  CHECK(written_w == 40u);
  CHECK(written_h == 30u);
  CHECK(loaded_from.find("models/still") != std::string::npos);

  // Remove noise is not here yet: the sheet offers its download and nothing
  // is fetched until that click.
  f.eng->event(kSheetScreen, "detail", MV_SLOT_EVENT_CHANGE, "\"denoise\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(*t.node("detail")->str("value") == "denoise");
    CHECK_FALSE(t.has("k.still.scale"));
    REQUIRE(t.has("install.denoise"));
    CHECK(t.text("missing.denoise").find("Remove noise needs its model") != std::string::npos);
    CHECK(t.node("run")->boolean("disabled").value_or(false));
  }
  CHECK(f.net->count() == after_still);
  CHECK(f.eng->current_prefs().detail == model_kind::denoise);
  f.eng->event(kSheetScreen, "install.denoise", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::denoise).state == dl_state::ready; }));
  {
    std::lock_guard lock(f.net->m);
    REQUIRE(f.net->calls.size() == after_still + 1);
    CHECK(f.net->calls.back().first == kDenoiseUrl);
  }
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).has("k.denoise.strength"); }));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK_FALSE(t.has("k.denoise.scale"));  // a same-size model has no Size
    CHECK_FALSE(t.node("run")->boolean("disabled").value_or(false));
  }
  written_w = written_h = 0;
  f.eng->event(kSheetScreen, "run", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return written_w != 0; }));
  CHECK(written_w == 40u);
  CHECK(written_h == 30u);
  CHECK(loaded_from.find("models/denoise") != std::string::npos);

  // The choice is kept; an unknown or unoffered one is ignored.
  REQUIRE(eventually([&] { return parsed(f.host.tree(kSheetScreen)).text("status").find("Saved as") != std::string::npos; }));
  f.eng->event(kSheetScreen, "detail", MV_SLOT_EVENT_CHANGE, "\"motion\"");
  CHECK(f.eng->current_prefs().detail == model_kind::denoise);
  f.restart();
  CHECK(f.eng->current_prefs().detail == model_kind::denoise);
}

#ifdef MV_ENHANCE_SOURCE_DIR
TEST_CASE("enhance image: the shipped models, their pins and credits", "[enhance][still]") {
  const fs::path src(MV_ENHANCE_SOURCE_DIR);
  struct want {
    model_kind kind;
    const char* file;
    std::uint64_t size;
    const char* sha;
    const char* licence;
    const char* revision;
    std::uint32_t scale;
  };
  // docs/plans/edit-refresh.md, "Models" (owner OK 2026-11-04).
  const want table[] = {
      {model_kind::still, "Real_HAT_GAN_sharper.onnx", 161105121,
       "b3df277d8f68cf440eca120d39aa659fb08e2537a22d368646e79acfc2062db6", "Apache-2.0",
       "MahoCommerce/Real_HAT_GAN_sharper-ONNX/resolve/f0c8c3a46b1a45706d34ac73cbd71c21893a1ebb/onnx/model.onnx", 4},
      {model_kind::natural, "4xRealWebPhoto_v4_fp32_opset17.onnx", 48760847,
       "a9a3a9099e0108b793556ed9cf2123a61a727bbc611dd8d93475f7144596589e", "CC-BY-4.0",
       "Phips/4xRealWebPhoto_v4_dat2/resolve/95e9cd50553951c27d86bafbdc1bc27ca8e750d2/", 4},
      {model_kind::denoise, "SCUNet-PSNR.onnx", 91264256,
       "b0f8c12f1575bb49e39a85924152f1c6d4b527a4aae0432c9e5c7397123465e3", "Apache-2.0",
       "deepghs/image_restoration/resolve/77885239ff28876d6b37dd080ab86aa1de2c49cc/", 1},
      {model_kind::deblur, "NAFNet-REDS-width64.onnx", 275371113,
       "fb5b264996d26cb1f7399a0af4e25cd26dbd5241910e58f7a4f1275734f76ea1", "MIT",
       "deepghs/image_restoration/resolve/77885239ff28876d6b37dd080ab86aa1de2c49cc/", 1},
      {model_kind::motion, "NAFNet-GoPro-width64.onnx", 275371113,
       "00c07294585512d162bc2313a2a12cd8d9cd532d0981d0d01f444bd48c4330cd", "MIT",
       "deepghs/image_restoration/resolve/77885239ff28876d6b37dd080ab86aa1de2c49cc/", 1},
  };
  const auto remote_bytes = mv::test::read_bytes(src / "remote.json");
  const auto remote = mv::json::parse(std::string(remote_bytes.begin(), remote_bytes.end()), 8);
  REQUIRE(remote);
  REQUIRE(remote->k == mv::json::kind::array);
  for (const want& w : table) {
    INFO(kind_name(w.kind));
    const auto text = mv::test::read_bytes(src / "models" / kind_name(w.kind) / "model.json");
    const auto spec = parse_spec(std::string(text.begin(), text.end()), w.kind);
    REQUIRE(spec);
    REQUIRE(spec->weights.size() == 1);
    CHECK(spec->weights[0] == w.file);
    CHECK((spec->find("scale") != nullptr) == (w.scale > 1));  // a Size only where the model enlarges
    CHECK_FALSE(spec->credit.empty());
    const auto shape = tile_shape_of(*spec);
    REQUIRE(shape);
    CHECK(shape->scale == w.scale);
    CHECK(shape->cpu_only);                       // fp32 on the CPU (quality over speed)
    CHECK(shape->overlap * 4 >= shape->tile);     // at least a quarter of a tile shared each side
    const std::string path = std::string(kind_name(w.kind)) + "/" + w.file;
    const mv::json::value* pin = nullptr;
    for (const mv::json::value& p : remote->a) {
      if (const std::string* pp = p.str("path"); pp && *pp == path) pin = &p;
    }
    REQUIRE(pin);
    CHECK(pin->find("size")->i == static_cast<std::int64_t>(w.size));
    CHECK(*pin->str("sha256") == w.sha);
    CHECK(*pin->str("licence") == w.licence);
    CHECK(pin->str("url")->find(w.revision) != std::string::npos);
  }
  // Real-HAT-GAN is fixed at 192 -> 768; the rest have free sizes.
  const auto hat_text = mv::test::read_bytes(src / "models/still/model.json");
  const auto hat = tile_shape_of(*parse_spec(std::string(hat_text.begin(), hat_text.end()), model_kind::still));
  CHECK(hat->tile == 192);
  CHECK(hat->dim_h.empty());
  CHECK(hat->output == "reconstruction");
  // CC-BY-4.0 asks for the credit: Philip Hofmann's, in the spec.
  const auto nat_text = mv::test::read_bytes(src / "models/natural/model.json");
  const auto nat = parse_spec(std::string(nat_text.begin(), nat_text.end()), model_kind::natural);
  CHECK(nat->credit.find("Philip Hofmann") != std::string::npos);
  CHECK(nat->credit.find("CC-BY-4.0") != std::string::npos);
}
#endif

// ---- the real models, when they are here -------------------------------------------

namespace {

// A picture with edges at every scale: rings, bars, a checker and fine lines
// over a soft gradient (synthetic: nothing about it is a person's photo).
std::vector<float> scene(std::uint32_t w, std::uint32_t h) {
  std::vector<float> v(std::size_t{w} * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const double fx = double(x) / w, fy = double(y) / h;
      double g = 0.25 + 0.35 * fx;
      const double r = std::hypot(x - w * 0.3, y - h * 0.4);
      if (std::fmod(r, 14.0) < 5.0) g += 0.3;
      if (x > w / 2 && ((x / 6 + y / 6) % 2 == 0)) g -= 0.2;
      if (y % 23 == 0 || x % 37 == 0) g = 0.92;
      if (fy > 0.75 && (x / 3) % 2 == 0) g = 0.1;
      for (int c = 0; c < 3; ++c) {
        v[(std::size_t{y} * w + x) * 3 + c] = static_cast<float>(std::clamp(g * (0.85 + 0.1 * c), 0.0, 1.0));
      }
    }
  }
  return v;
}

std::vector<float> blur(const std::vector<float>& in, std::uint32_t w, std::uint32_t h, int rx, int ry) {
  // Separable box blur, rx / ry each side, applied twice (near Gaussian when both are set).
  std::vector<float> a = in, b(in.size());
  for (int pass = 0; pass < 2; ++pass) {
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
          double s = 0;
          for (int d = -rx; d <= rx; ++d) {
            const auto xx = static_cast<std::uint32_t>(std::clamp<int>(int(x) + d, 0, int(w) - 1));
            s += a[(std::size_t{y} * w + xx) * 3 + c];
          }
          b[(std::size_t{y} * w + x) * 3 + c] = static_cast<float>(s / (2 * rx + 1));
        }
      }
    }
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
          double s = 0;
          for (int d = -ry; d <= ry; ++d) {
            const auto yy = static_cast<std::uint32_t>(std::clamp<int>(int(y) + d, 0, int(h) - 1));
            s += b[(std::size_t{yy} * w + x) * 3 + c];
          }
          a[(std::size_t{y} * w + x) * 3 + c] = static_cast<float>(s / (2 * ry + 1));
        }
      }
    }
  }
  return a;
}

std::vector<std::uint8_t> to8(const std::vector<float>& v, float noise = 0) {
  std::vector<std::uint8_t> out(v.size());
  std::uint32_t seed = 12345;
  const auto uni = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (static_cast<double>(seed >> 8) + 0.5) / 16777216.0;
  };
  for (std::size_t i = 0; i < v.size(); ++i) {
    double x = v[i];
    if (noise > 0) x += noise * std::sqrt(-2 * std::log(uni())) * std::cos(2 * 3.14159265358979 * uni());
    out[i] = static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255));
  }
  return out;
}

// Variance of the 4-neighbour Laplacian on luma: higher is sharper.
double lap_var(const std::vector<std::uint8_t>& rgb, std::uint32_t w, std::uint32_t h) {
  const auto Y = [&](std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (std::size_t{y} * w + x) * 3;
    return 0.299 * rgb[i] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2];
  };
  double s = 0, s2 = 0;
  std::size_t n = 0;
  for (std::uint32_t y = 2; y + 2 < h; ++y) {
    for (std::uint32_t x = 2; x + 2 < w; ++x) {
      const double l = 4 * Y(x, y) - Y(x - 1, y) - Y(x + 1, y) - Y(x, y - 1) - Y(x, y + 1);
      s += l;
      s2 += l * l;
      ++n;
    }
  }
  const double count = static_cast<double>(n);
  const double m = s / count;
  return s2 / count - m * m;
}

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  double e = 0;
  for (std::size_t i = 0; i < a.size(); ++i) e += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
  e /= static_cast<double>(a.size());
  return 10 * std::log10(255.0 * 255.0 / std::max(e, 1e-9));
}

double mean_of(const std::vector<std::uint8_t>& v) {
  double s = 0;
  for (const std::uint8_t b : v) s += b;
  return s / static_cast<double>(v.size());
}

}  // namespace

// The five downloaded models on ONNX Runtime, when they are here:
// MV_ENHANCE_IMAGE_MODELS names a folder laid out as the toolchain's
// enhance-models (hat/, realwebphoto/, scunet/, nafnet-reds/, nafnet-gopro/);
// ONNX Runtime is the one staged beside mv_enhance (MV_ENHANCE_RUNTIME_DIR).
// MV_ENHANCE_IMAGE_SIZE ("320x240" by default) sets the picture's size;
// MV_ENHANCE_IMAGE_RGB=<raw rgb8 of that size> uses a real crop instead of
// the synthetic scene; MV_ENHANCE_IMAGE_ONLY=<kind> runs one. Each is given
// the damage it is for (blur, sideways shake, noise); prints seconds per
// megapixel.
TEST_CASE("enhance image: the five models restore a damaged picture", "[.enhance-image-model]") {
#ifndef MV_ENHANCE_SOURCE_DIR
  SKIP("no source dir");
#else
  const char* models = std::getenv("MV_ENHANCE_IMAGE_MODELS");
  const char* runtime = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  if (!models || !runtime) SKIP("MV_ENHANCE_IMAGE_MODELS / MV_ENHANCE_RUNTIME_DIR not set");
  std::uint32_t w = 320, h = 240;
  if (const char* sz = std::getenv("MV_ENHANCE_IMAGE_SIZE")) {
    unsigned a = 0, b = 0;
    if (std::sscanf(sz, "%ux%u", &a, &b) == 2 && a >= 16 && b >= 16) w = a, h = b;
  }
  const char* only = std::getenv("MV_ENHANCE_IMAGE_ONLY");
  auto clean_f = scene(w, h);
  if (const char* raw = std::getenv("MV_ENHANCE_IMAGE_RGB")) {
    // A real crop instead (rgb8, MV_ENHANCE_IMAGE_SIZE its size).
    const auto bytes = mv::test::read_bytes(raw);
    REQUIRE(bytes.size() == std::size_t{w} * h * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) clean_f[i] = bytes[i] / 255.0f;
  }
  const auto clean = to8(clean_f);
  const auto soft = to8(blur(clean_f, w, h, 1, 1));    // out of focus
  const auto shaken = to8(blur(clean_f, w, h, 4, 0));  // moved sideways
  const auto noisy = to8(clean_f, 0.06f);              // high ISO

  runtime_holder rt(runtime);
  scratch_dir dir("enhance-image-model");
  struct run_case {
    model_kind kind;
    const char* sub;
    const char* file;
    std::uint32_t scale;
  };
  const run_case cases[] = {
      {model_kind::still, "hat", "model.onnx", 1},
      {model_kind::natural, "realwebphoto", "4xRealWebPhoto_v4_fp32_opset17.onnx", 2},
      {model_kind::denoise, "scunet", "SCUNet-PSNR.onnx", 1},
      {model_kind::deblur, "nafnet-reds", "NAFNet-REDS-width64.onnx", 1},
      {model_kind::motion, "nafnet-gopro", "NAFNet-GoPro-width64.onnx", 1},
  };
  for (const run_case& rc : cases) {
    if (only && std::string(only) != kind_name(rc.kind)) continue;
    INFO(kind_name(rc.kind));
    const fs::path file = fs::path(models) / rc.sub / rc.file;
    if (!fs::exists(file)) {
      WARN("missing " << utf8(file));
      continue;
    }
    const auto text = mv::test::read_bytes(fs::path(MV_ENHANCE_SOURCE_DIR) / "models" / kind_name(rc.kind) / "model.json");
    const auto spec = parse_spec(std::string(text.begin(), text.end()), rc.kind);
    REQUIRE(spec);
    // The add-on's folder layout: models/<kind>/<weights>, here a link.
    const fs::path kdir = dir / kind_name(rc.kind);
    fs::create_directories(kdir);
    fs::create_symlink(file, kdir / spec->weights.front());
    auto m = rt.open_tiles(*spec, utf8(kdir), utf8(dir / "cache"));
    INFO(mv::infer::session::last_error());
    REQUIRE(m);
    const auto& in = rc.kind == model_kind::denoise ? noisy : rc.kind == model_kind::motion ? shaken : soft;
    restore_options o;
    o.out_scale = rc.scale;
    o.overlap = tile_shape_of(*spec)->overlap;
    std::size_t tiles = 0, unstable = 0;
    o.progress = [&](std::size_t, std::size_t total) { tiles = total; };
    o.unstable = &unstable;
    std::vector<std::uint8_t> out;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(restore_picture(in, w, h, **m, o, out));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double mp = double(w) * h / 1e6;
    std::printf("enhance image %-8s %ux%u -> x%u: %zu tiles, %.1f s, %.1f s per input megapixel\n",
                kind_name(rc.kind), w, h, rc.scale, tiles, secs, secs / mp);
    REQUIRE(out.size() == std::size_t{w} * rc.scale * h * rc.scale * 3);
    // In range: the picture's brightness kept, not blown out or crushed
    // (the scene's one-pixel bars move an upscaler's mean a little).
    CHECK(std::fabs(mean_of(out) - mean_of(in)) < 12.0);
    if (unstable) std::printf("  %zu of %zu tiles left as the photo (the model blew up on them)\n", unstable, tiles);
    const std::size_t lo = std::size_t(std::count(out.begin(), out.end(), 0)),
                      hi = std::size_t(std::count(out.begin(), out.end(), 255));
    CHECK(lo + hi < out.size() / 20);
    if (rc.kind == model_kind::denoise) {
      const double before = psnr(noisy, clean), after = psnr(out, clean);
      std::printf("  noise: PSNR %.2f -> %.2f dB\n", before, after);
      CHECK(after > before + 3.0);
    } else if (unstable == tiles) {
      // The model gave no picture for any view of any tile (NAFNet-GoPro on
      // the synthetic scene): the photo comes back as it was, never noise.
      WARN(kind_name(rc.kind) << ": the model could not restore any tile; the photo was kept");
      CHECK(out == in);
    } else if (rc.scale == 1) {
      const double before = lap_var(in, w, h), after = lap_var(out, w, h);
      std::printf("  sharpness (Laplacian variance) %.1f -> %.1f; PSNR to the sharp original %.2f -> %.2f dB\n",
                  before, after, psnr(in, clean), psnr(out, clean));
      CHECK(after > before * 1.2);
    } else {
      // Enlarged: sharper than the same enlargement done plainly (Strength
      // 0 never uses the model's pixels: a stand-in runs the tiles).
      restore_options plain = o;
      plain.strength = 0;
      plain.progress = nullptr;
      plain.overlap = 8;
      nearest4 stand_in(32);
      std::vector<std::uint8_t> base;
      REQUIRE(restore_picture(in, w, h, stand_in, plain, base));
      const double before = lap_var(base, w * rc.scale, h * rc.scale), after = lap_var(out, w * rc.scale, h * rc.scale);
      std::printf("  sharpness at x%u (Laplacian variance) plain %.1f -> model %.1f\n", rc.scale, before, after);
      CHECK(after > before * 1.2);
    }
  }
#endif
}
