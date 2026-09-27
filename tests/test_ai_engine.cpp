// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The AI pack's engine end to end over the real host table (io walk, pairing)
// with deterministic stand-ins for the parts that need hardware or weights:
// pixels come from a file's NAME (a "red" photo, a clip that is red, then
// green, then blue), and the embedder turns a colour into a direction. What
// that proves is the machinery plan/17's PR 21-24 verify lines rest on:
// resume without redoing committed frames, stale detection, delta scans,
// dedupe, scope, per-clip grouping, "nothing found", migration without mixed
// vector spaces, clearing, the size cap, the yield policy, and people with
// their corrections and one-click deletion. The model numbers themselves
// (recall, throughput) are tests/test_ai_infer.cpp and tools/ai-bench.
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_ai.h>

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
#include <vector>

#include "addon/host.h"
#include "addons/ai/engine.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::ai::engine;

namespace {

constexpr std::uint32_t kDim = 8;

// A colour -> a unit vector. Channels 0-2 carry the colour, 3-7 a small bias
// every image shares (so the generic prompts sit near every image, as real
// CLIP's do).
std::vector<float> colour_vec(float r, float g, float b, float spice = 0) {
  std::vector<float> v{r, g, b, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f + spice};
  mv::infer::l2_normalise(v);
  return v;
}

struct rgb {
  std::uint8_t r, g, b;
};

rgb colour_named(const std::string& name) {
  if (name.find("red") != std::string::npos) return {220, 20, 20};
  if (name.find("green") != std::string::npos) return {20, 220, 20};
  if (name.find("blue") != std::string::npos) return {20, 20, 220};
  return {128, 128, 128};
}

mv::addon::rgb_image solid(rgb c, std::uint32_t w = 32, std::uint32_t h = 24) {
  mv::addon::rgb_image img;
  img.width = w;
  img.height = h;
  img.rgb.resize(static_cast<std::size_t>(w) * h * 3);
  for (std::size_t i = 0; i < img.rgb.size(); i += 3) {
    img.rgb[i] = c.r;
    img.rgb[i + 1] = c.g;
    img.rgb[i + 2] = c.b;
  }
  return img;
}

class fake_embedder final : public mv::infer::embedder {
 public:
  explicit fake_embedder(std::string key, float spice = 0) : key_(std::move(key)), spice_(spice) {}
  std::uint32_t dim() const noexcept override { return kDim; }
  const std::string& spec_key() const noexcept override { return key_; }
  mv::infer::backend on() const noexcept override { return mv::infer::backend::cpu; }
  mv::expected embed_images(std::span<const mv::infer::rgb_view> images, std::vector<float>& out) override {
    out.clear();
    for (const auto& img : images) {
      double r = 0, g = 0, b = 0;
      const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
      for (std::size_t i = 0; i < n; ++i) {
        r += img.rgb[i * 3];
        g += img.rgb[i * 3 + 1];
        b += img.rgb[i * 3 + 2];
      }
      const auto v = colour_vec(static_cast<float>(r / n / 255), static_cast<float>(g / n / 255),
                                static_cast<float>(b / n / 255), spice_);
      out.insert(out.end(), v.begin(), v.end());
      ++images_embedded;
    }
    return {};
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override {
    if (text.find("red") != std::string_view::npos) return colour_vec(1, 0, 0, spice_);
    if (text.find("green") != std::string_view::npos) return colour_vec(0, 1, 0, spice_);
    if (text.find("blue") != std::string_view::npos) return colour_vec(0, 0, 1, spice_);
    // Anything else (and the generic prompts): the shared bias direction.
    return colour_vec(0.3f, 0.3f, 0.3f, spice_);
  }
  std::atomic<int> images_embedded{0};

 private:
  std::string key_;
  float spice_;
};

// Faces from the file name: "anna_*" holds Anna's face, "ben_*" Ben's.
class fake_faces final : public mv::ai::face_analyzer {
 public:
  mv::result<std::vector<mv::ai::face_in>> analyze(const mv::ai::rgb_frame& img) override {
    std::vector<mv::ai::face_in> out;
    // The fake still encodes the person in the blue channel of pixel 0.
    if (img.rgb.size() < 3) return out;
    const std::uint8_t tag = img.rgb[2];
    if (tag != 11 && tag != 22) return out;
    mv::ai::face_in f;
    f.x = 0.25f;
    f.y = 0.2f;
    f.w = 0.3f;
    f.h = 0.4f;
    f.score = 0.95f;
    f.emb.assign(128, 0.0f);
    f.emb[tag == 11 ? 0 : 1] = 1.0f;
    f.emb[2] = 0.05f;
    mv::infer::l2_normalise(f.emb);
    out.push_back(std::move(f));
    return out;
  }
  const std::string& spec_key() const noexcept override { return key_; }
  float same_person() const noexcept override { return 0.4f; }
  std::uint32_t dim() const noexcept override { return 128; }

