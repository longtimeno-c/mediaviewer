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

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addons/ai/sha256.h"
#include "addons/enhance/engine.h"
#include "addons/enhance/picture_job.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/sound_job.h"
#include "addons/enhance/stream_model.h"
#include "addons/enhance/screens.h"
#include "addons/enhance/sniff.h"
#include "addons/enhance/spec.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::enhance;
using mv::test::scratch_dir;
using mv::test::utf8;
using mv::test::write_bytes;
using mv::test::write_text;

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
    p.show = [this](const char* screen, bool show) {
      std::lock_guard lock(host.m);
      host.shown[screen] = show ? 1 : 0;
    };
    p.probe = [this](const std::string&) { return host.facts; };
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
    CHECK(*t.node("run")->boolean("disabled"));
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
    CHECK(*t.node("run")->boolean("disabled"));
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
    CHECK(*t.node("run")->boolean("disabled"));
  }

  // A document: always greyed.
  f.eng->sheet_opened(10, utf8(doc));
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(t.text("note").find("not on documents") != std::string::npos);
    CHECK(*t.node("run")->boolean("disabled"));
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
  port.open_sound = [&](const std::string& path, std::uint32_t rate) -> mv::result<run_port::reader> {
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
    CHECK_FALSE(*t.node("run")->boolean("disabled"));
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
  CHECK_FALSE(*parsed(f.host.tree(kSheetScreen)).node("run")->boolean("disabled"));

  // A clip with both: Run is greyed until its picture can be enhanced too.
  f.eng->event(kSettingsScreen, "install.video", MV_SLOT_EVENT_CLICK, "");
  REQUIRE(eventually([&] { return f.eng->download_status(model_kind::video).state == dl_state::ready; }));
  const fs::path clip = f.dir / "clip.mov";
  write_bytes(clip, ftyp("qt  ", "qt  "));
  f.eng->sheet_opened(2, utf8(clip));
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"both\"");
  {
    parsed t(f.host.tree(kSheetScreen));
    CHECK(*t.node("run")->boolean("disabled"));
    CHECK(t.text("status").find("clip's picture") != std::string::npos);
  }
  // Its sound alone runs.
  f.eng->event(kSheetScreen, "target", MV_SLOT_EVENT_CHANGE, "\"sound\"");
  CHECK_FALSE(*parsed(f.host.tree(kSheetScreen)).node("run")->boolean("disabled"));
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
  for (const auto [w, h] : {std::pair{70u, 45u}, std::pair{32u, 32u}, std::pair{101u, 7u}}) {
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
  run_port port;
  port.open_sound = [&](const std::string&, std::uint32_t) -> mv::result<run_port::reader> {
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
  port.open_frames = [&](const std::string&) -> mv::result<run_port::picture_reader> {
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
    CHECK_FALSE(*t.node("run")->boolean("disabled"));
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
}