 private:
  std::string key_ = "fake-faces/1";
};

// A clip named "*_rgb.mp4": red for 3 s, green for 3 s, blue for 3 s, one
// keyframe a second. "*_slow*" sleeps per frame so a test can stop mid-clip.
class fake_sampler final : public mv::addon::video_sampler {
 public:
  fake_sampler(std::string name, std::int64_t start) : name_(std::move(name)), t_(start) {
    facts_.duration_ms = 9000;
    facts_.width = 64;
    facts_.height = 36;
    // Resume lands on the first keyframe at or after the start.
    t_ = (t_ + 999) / 1000 * 1000;
  }
  const mv::addon::video_facts& facts() const noexcept override { return facts_; }
  mv::result<mv::addon::sampled_frame> next() override {
    mv::addon::sampled_frame f;
    if (t_ >= facts_.duration_ms) {
      f.end = true;
      return f;
    }
    if (name_.find("slow") != std::string::npos) std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const rgb c = t_ < 3000 ? rgb{220, 20, 20} : (t_ < 6000 ? rgb{20, 220, 20} : rgb{20, 20, 220});
    f.image = solid(c, 64, 36);
    f.pts_ms = t_;
    f.pts_tb = t_ * 90;
    f.tb_num = 1;
    f.tb_den = 90000;
    f.keyframe = true;
    t_ += 1000;
    return f;
  }

 private:
  std::string name_;
  std::int64_t t_;
  mv::addon::video_facts facts_;
};

// Audio stand-ins (2026-09-27). A clip named "*_talk_bark*" has a soundtrack
// whose samples say what is in it: 0.1 while someone talks (0-10 s), 0.3
// while a dog barks (10-20 s), silence after (to 30 s).
class fake_audio final : public mv::addon::audio_stream {
 public:
  fake_audio(std::uint32_t rate, std::int64_t start_ms) : rate_(rate), t_ms_(start_ms) {}
  std::int64_t duration_ms() const noexcept override { return 30000; }
  mv::result<std::vector<float>> read(std::size_t max_samples, std::int64_t& start_ms) override {
    start_ms = t_ms_;
    std::vector<float> out;
    const std::int64_t end_ms = 30000;
    while (out.size() < max_samples && t_ms_ < end_ms) {
      const std::int64_t ms = t_ms_ + static_cast<std::int64_t>((pos_ % rate_) * 1000 / rate_);
      if (ms >= end_ms) break;
      out.push_back(ms < 10000 ? 0.1f : (ms < 20000 ? 0.3f : 0.0f));
      if (++pos_ % rate_ == 0) t_ms_ += 1000;
    }
    return out;
  }

 private:
  std::uint32_t rate_;
  std::int64_t t_ms_;
  std::uint64_t pos_ = 0;
};

// A clip named "*talk_bark*" has the soundtrack above; any other has none.
mv::result<std::unique_ptr<mv::addon::audio_stream>> open_fake_audio(const std::string& path, std::uint32_t rate,
                                                                    std::int64_t start_ms) {
  if (utf8(fs::path(path).filename()).find("talk_bark") == std::string::npos) {
    return mv::err(mv::status::unsupported_format);
  }
  std::unique_ptr<mv::addon::audio_stream> s = std::make_unique<fake_audio>(rate, start_ms);
  return s;
}

class fake_sound final : public mv::ai::sound_model {
 public:
  mv::expected embed_audio(std::span<const std::span<const float>> windows, std::vector<float>& out) override {
    out.clear();
    for (const auto& w : windows) {
      double sum = 0;
      for (float v : w) sum += v;
      const double mean = w.empty() ? 0 : sum / static_cast<double>(w.size());
      // Mostly bark -> the "dog" direction (blue channel here); else noise.
      const auto v = mean > 0.2 ? colour_vec(0, 0, 1) : colour_vec(0.3f, 0.3f, 0.3f);
      out.insert(out.end(), v.begin(), v.end());
      ++windows_embedded;
    }
    return {};
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override {
    if (text.find("dog") != std::string_view::npos || text.find("bark") != std::string_view::npos) {
      return colour_vec(0, 0, 1);
    }
    return colour_vec(0.3f, 0.3f, 0.3f);
  }
  std::atomic<int> windows_embedded{0};
};

class fake_speech final : public mv::ai::speech_model {
 public:
  mv::result<mv::infer::speech_window> transcribe(std::span<const float> pcm, std::int64_t start_ms) override {
    mv::infer::speech_window w;
    w.consumed_ms = static_cast<std::int64_t>(pcm.size()) * 1000 / 16000;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      if (std::fabs(pcm[i] - 0.1f) < 1e-4f) {
        w.segments.push_back({start_ms + static_cast<std::int64_t>(i / 16), start_ms + 10000,
                              "Happy birthday Anna, make a wish!"});
        w.speech = true;
        break;
      }
    }
    ++windows;
    return w;
  }
  std::atomic<int> windows{0};
};

struct rig {
  scratch_dir dir{"ai"};
  std::mutex events_m;
  std::vector<mv_addon_event> events;
  std::map<std::string, std::string> moment_thumbs;
  std::mutex thumbs_m;
  std::atomic<bool> busy{false};
  std::atomic<bool> on_battery{false};  // the fake power source
  std::atomic<int> battery_percent{100};
  std::atomic<int> stills_decoded{0};
  std::unique_ptr<mv::addon::host_table> table;
  std::shared_ptr<fake_embedder> fast = std::make_shared<fake_embedder>("fake-fast/fp16/pre1");
  std::shared_ptr<fake_embedder> high = std::make_shared<fake_embedder>("fake-high/fp16/pre1", 0.01f);
  bool faces_available = true;
  bool audio_available = false;
  std::shared_ptr<fake_sound> sound = std::make_shared<fake_sound>();
  std::shared_ptr<fake_speech> speech = std::make_shared<fake_speech>();
  std::unique_ptr<engine> eng;

  rig() {
    mv::addon::host_services svc;
    svc.post = [this](const mv_addon_event& e) {
      std::lock_guard lock(events_m);
      events.push_back(e);
    };
    svc.should_yield = [this] { return busy.load(); };
    svc.data_dir = utf8(dir / "data");
    svc.thumbnail = [](const std::string& path) -> mv::result<std::string> { return path + ".thumb.jpg"; };
    svc.still_rgb = [this](const std::string& path, std::uint32_t) -> mv::result<mv::addon::rgb_image> {
      ++stills_decoded;
      const std::string name = utf8(fs::path(path).filename());
      if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
      auto img = solid(colour_named(name));
      if (name.rfind("anna", 0) == 0) img.rgb[2] = 11;
      if (name.rfind("ben", 0) == 0) img.rgb[2] = 22;
      return img;
    };
    svc.open_sampler = [](const std::string& path, const mv::addon::sampler_options& o)
        -> mv::result<std::unique_ptr<mv::addon::video_sampler>> {
      return std::unique_ptr<mv::addon::video_sampler>(
          new fake_sampler(utf8(fs::path(path).filename()), o.start_ms));
    };
    svc.video_frame = [](const std::string& path, std::int64_t ms, std::uint32_t) -> mv::result<mv::addon::rgb_image> {
      fake_sampler s(utf8(fs::path(path).filename()), ms);
      MV_TRY(auto f, s.next());
      return f.image;
    };
    svc.moment_thumbnail = [this](const std::string& path, std::int64_t ms,
                                  const mv::addon::rgb_image* img) -> mv::result<std::string> {
      std::lock_guard lock(thumbs_m);
      const std::string key = path + "#" + std::to_string(ms);
      if (img) {
        moment_thumbs[key] = key + ".jpg";
        return moment_thumbs[key];
      }
      auto it = moment_thumbs.find(key);
      if (it == moment_thumbs.end()) return mv::err(mv::status::io);
      return it->second;
    };
    svc.piece_dir = [](const std::string&) -> mv::result<std::string> { return mv::err(mv::status::io); };
    svc.open_audio = &open_fake_audio;
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    table->set_negotiated(MV_ADDON_HOST_API);
  }
  ~rig() { eng.reset(); }

  mv::ai::engine_deps deps() {
    mv::ai::engine_deps d;
    d.qualities = [] { return std::vector<std::uint32_t>{1, 2}; };
    d.model_name = [](std::uint32_t q) { return q == 1 ? std::string("Fake fast") : std::string("Fake high"); };
    d.backend_available = [](mv::infer::backend) { return false; };
    d.open_clip = [this](std::uint32_t quality, std::uint32_t) -> mv::result<mv::ai::loaded_clip> {
      mv::ai::loaded_clip c;
      c.model = quality == 2 ? std::static_pointer_cast<mv::infer::embedder>(high)
                             : std::static_pointer_cast<mv::infer::embedder>(fast);
      c.meta.name = quality == 2 ? "Fake high" : "Fake fast";
      c.meta.spec_key = c.model->spec_key();
      c.meta.quality = quality;
      c.meta.dim = kDim;
      c.meta.input_edge = 16;
      c.meta.dedupe = 0.97f;
      c.meta.query_margin = 0.04f;
      c.meta.result_margin = 0.015f;
      c.meta.similar_min = 0.9f;
      c.meta.generic_prompts = {"a photo.", "an image."};
      return c;
    };
    d.open_faces = [this]() -> mv::result<std::unique_ptr<mv::ai::face_analyzer>> {
      if (!faces_available) return mv::err(mv::status::io);
      return std::unique_ptr<mv::ai::face_analyzer>(new fake_faces());
    };
    d.runtime_version = [] { return std::string("fake"); };
    d.power = [this] {
      mv::ai::platform::power p;
      p.on_battery = on_battery.load();
      p.percent = battery_percent.load();
      return p;
    };
    d.open_sound = [this](std::uint32_t) -> mv::result<mv::ai::loaded_sound> {
      if (!audio_available) return mv::err(mv::status::io);
      mv::ai::loaded_sound s;
      s.model = sound;
      s.name = "Fake CLAP";
      s.spec_key = "fake-clap/fp16/pre1";
      s.dim = kDim;
      s.window_ms = 10000;
      s.hop_ms = 5000;
      s.generic_prompts = {"a sound."};
      return s;
    };
    d.open_speech = [this](std::uint32_t, std::uint32_t) -> mv::result<mv::ai::loaded_speech> {
      if (!audio_available) return mv::err(mv::status::io);
      mv::ai::loaded_speech s;
      s.model = speech;
      s.name = "Fake Whisper";
      s.spec_key = "fake-whisper/fp16/pre1";
      return s;
    };
    return d;
  }

  void start() {
    eng.reset();
    eng = std::make_unique<engine>(table->api(), deps());
    REQUIRE(eng->start());
  }

  fs::path photos() const { return dir / "Photos"; }
  void file(const std::string& rel, std::size_t size = 16) { write_bytes(photos() / rel, pattern(size, 7)); }

  mv_ai_status status() const {
    mv_ai_status s{};
    eng->status(s);
    return s;
  }

  // Runs a text search to completion and returns (path, pts) pairs.
  std::vector<std::pair<std::string, std::int64_t>> search(const std::string& q, std::uint32_t scope = MV_AI_SCOPE_ALL,
                                                           const std::string& dir_scope = "",
                                                           std::uint32_t kinds = MV_AI_KIND_ALL) {
    const std::uint64_t id = eng->search_text(q, dir_scope, scope, kinds);
    REQUIRE(eng->wait_search(id, 5000));
    return rows(id);
  }
  std::vector<std::pair<std::string, std::int64_t>> rows(std::uint64_t id) {
    std::vector<std::pair<std::string, std::int64_t>> out;
    auto n = eng->result_count(id);
    REQUIRE(n);
    for (std::uint32_t i = 0; i < *n; ++i) {
      auto r = eng->result_at(id, i);
      auto p = eng->result_path(id, i);
      REQUIRE(r);
      REQUIRE(p);
      out.push_back({utf8(fs::path(*p).filename()), r->pts_ms});
    }
    return out;
  }
  bool idle(int ms = 15000) { return eng->wait_idle(ms); }
};

}  // namespace

TEST_CASE("a remembered folder indexes in the background and answers a description", "[ai][engine]") {
  rig r;
  r.file("red_car.jpg");
  r.file("green_field.jpg");
  r.file("blue_sea.jpg");
  r.file("notes.txt");
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos()), false);
  REQUIRE(root);
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK(s.state == MV_AI_STATE_IDLE);
  CHECK(s.assets_total == 3);  // the .txt is not media
  CHECK(s.assets_done == 3);
  CHECK(s.frames_indexed == 3);

  const auto red = r.search("something red");
  REQUIRE_FALSE(red.empty());
  CHECK(red.front().first == "red_car.jpg");
  CHECK(red.front().second == -1);
  // plan/17: a nonsense query says "nothing found", not the least-bad ten.
  CHECK(r.search("xyzzy plugh").empty());
  CHECK(r.eng->folder_coverage(utf8(r.photos())) == 2u);
  CHECK(r.eng->folder_coverage(utf8(r.dir / "Elsewhere")) == 0u);
}

TEST_CASE("a clip keeps one row per distinct moment and groups its matches", "[ai][engine]") {
  rig r;
  r.file("holiday_rgb.mp4");
  r.file("red_apple.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  // Nine keyframes, three colours: dedupe keeps the first of each run.
  CHECK(r.status().frames_indexed == 1 + 3);

  const auto green = r.search("green");
  REQUIRE(green.size() == 1);
  CHECK(green.front().first == "holiday_rgb.mp4");
  CHECK(green.front().second == 3000);  // the moment, not the clip's start

  const std::uint64_t id = r.eng->search_text("red", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, 5000));
  const auto rows = r.rows(id);
  REQUIRE(rows.size() == 2);  // the photo and the clip: one row per asset
  auto m = r.eng->clip_matches(id, utf8(r.photos() / "holiday_rgb.mp4"));
  REQUIRE(m);
  REQUIRE(m->size() == 1);
  CHECK(m->front().first == 0);
  // The tile for a moment is the viewer's cache entry for that moment.
  for (std::uint32_t i = 0; i < rows.size(); ++i) {
    auto t = r.eng->result_thumb(id, i);
    REQUIRE(t);
    if (rows[i].first == "holiday_rgb.mp4") {
      CHECK(t->find("#0") != std::string::npos);
    }
  }
  // Kind filter.
  CHECK(r.search("red", MV_AI_SCOPE_ALL, "", MV_AI_KIND_PHOTOS).size() == 1);
  CHECK(r.search("red", MV_AI_SCOPE_ALL, "", MV_AI_KIND_VIDEOS).size() == 1);
}

TEST_CASE("killing mid-clip resumes without redoing committed frames", "[ai][engine]") {
  rig r;
  r.file("long_slow_rgb.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  // Wait for the first batch to commit, then stop (the app quitting).
  for (int i = 0; i < 400 && r.fast->images_embedded.load() < 8; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  r.eng->stop();
  const int first_run = r.fast->images_embedded.load();
  REQUIRE(first_run >= 8);
  REQUIRE(first_run < 9 + 1);
  r.start();
  REQUIRE(r.idle());
  // Nine keyframes in all: the second run embeds only what was not committed.
  CHECK(r.fast->images_embedded.load() == 9);
  CHECK(r.status().assets_done == 1);
  CHECK(r.status().frames_indexed == 3);
}

TEST_CASE("an edited file is re-queued and a reopened tree queues only its delta", "[ai][engine]") {
  rig r;
  r.file("a/red_1.jpg");
  r.file("a/b/green_2.jpg");
  r.file("c/blue_3.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 3);
  CHECK(r.search("green").size() == 1);

  // Restart (the app reopened): nothing new, nothing redone.
  r.start();
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 3);

  // One new file and one edited file: exactly two decodes.
  r.file("c/red_4.jpg");
  r.file("a/red_1.jpg", 64);  // new size: the old vectors describe other pixels
  r.eng->note_folder_opened(utf8(r.photos() / "a"));
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 5);
  CHECK(r.status().assets_total == 4);
  CHECK(r.search("red").size() == 2);

  // A removed file drops out of results.
  fs::remove(r.photos() / "c/red_4.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  CHECK(r.search("red").size() == 1);
}

TEST_CASE("scope limits results to the folder or the folder and below", "[ai][engine]") {
  rig r;
  r.file("red_top.jpg");
  r.file("sub/red_below.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK(r.search("red", MV_AI_SCOPE_FOLDER, utf8(r.photos())).size() == 1);
  CHECK(r.search("red", MV_AI_SCOPE_TREE, utf8(r.photos())).size() == 2);
  CHECK(r.search("red", MV_AI_SCOPE_FOLDER, utf8(r.photos() / "sub")).size() == 1);
  CHECK(r.search("red", MV_AI_SCOPE_ALL).size() == 2);
}

TEST_CASE("a folder already covered by a tree root is not remembered twice", "[ai][engine]") {
  rig r;
  r.file("x/red.jpg");
  r.start();
  auto sub = r.eng->index_folder(utf8(r.photos() / "x"), false);
  REQUIRE(sub);
  auto tree = r.eng->index_folder(utf8(r.photos()), true);
  REQUIRE(tree);
  auto again = r.eng->index_folder(utf8(r.photos() / "x"), false);
  REQUIRE(again);
  CHECK(*again == *tree);
  REQUIRE(r.idle());
  const auto roots = mv::json::parse(r.eng->roots_json());
  REQUIRE(roots);
  CHECK(roots->a.size() == 1);  // the tree swallowed the folder root
}

TEST_CASE("indexing waits while the viewer is busy and says so", "[ai][engine]") {
  rig r;
  r.file("red.jpg");
  r.busy = true;
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.stills_decoded.load() == 0);
  // Not even the models open while the viewer is busy (they cost frames).
  const mv_ai_status s = r.status();
  CHECK(s.state == MV_AI_STATE_LOADING);
  CHECK(s.yield_reason == MV_AI_YIELD_VIEWER);
  r.busy = false;
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 1);

  // Loaded, then busy again: the indexer yields and says so.
  r.busy = true;
  r.file("blue.jpg");
  REQUIRE(r.eng->root_rescan(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.stills_decoded.load() == 1);
  const mv_ai_status y = r.status();
  CHECK(y.state == MV_AI_STATE_YIELDING);
  CHECK(y.yield_reason == MV_AI_YIELD_VIEWER);
  r.busy = false;
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 2);

  r.eng->pause(true);
  r.file("green.jpg");
  REQUIRE(r.eng->root_rescan(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.stills_decoded.load() == 2);
  CHECK(r.status().state == MV_AI_STATE_PAUSED);
  r.eng->pause(false);
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 3);
}

TEST_CASE("indexing pauses on a low battery until the user says index anyway", "[ai][engine]") {
  rig r;
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());  // models loaded on AC
  REQUIRE(r.stills_decoded.load() == 1);

  // Unplugged below the threshold (30 %): the indexer waits and says why.
  r.on_battery = true;
  r.battery_percent = 12;
  r.file("blue.jpg");
  REQUIRE(r.eng->root_rescan(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.stills_decoded.load() == 1);
  const mv_ai_status s = r.status();
  CHECK(s.state == MV_AI_STATE_YIELDING);
  CHECK(s.yield_reason == MV_AI_YIELD_BATTERY);

  // "Index anyway": the session override, reported but never saved.
  REQUIRE(r.eng->set_setting("battery_override", "1"));
  CHECK(r.eng->settings_json().find("\"battery_override\":true") != std::string::npos);
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 2);
  REQUIRE(r.eng->set_setting("index_cap_bytes", "8000000000"));  // writes settings.json
  {
    std::ifstream in(r.dir / "data" / "settings.json", std::ios::binary);
    REQUIRE(in);
    std::ostringstream saved;
    saved << in.rdbuf();
    CHECK(saved.str().find("battery_override") == std::string::npos);
  }
  CHECK(r.eng->settings_json().find("\"pause_on_battery_percent\":30") != std::string::npos);

  // Plugged in and out again: the override has ended and the pause is back.
  r.on_battery = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.eng->settings_json().find("\"battery_override\":false") != std::string::npos);
  r.on_battery = true;
  r.file("green.jpg");
  REQUIRE(r.eng->root_rescan(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r.stills_decoded.load() == 2);
  CHECK(r.status().yield_reason == MV_AI_YIELD_BATTERY);

  // A restart forgets an override too.
  REQUIRE(r.eng->set_setting("battery_override", "1"));
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 3);
  r.start();
  CHECK(r.eng->settings_json().find("\"battery_override\":false") != std::string::npos);

  // Above the threshold on battery, nothing waits.
  r.battery_percent = 80;
  r.file("white.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 4);
}

TEST_CASE("a quality change migrates without ever mixing vector spaces", "[ai][engine]") {
  rig r;
  r.file("red.jpg");
  r.file("green.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(r.eng->active_spec() == "fake-fast/fp16/pre1");

  REQUIRE(r.eng->set_setting("quality", "2"));
  // Until the new tower has embedded everything, queries use the old one.
  for (int i = 0; i < 300 && r.eng->active_spec() != "fake-high/fp16/pre1"; ++i) {
    CHECK((r.eng->active_spec() == "fake-fast/fp16/pre1" || r.eng->active_spec() == "fake-high/fp16/pre1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(r.idle());
  for (int i = 0; i < 300 && r.eng->active_spec() != "fake-high/fp16/pre1"; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(r.eng->active_spec() == "fake-high/fp16/pre1");
  CHECK(r.high->images_embedded.load() == 2);
  CHECK(r.status().frames_indexed == 2);  // the old rows went; nothing doubled
  CHECK(r.search("green").front().first == "green.jpg");
}

TEST_CASE("clearing the index frees it and a size cap stops indexing", "[ai][engine]") {
  rig r;
  for (int i = 0; i < 40; ++i) r.file("red_" + std::to_string(i) + ".jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(r.status().frames_indexed == 40);
  REQUIRE(r.eng->clear_index());
  CHECK(r.status().frames_indexed == 0);
  CHECK(r.search("red").empty());
  const auto roots = mv::json::parse(r.eng->roots_json());
  REQUIRE(roots);
  CHECK(roots->a.empty());
  // Thumbnails belong to the viewer's cache: clearing never touches them.
  CHECK(r.moment_thumbs.size() == r.moment_thumbs.size());

  REQUIRE(r.eng->set_setting("index_cap_bytes", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  for (int i = 0; i < 200 && !(r.status().flags & MV_AI_STATUS_INDEX_FULL); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  CHECK((r.status().flags & MV_AI_STATUS_INDEX_FULL) != 0);
  CHECK(r.status().state == MV_AI_STATE_PAUSED);
}

TEST_CASE("find similar returns the other stills of the same look, not the query", "[ai][engine]") {
  rig r;
  r.file("red_a.jpg");
  r.file("red_b.jpg");
  r.file("blue_c.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const std::uint64_t id =
      r.eng->search_similar(utf8(r.photos() / "red_a.jpg"), -1, "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, 5000));
  const auto rows = r.rows(id);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "red_b.jpg");
}

TEST_CASE("people: opt-in, clusters, names, corrections, and deletion that leaves no vectors",
          "[ai][engine][faces]") {
  rig r;
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.file("anna_3.jpg");
  r.file("ben_1.jpg");
  r.file("ben_2.jpg");
  r.file("red_nobody.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(mv::json::parse(r.eng->people_json())->a.empty());  // off until the opt-in

  REQUIRE(r.eng->faces_enable(true));
  for (int i = 0; i < 300 && !(r.status().flags & MV_AI_STATUS_FACES_READY); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 2);
  const std::int64_t anna = *people->a[0].integer("id");
  const std::int64_t ben = *people->a[1].integer("id");
  CHECK(*people->a[0].integer("faces") == 3);
  REQUIRE(r.eng->person_rename(anna, "Anna"));

  auto photos_of = r.search("photos of Anna");
  CHECK(photos_of.size() == 3);
  // A face thumbnail comes from the viewer's cache, never a crop on disk.
  auto faces = mv::json::parse(r.eng->person_faces_json(anna));
  REQUIRE(faces);
  REQUIRE(faces->a.size() == 3);
  auto thumb = r.eng->face_thumb(*faces->a[0].integer("face"));
  REQUIRE(thumb);
  CHECK(thumb->find(".thumb.jpg") != std::string::npos);

  // "Not this person" moves a face out for good; split makes a new person.
  REQUIRE(r.eng->face_reject(*faces->a[0].integer("face")));
  CHECK(mv::json::parse(r.eng->person_faces_json(anna))->a.size() == 2);
  const auto ben_faces = mv::json::parse(r.eng->person_faces_json(ben));
  REQUIRE(ben_faces);
  const std::vector<std::int64_t> one{*ben_faces->a[0].integer("face")};
  auto split = r.eng->face_split(one);
  REQUIRE(split);
  CHECK(*split != ben);
  // Corrections persist across a restart.
  r.start();
  REQUIRE(r.idle());
  CHECK(mv::json::parse(r.eng->person_faces_json(anna))->a.size() == 2);

  // Off deletes every face vector: the file is gone, the frame index stays.
  REQUIRE(r.eng->faces_enable(false));
  CHECK_FALSE(fs::exists(r.dir / "data" / "faces.db"));
  CHECK_FALSE(fs::exists(r.dir / "data" / "faces.db-wal"));
  CHECK(r.search("red").size() == 1);
  CHECK(r.status().frames_indexed == 6);
}

TEST_CASE("a file the host cannot decode fails without stopping the rest", "[ai][engine]") {
  rig r;
  r.file("broken_red.jpg");
  r.file("red_fine.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK(s.assets_done == 1);
  CHECK(s.assets_failed == 1);
  CHECK(r.search("red").size() == 1);
}

TEST_CASE("audio: a clip's sounds and speech are indexed and found at their moments", "[ai][engine][audio]") {
  rig r;
  r.audio_available = true;
  r.file("party_talk_bark.mp4");
  r.file("quiet_rgb.mp4");  // no soundtrack: sound and speech finish, nothing fails
  r.file("red_photo.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle(30000));
  const mv_ai_status s = r.status();
  CHECK((s.flags & MV_AI_STATUS_AUDIO_READY) != 0);
  CHECK(s.sound_total == 2);   // videos only: photos have no soundtrack
  CHECK(s.sound_done == 2);
  CHECK(s.speech_total == 2);
  CHECK(s.speech_done == 2);
  CHECK(s.assets_failed == 0);

  // What it sounds like: the bark window (10-20 s), not the talk before it.
  const std::uint64_t bark = r.eng->search_text("a dog barking", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(bark, 5000));
  auto rows = r.rows(bark);
  REQUIRE_FALSE(rows.empty());
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(rows.front().second >= 5000);
  CHECK(rows.front().second <= 15000);
  CHECK((r.eng->result_at(bark, 0)->match & MV_AI_MATCH_SOUND) != 0);

  // What is said: the words, with a snippet, at the moment they start.
  const std::uint64_t said = r.eng->search_text("birthday anna", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(said, 5000));
  rows = r.rows(said);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(rows.front().second == 0);
  CHECK(r.eng->result_at(said, 0)->match == MV_AI_MATCH_SPEECH);
  CHECK(r.eng->result_snippet(said, 0)->find("birthday") != std::string::npos);

  // "Find in" narrows: pictures only never answers from the soundtrack.
  const std::uint64_t pics =
      r.eng->search_text("birthday anna", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL | MV_AI_FIND_PICTURES);
  REQUIRE(r.eng->wait_search(pics, 5000));
  CHECK(r.rows(pics).empty());

  // Photos still answer as pictures alongside.
  CHECK(r.search("red").front().first == "red_photo.jpg");
}

TEST_CASE("audio: a folder indexed for pictures only does no audio work", "[ai][engine][audio]") {
  rig r;
  r.audio_available = true;
  r.file("party_talk_bark.mp4");
  r.start();
  REQUIRE(r.eng->set_setting("video_index", "1"));  // Pictures
  auto root = r.eng->index_folder(utf8(r.photos()), false);
  REQUIRE(root);
  REQUIRE(r.idle(30000));
  CHECK(r.sound->windows_embedded.load() == 0);
  CHECK(r.speech->windows.load() == 0);
  CHECK(r.status().sound_total == 0);

  // Switching this folder to Both queues its sound and speech.
  REQUIRE(r.eng->root_set_media(*root, MV_AI_MEDIA_BOTH));
  REQUIRE(r.idle(30000));
  CHECK(r.sound->windows_embedded.load() > 0);
  CHECK(r.speech->windows.load() > 0);
  CHECK(r.status().sound_done == 1);
  const auto roots = mv::json::parse(r.eng->roots_json());
  REQUIRE(roots);
  CHECK(*roots->a[0].integer("media") == MV_AI_MEDIA_BOTH);
}

TEST_CASE("audio: without the ai-audio piece nothing changes", "[ai][engine][audio]") {
  rig r;
  r.file("party_talk_bark.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle(30000));
  CHECK((r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0);
  CHECK(r.status().sound_total == 0);
  CHECK(r.search("birthday anna").empty());
}
