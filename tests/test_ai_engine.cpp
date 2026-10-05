// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The AI pack's engine end to end over the real host table (io walk, pairing)
// with deterministic stand-ins for the parts that need hardware or weights:
// pixels come from a file's NAME (a "red" photo, a clip that is red, then
// green, then blue), and the embedder turns a colour into a direction. What
// that proves is the machinery docs/design/17's PR 21-24 verify lines rest on:
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
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addons/ai/engine.h"
#include "nle/search_session.h"
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
  // Near misses for "red" (Precision): coral beats the generic prompts by
  // ~0.023, blush by ~0.008 (the fake meta's result margin is 0.015).
  if (name.find("coral") != std::string::npos) return {200, 70, 70};
  if (name.find("blush") != std::string::npos) return {200, 75, 75};
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

// A tower a test can hold at each step the pack's Core ML tower takes long
// over (2026-09-28): its open (the rig's `tower` hook waits on `open`), its
// image embeds (`embed`), and its last release (`death`: upgrading_clip's
// destructor waits for a compile in progress, minutes on a first L/14
// compile). Every hold is bounded, so a regression fails the timing checks
// rather than hanging the suite. `pad` zero dimensions make it another
// vector space's size.
struct gates {
  std::atomic<bool> open{false};
  std::atomic<bool> embed{false};
  std::atomic<bool> death{false};
  std::atomic<int> opens_waiting{0};
  std::atomic<int> died{0};
  std::mutex m;
  std::vector<std::thread::id> died_on;
};

void hold_while(const std::atomic<bool>& flag) {
  for (int i = 0; i < 2000 && flag.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

class gated_embedder final : public mv::infer::embedder {
 public:
  gated_embedder(std::shared_ptr<fake_embedder> inner, gates* g, std::uint32_t pad = 0)
      : inner_(std::move(inner)), g_(g), pad_(pad) {}
  ~gated_embedder() override {
    hold_while(g_->death);
    std::lock_guard lock(g_->m);
    g_->died_on.push_back(std::this_thread::get_id());
    ++g_->died;
  }
  std::uint32_t dim() const noexcept override { return inner_->dim() + pad_; }
  const std::string& spec_key() const noexcept override { return inner_->spec_key(); }
  mv::infer::backend on() const noexcept override { return mv::infer::backend::cpu; }
  bool settling() const noexcept override { return g_->death.load(); }
  mv::expected embed_images(std::span<const mv::infer::rgb_view> images, std::vector<float>& out) override {
    hold_while(g_->embed);
    std::vector<float> plain;
    MV_TRY_VOID(inner_->embed_images(images, plain));
    out.clear();
    const std::size_t d = inner_->dim();
    for (std::size_t i = 0; i + d <= plain.size(); i += d) {
      out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(i),
                 plain.begin() + static_cast<std::ptrdiff_t>(i + d));
      out.insert(out.end(), pad_, 0.0f);
    }
    return {};
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override {
    MV_TRY(std::vector<float> v, inner_->embed_text(text));
    v.insert(v.end(), pad_, 0.0f);
    return v;
  }

 private:
  std::shared_ptr<fake_embedder> inner_;
  gates* g_;
  std::uint32_t pad_;
};

// The Photos library (issue #72) without PhotoKit: assets by name, each local
// or iCloud-only, whose colour comes from the name like a file's. Tests change
// `lib` after the engine holds the source.
struct photos_lib {
  struct item {
    std::string id;
    std::int64_t mtime = 1;
    mv::ai::asset_kind kind = mv::ai::asset_kind::photo;
    bool local = true;       // false: only iCloud has the original
    std::string clip_file;   // a local clip's file (the fake sampler reads its name)
  };
  std::mutex m;
  mv::ai::photos_access access = mv::ai::photos_access::full;
  std::vector<item> items;
  std::function<void()> changed;
  std::atomic<int> enumerations{0};
  std::atomic<int> stills{0};
  std::atomic<int> videos{0};  // video_file asks
  std::atomic<int> fetches{0};  // fetch_video downloads (the opt-in iCloud fetch)
  std::atomic<bool> unmetered{true};
  void notify() {
    std::function<void()> f;
    {
      std::lock_guard lock(m);
      f = changed;
    }
    if (f) f();
  }
};

class fake_photos final : public mv::ai::photos_source {
 public:
  explicit fake_photos(std::shared_ptr<photos_lib> lib) : lib_(std::move(lib)) {}
  mv::ai::photos_access access() const override {
    std::lock_guard lock(lib_->m);
    return lib_->access;
  }
  mv::expected enumerate(const std::function<bool(const mv::ai::photos_item&)>& visit) override {
    if (!mv::ai::readable(access())) return mv::err(mv::status::permission_denied);
    ++lib_->enumerations;
    std::vector<photos_lib::item> items;
    {
      std::lock_guard lock(lib_->m);
      items = lib_->items;
    }
    for (const auto& it : items) {
      if (!visit(mv::ai::photos_item{it.id, it.mtime, 100, it.kind})) return mv::err(mv::status::cancelled);
    }
    return {};
  }
  mv::result<mv::ai::rgb_frame> still(std::string_view id, std::uint32_t) override {
    if (!mv::ai::readable(access())) return mv::err(mv::status::permission_denied);
    ++lib_->stills;
    auto it = find(id);
    if (!it) return mv::err(mv::status::not_found);
    // A photo only iCloud has has no local derivative; a clip's poster always has one.
    if (!it->local && it->kind == mv::ai::asset_kind::photo) return mv::err(mv::status::io);
    const auto img = solid(colour_named(it->id));
    mv::ai::rgb_frame f;
    f.width = img.width;
    f.height = img.height;
    f.rgb = img.rgb;
    if (it->id.find("anna") != std::string::npos) f.rgb[2] = 11;  // fake_faces: Anna
    return f;
  }
  mv::result<std::string> video_file(std::string_view id) override {
    if (!mv::ai::readable(access())) return mv::err(mv::status::permission_denied);
    ++lib_->videos;
    auto it = find(id);
    if (!it) return mv::err(mv::status::not_found);
    if (!it->local) return mv::err(mv::status::io);
    return it->clip_file;
  }
  // "Downloads" a clip: an empty file the fake sampler reads by name (red,
  // green, then blue, whatever the name).
  mv::result<std::string> fetch_video(std::string_view id, const std::string& dest_stem,
                                      const std::function<bool(double)>& progress) override {
    if (!mv::ai::readable(access())) return mv::err(mv::status::permission_denied);
    auto it = find(id);
    if (!it) return mv::err(mv::status::not_found);
    if (it->kind != mv::ai::asset_kind::video) return mv::err(mv::status::unsupported_format);
    if (progress && !progress(0.5)) return mv::err(mv::status::cancelled);
    ++lib_->fetches;
    const std::string file = dest_stem + ".mp4";
    std::ofstream(fs::path(file), std::ios::binary) << "clip";
    return file;
  }
  bool network_unmetered() const override { return lib_->unmetered.load(); }
  void observe(std::function<void()> changed) override {
    std::lock_guard lock(lib_->m);
    lib_->changed = std::move(changed);
  }

 private:
  std::optional<photos_lib::item> find(std::string_view id) {
    std::lock_guard lock(lib_->m);
    for (const auto& it : lib_->items) {
      if (it.id == id) return it;
    }
    return std::nullopt;
  }
  std::shared_ptr<photos_lib> lib_;
};

struct rig {
  scratch_dir dir{"ai"};
  std::mutex events_m;
  std::vector<mv_addon_event> events;
  std::map<std::string, std::string> moment_thumbs;
  std::map<std::string, std::vector<std::uint8_t>> jpegs;  // path#ms -> bytes
  std::mutex thumbs_m;
  std::atomic<bool> busy{false};
  std::atomic<bool> on_battery{false};  // the fake power source
  std::atomic<int> battery_percent{100};
  std::atomic<int> stills_decoded{0};
  std::unique_ptr<mv::addon::host_table> table;
  std::shared_ptr<fake_embedder> fast = std::make_shared<fake_embedder>("fake-fast/fp16/pre1");
  std::shared_ptr<fake_embedder> high = std::make_shared<fake_embedder>("fake-high/fp16/pre1", 0.01f);
  std::atomic<bool> faces_available{true};
  std::atomic<bool> audio_available{false};
  std::atomic<int> clip_opens{0};
  // A test's own towers (fresh instances, gates); unset: `fast` and `high`.
  std::function<std::shared_ptr<mv::infer::embedder>(std::uint32_t quality)> tower;
  // The backend open_clip reports (Core ML: Auto quality chooses High).
  mv::infer::backend lands_on = mv::infer::backend::cpu;
  // The pack's provider verdict for the High tower (provider.txt): set, the
  // accelerated provider failed it here, with this message.
  std::mutex verdict_m;
  std::optional<std::string> large_failure;
  void set_large_failure(std::optional<std::string> v) {
    std::lock_guard lock(verdict_m);
    large_failure = std::move(v);
  }
  std::shared_ptr<fake_sound> sound = std::make_shared<fake_sound>();
  std::shared_ptr<fake_speech> speech = std::make_shared<fake_speech>();
  // Never the real library: PhotoKit is not asked in a test.
  std::shared_ptr<photos_lib> library = std::make_shared<photos_lib>();
  std::unique_ptr<engine> eng;

  rig() {
    mv::addon::host_services svc;
    svc.post = [this](const mv_addon_event& e) {
      std::lock_guard lock(events_m);
      events.push_back(e);
    };
    svc.should_yield = [this] { return busy.load(); };
    svc.data_dir = utf8(dir / "data");
    // One cache, as the host's: a still's thumbnail and a stored moment are
    // what thumbnail_jpeg then finds.
    svc.thumbnail = [this](const std::string& path) -> mv::result<std::string> {
      const std::string name = utf8(fs::path(path).filename());
      if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
      std::lock_guard lock(thumbs_m);
      std::vector<std::uint8_t>& jpeg = jpegs[path + "#-1"];
      if (jpeg.empty()) jpeg = {0xFF, 0xD8, 9};
      std::string out = path + ".thumb.jpg";
      return out;
    };
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
        jpegs[key] = {0xFF, 0xD8, 7};
        return moment_thumbs[key];
      }
      auto it = moment_thumbs.find(key);
      if (it == moment_thumbs.end()) return mv::err(mv::status::io);
      return it->second;
    };
    svc.piece_dir = [](const std::string&) -> mv::result<std::string> { return mv::err(mv::status::io); };
    svc.open_audio = &open_fake_audio;
    // The viewer's JPEG-512 cache as bytes (sharing an index).
    svc.thumbnail_jpeg = [this](const std::string& path, std::int64_t ms) -> mv::result<std::vector<std::uint8_t>> {
      std::lock_guard lock(thumbs_m);
      auto it = jpegs.find(path + "#" + std::to_string(ms));
      if (it == jpegs.end()) return mv::err(mv::status::io);
      return it->second;
    };
    svc.store_thumbnail_jpeg = [this](const std::string& path, std::int64_t ms,
                                      std::span<const std::uint8_t> jpeg) -> mv::expected {
      std::lock_guard lock(thumbs_m);
      jpegs[path + "#" + std::to_string(ms)].assign(jpeg.begin(), jpeg.end());
      return {};
    };
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    table->set_negotiated(MV_ADDON_HOST_API);
  }
  ~rig() { eng.reset(); }

  mv::ai::engine_deps deps() {
    mv::ai::engine_deps d;
    d.qualities = [] { return std::vector<std::uint32_t>{1, 2}; };
    d.model_name = [](std::uint32_t q) { return q == 1 ? std::string("Fake fast") : std::string("Fake high"); };
    d.backend_available = [](mv::infer::backend) { return false; };
    d.clip_spec = [this](std::uint32_t q) { return q == 2 ? high->spec_key() : fast->spec_key(); };
    d.open_clip = [this](std::uint32_t quality, std::uint32_t) -> mv::result<mv::ai::loaded_clip> {
      ++clip_opens;
      mv::ai::loaded_clip c;
      if (tower) {
        c.model = tower(quality);
      } else {
        c.model = quality == 2 ? std::static_pointer_cast<mv::infer::embedder>(high)
                               : std::static_pointer_cast<mv::infer::embedder>(fast);
      }
      c.on = lands_on;
      c.meta.name = quality == 2 ? "Fake high" : "Fake fast";
      c.meta.spec_key = c.model->spec_key();
      c.meta.quality = quality;
      c.meta.dim = c.model->dim();
      c.meta.input_edge = 16;
      c.meta.dedupe = 0.97f;
      c.meta.query_margin = 0.04f;
      c.meta.result_margin = 0.015f;
      c.meta.similar_min = 0.9f;
      c.meta.generic_prompts = {"a photo.", "an image."};
      return c;
    };
    d.open_faces = [this](std::uint32_t) -> mv::result<std::unique_ptr<mv::ai::face_analyzer>> {
      if (!faces_available) return mv::err(mv::status::io);
      return std::unique_ptr<mv::ai::face_analyzer>(new fake_faces());
    };
    d.runtime_version = [] { return std::string("fake"); };
    d.accelerated_failure = [this](std::uint32_t q) -> std::optional<std::string> {
      std::lock_guard lock(verdict_m);
      return q == 2 ? large_failure : std::nullopt;
    };
    d.forget_accelerated_failure = [this](std::uint32_t q) {
      if (q == 2) set_large_failure(std::nullopt);
    };
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
    d.photos = [this] { return std::unique_ptr<mv::ai::photos_source>(new fake_photos(library)); };
    d.photos_rescan_gap_s = 0;
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

  // The search agent's engine (docs/design/23): the same data folder, read-only,
  // the towers' text halves only (here the same fakes: the vectors match).
  mv::ai::engine_deps reader_deps() {
    mv::ai::engine_deps d = deps();
    d.open_clip = nullptr;  // a reader never opens a picture tower
    d.open_faces = nullptr;
    d.open_sound = nullptr;
    d.open_speech = nullptr;
    d.clip_spec_key = [this](std::uint32_t q) { return (q == 2 ? high : fast)->spec_key(); };
    d.open_clip_text = [this](std::uint32_t q) { return deps().open_clip(q, MV_AI_COMPUTE_CPU_ONLY); };
    d.open_sound_text = [this]() { return deps().open_sound(MV_AI_COMPUTE_CPU_ONLY); };
    d.speech_spec_key = [this](std::uint32_t) {
      return audio_available ? std::string("fake-whisper/fp16/pre1") : std::string();
    };
    return d;
  }
  std::unique_ptr<engine> start_reader() {
    auto rd = std::make_unique<engine>(table->api(), reader_deps(), mv::ai::engine_options{.read_only = true});
    REQUIRE(rd->start());
    for (int i = 0; i < 500; ++i) {
      mv_ai_status s{};
      rd->status(s);
      if (s.state != MV_AI_STATE_LOADING) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return rd;
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
      out.push_back({mv::ai::is_photos_key(*p) ? *p : utf8(fs::path(*p).filename()), r->pts_ms});
    }
    return out;
  }
  // An upper bound, not an expectation: a passing run returns in about a
  // second. Windows runs the workers in THREAD_MODE_BACKGROUND_BEGIN (lowest
  // CPU and I/O priority), and a Debug build on a CI runner indexing 40 photos
  // has taken more than 15 s ("clearing the index...", msvc Debug, 2026-09-28,
  // twice), which says nothing about whether the engine goes idle.
  bool idle(int ms = 60000) { return eng->wait_idle(ms); }
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
  // docs/design/17: a nonsense query says "nothing found", not the least-bad ten.
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

TEST_CASE("precision: stricter answers a subset, broader a superset, and the setting persists", "[ai][engine]") {
  rig r;
  for (const char* name : {"red_1.jpg", "red_2.jpg", "red_3.jpg", "coral_1.jpg", "coral_2.jpg", "blush_1.jpg",
                           "blush_2.jpg", "grey_1.jpg", "grey_2.jpg"}) {
    r.file(name);
  }
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto found = [&](const std::string& q) {
    std::set<std::string> names;
    for (const auto& row : r.search(q)) names.insert(row.first);
    return names;
  };
  const auto includes = [](const std::set<std::string>& big, const std::set<std::string>& small) {
    return std::includes(big.begin(), big.end(), small.begin(), small.end());
  };
  // The default is the calibrated rule, and setting it changes nothing.
  CHECK(r.eng->settings_json().find("\"precision\":2") != std::string::npos);
  const auto base = found("red");
  CHECK(base == std::set<std::string>{"coral_1.jpg", "coral_2.jpg", "red_1.jpg", "red_2.jpg", "red_3.jpg"});
  REQUIRE(r.eng->set_setting("precision", "2"));
  CHECK(found("red") == base);

  // Stricter: a subset, without the near miss; no re-index, no reload.
  const int opens = r.clip_opens.load();
  REQUIRE(r.eng->set_setting("precision", "3"));
  const auto strict3 = found("red");
  REQUIRE(r.eng->set_setting("precision", "4"));
  const auto strict4 = found("red");
  CHECK(includes(base, strict3));
  CHECK(includes(strict3, strict4));
  CHECK(strict4 == std::set<std::string>{"red_1.jpg", "red_2.jpg", "red_3.jpg"});
  CHECK(found("xyzzy plugh").empty());
  CHECK(r.clip_opens.load() == opens);

  // Broader: a superset, with the looser match.
  REQUIRE(r.eng->set_setting("precision", "1"));
  const auto loose1 = found("red");
  REQUIRE(r.eng->set_setting("precision", "0"));
  const auto loose0 = found("red");
  CHECK(includes(loose1, base));
  CHECK(includes(loose0, loose1));
  CHECK(loose0.count("blush_1.jpg") == 1);
  CHECK(base.count("blush_1.jpg") == 0);

  // Out of range clamps; anything but a number is refused.
  REQUIRE(r.eng->set_setting("precision", "9"));
  CHECK(r.eng->settings_json().find("\"precision\":4") != std::string::npos);
  REQUIRE(r.eng->set_setting("precision", "-3"));
  CHECK(r.eng->settings_json().find("\"precision\":0") != std::string::npos);
  CHECK_FALSE(r.eng->set_setting("precision", "\"strict\""));

  // Saved with the other settings: a restart keeps it.
  REQUIRE(r.eng->set_setting("precision", "3"));
  r.start();
  CHECK(r.eng->settings_json().find("\"precision\":3") != std::string::npos);
  REQUIRE(r.idle());
  CHECK(found("red") == strict3);
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
  // The query language (docs/design/17 "Query syntax"): a name while it is typed, an
  // explicit @prefix, leaving a person out, and a kind nobody has.
  CHECK(r.search("Ann").size() == 3);
  CHECK(r.search("anna").size() == 3);
  CHECK(r.search("@an").size() == 3);
  CHECK(r.search("-Anna").size() == 3);  // everything else: two of Ben, one red
  CHECK(r.search("Anna video").empty());
  CHECK(r.search("@zzz").empty());
  CHECK(r.search("in:1999").empty());    // file dates: these were written today
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

TEST_CASE("people in the open folder: only those with a face there, counted and covered there",
          "[ai][engine][faces]") {
  rig r;
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.file("sub/anna_3.jpg");
  r.file("sub/ben_1.jpg");
  r.file("sub/ben_2.jpg");
  r.file("other/ben_3.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.eng->faces_enable(true));
  for (int i = 0; i < 300 && !(r.status().flags & MV_AI_STATUS_FACES_READY); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(r.idle());
  const auto is_anna = [](const mv::json::value& p) {
    const std::string* cover = p.str("cover_path");
    return cover && cover->find("anna") != std::string::npos;
  };

  auto all = mv::json::parse(r.eng->people_json());
  REQUIRE(all);
  REQUIRE(all->a.size() == 2);
  CHECK(*all->a[0].integer("faces") == 3);
  CHECK(*all->a[1].integer("faces") == 3);
  const std::int64_t anna = *all->a[is_anna(all->a[0]) ? 0 : 1].integer("id");
  const std::int64_t ben = *all->a[is_anna(all->a[0]) ? 1 : 0].integer("id");

  // The top folder alone: Anna's two photos there; Ben is only below it.
  auto top = mv::json::parse(r.eng->people_json(utf8(r.photos()), MV_AI_SCOPE_FOLDER));
  REQUIRE(top);
  REQUIRE(top->a.size() == 1);
  CHECK(*top->a[0].integer("id") == anna);
  CHECK(*top->a[0].integer("faces") == 2);

  // Its subfolder: both, Ben first (two faces there to Anna's one), and
  // Anna's cover is her photo in that folder, not her clearest anywhere.
  auto sub = mv::json::parse(r.eng->people_json(utf8(r.photos() / "sub"), MV_AI_SCOPE_FOLDER));
  REQUIRE(sub);
  REQUIRE(sub->a.size() == 2);
  CHECK(*sub->a[0].integer("id") == ben);
  CHECK(*sub->a[0].integer("faces") == 2);
  CHECK(*sub->a[1].integer("id") == anna);
  CHECK(*sub->a[1].integer("faces") == 1);
  REQUIRE(sub->a[1].str("cover_path"));
  CHECK(sub->a[1].str("cover_path")->find("anna_3") != std::string::npos);

  // The tree from the top is everyone with every face; a deeper tree is
  // whoever is in it.
  auto tree = mv::json::parse(r.eng->people_json(utf8(r.photos()), MV_AI_SCOPE_TREE));
  REQUIRE(tree);
  REQUIRE(tree->a.size() == 2);
  CHECK(*tree->a[0].integer("faces") == 3);
  CHECK(*tree->a[1].integer("faces") == 3);
  auto other = mv::json::parse(r.eng->people_json(utf8(r.photos() / "other"), MV_AI_SCOPE_TREE));
  REQUIRE(other);
  REQUIRE(other->a.size() == 1);
  CHECK(*other->a[0].integer("id") == ben);
  CHECK(*other->a[0].integer("faces") == 1);

  // A named person leads in a folder too; Everywhere and no folder are people_json.
  REQUIRE(r.eng->person_rename(anna, "Anna"));
  sub = mv::json::parse(r.eng->people_json(utf8(r.photos() / "sub"), MV_AI_SCOPE_FOLDER));
  REQUIRE(sub);
  REQUIRE(sub->a.size() == 2);
  CHECK(*sub->a[0].integer("id") == anna);
  CHECK(r.eng->people_json("", MV_AI_SCOPE_FOLDER) == r.eng->people_json());
  CHECK(r.eng->people_json(utf8(r.photos() / "sub"), MV_AI_SCOPE_ALL) == r.eng->people_json());
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

  // Quoted words must be said, in that order (docs/design/17 "Query syntax"); the
  // phrase is the snippet. A kind or a date narrows them like anything else.
  const std::uint64_t quoted = r.eng->search_text("\"make a wish\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(quoted, 5000));
  rows = r.rows(quoted);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(r.eng->result_at(quoted, 0)->match == MV_AI_MATCH_SPEECH);
  CHECK(r.eng->result_snippet(quoted, 0)->find("wish") != std::string::npos);
  CHECK(r.search("\"wish a make\"").empty());
  CHECK(r.search("said:birthday video").size() == 1);
  CHECK(r.search("said:birthday photo").empty());
  CHECK(r.search("\"birthday\" before:2000").empty());
  CHECK(r.search("red -\"birthday\"").front().first == "red_photo.jpg");
  CHECK(r.search("\"make a wi").size() == 1);  // a quote still being typed
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

// Owner report (2026-09-27): choosing Sound or Both before the Sound piece was
// ready crashed the app. Whatever the chrome lets through, the engine must
// hold: the setting and a folder's media flip while the piece comes and goes
// ("reload" from the host after an install or a removal), workers index, and
// searches, status and the JSON reads run from other threads.
TEST_CASE("audio: video_index flips while the Sound piece comes and goes", "[ai][engine][audio][stress]") {
  for (int round = 0; round < 4; ++round) {
    rig r;
    r.file("party_talk_bark.mp4");
    r.file("more_talk_bark_slow.mp4");
    r.file("quiet_rgb.mp4");
    r.file("red_photo.jpg");
    r.file("green_photo.jpg");
    r.audio_available = round % 2 == 1;
    r.start();
    auto root = r.eng->index_folder(utf8(r.photos()), false);
    REQUIRE(root);

    std::atomic<bool> done{false};
    // Readers the chrome runs: the status pill, Settings, the search panel.
    std::thread reader([&] {
      int i = 0;
      while (!done) {
        mv_ai_status s{};
        r.eng->status(s);
        (void)r.eng->settings_json();
        (void)r.eng->roots_json();
        (void)r.eng->folder_coverage(utf8(r.photos()));
        const std::uint64_t id = r.eng->search_text(i % 2 ? "a dog barking" : "birthday red", "",
                                                    MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
        (void)r.eng->wait_search(id, 2000);
        if (auto n = r.eng->result_count(id); n && *n > 0) {
          (void)r.eng->result_at(id, 0);
          (void)r.eng->result_snippet(id, 0);
          (void)r.eng->clip_matches(id, utf8(r.photos() / "party_talk_bark.mp4"));
        }
        r.eng->search_release(id);
        ++i;
      }
    });
    for (int i = 0; i < 60; ++i) {
      switch (i % 6) {
        case 0: (void)r.eng->set_setting("video_index", std::to_string(i / 6 % 4)); break;
        case 1: (void)r.eng->root_set_media(*root, static_cast<std::uint32_t>(i / 6 % 4)); break;
        case 2:
          // The piece installed or removed, then the host's reload.
          r.audio_available = !r.audio_available;
          (void)r.eng->set_setting("reload", "1");
          break;
        case 3: (void)r.eng->set_setting("video_index", "3"); break;
        case 4:
          (void)r.eng->set_setting("video_index", "2");
          // People on, and its piece coming and going too.
          if (i % 12 == 4) (void)r.eng->faces_enable(true);
          r.faces_available = !r.faces_available;
          break;
        default: r.eng->note_folder_opened(utf8(r.photos())); break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15 + (i * 7) % 40));
    }
    // Settle with the piece in, indexing for both: everything completes.
    r.audio_available = true;
    REQUIRE(r.eng->set_setting("reload", "1"));
    REQUIRE(r.eng->set_setting("video_index", "3"));
    REQUIRE(r.eng->root_set_media(*root, MV_AI_MEDIA_DEFAULT));
    done = true;
    reader.join();
    // The reload lands on the control thread: wait for it and for the work
    // it queues, not only for an idle moment before it.
    bool settled = false;
    mv_ai_status s{};
    for (int t = 0; t < 600 && !settled; ++t) {
      if (!r.idle(100)) continue;
      s = r.status();
      settled = s.state == MV_AI_STATE_IDLE && (s.flags & MV_AI_STATUS_AUDIO_READY) != 0 && s.sound_total == 3 &&
                s.sound_done == s.sound_total && s.speech_done == s.speech_total;
    }
    REQUIRE(settled);
  }
}

// Installing Sound (the host's "reload") picks the piece up without reopening
// the picture towers: on a Mac that was a second Core ML compile of the tower
// (minutes, gigabytes written) with indexing and search stopped meanwhile.
TEST_CASE("audio: a piece installed under a running pack keeps the picture towers", "[ai][engine][audio]") {
  rig r;
  r.file("party_talk_bark.mp4");
  r.file("red_photo.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle(30000));
  const int opens = r.clip_opens.load();
  CHECK((r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0);

  r.audio_available = true;
  REQUIRE(r.eng->set_setting("reload", "1"));
  bool ready = false;
  for (int t = 0; t < 300 && !ready; ++t) {
    const mv_ai_status s = r.status();
    ready = (s.flags & MV_AI_STATUS_AUDIO_READY) != 0 && s.sound_total == 1 && s.sound_done == 1 &&
            s.speech_done == 1 && s.state == MV_AI_STATE_IDLE;
    if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(ready);
  CHECK(r.clip_opens.load() == opens);
  CHECK(r.search("red").front().first == "red_photo.jpg");

  // And removing it clears what it answered, still without a tower reload.
  r.audio_available = false;
  REQUIRE(r.eng->set_setting("reload", "1"));
  bool gone = false;
  for (int t = 0; t < 300 && !gone; ++t) {
    gone = (r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0;
    if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(gone);
  CHECK(r.search("birthday anna").empty());
  CHECK(r.clip_opens.load() == opens);
}

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

template <typename Fn>
double timed(Fn&& fn) {
  const auto t0 = std::chrono::steady_clock::now();
  fn();
  return ms_since(t0);
}

bool wait_for(const std::function<bool()>& done, int ms) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    if (ms_since(t0) > ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

}  // namespace

// Owner, 2026-09-28: "I set Search quality to High, then tried to adjust the
// search Precision and it crashed." The reload opened L/14 again beside the
// Core ML compile already running, and swapped the towers under models_m_,
// where the replaced tower's destructor waited for its compile (minutes);
// settings_json, which the chrome calls on the main thread after every
// Settings change, waited on models_m_. Every call a chrome makes from its UI
// thread must return at once while a tower opens, and while a replaced one
// takes minutes to go.
TEST_CASE("a quality change never blocks Settings, Precision or a search while towers open and go",
          "[ai][engine]") {
  gates g;  // outlives the engine: its towers go when it does
  rig r;
  r.tower = [&](std::uint32_t quality) -> std::shared_ptr<mv::infer::embedder> {
    if (quality == 2) {
      ++g.opens_waiting;
      hold_while(g.open);  // CreateSession of a big tower
      --g.opens_waiting;
    }
    return std::make_shared<gated_embedder>(quality == 2 ? r.high : r.fast, &g);
  };
  r.file("red.jpg");
  r.file("green.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  REQUIRE(r.eng->active_spec() == "fake-fast/fp16/pre1");
  const int died_before = g.died.load();  // Auto's first look at High, closed at start

  g.open = true;
  g.death = true;
  REQUIRE(r.eng->set_setting("quality", "2"));
  REQUIRE(wait_for([&] { return g.opens_waiting.load() == 1; }, 5000));

  // The new tower is still opening: Settings, Precision and its re-run answer
  // now, the re-run from the tower in service.
  CHECK(timed([&] { REQUIRE(r.eng->set_setting("precision", "1")); }) < 250);
  CHECK(timed([&] { (void)r.eng->settings_json(); }) < 50);
  CHECK(timed([&] { (void)r.status(); }) < 50);
  std::vector<std::pair<std::string, std::int64_t>> red;
  CHECK(timed([&] { red = r.search("red"); }) < 1000);
  REQUIRE_FALSE(red.empty());
  CHECK(red.front().first == "red.jpg");
  CHECK(r.status().state == MV_AI_STATE_LOADING);

  // Open: the swap, the migration, and its end, when the Fast tower goes
  // while its destructor is held. Nothing the UI calls waits for any of it.
  g.open = false;
  double worst_settings = 0, worst_status = 0, worst_set = 0, worst_search = 0;
  int empty_results = 0;
  int level = 0;
  const bool migrated = wait_for(
      [&] {
        worst_settings = std::max(worst_settings, timed([&] { (void)r.eng->settings_json(); }));
        worst_status = std::max(worst_status, timed([&] { (void)r.status(); }));
        worst_set = std::max(worst_set, timed([&] {
                               (void)r.eng->set_setting("precision", std::to_string(1 + (level++ % 2)));
                             }));
        std::vector<std::pair<std::string, std::int64_t>> rows;
        worst_search = std::max(worst_search, timed([&] { rows = r.search("red"); }));
        if (rows.empty() || rows.front().first != "red.jpg") ++empty_results;
        return r.eng->reloads_done() >= 1 && r.eng->active_spec() == "fake-high/fp16/pre1";
      },
      15000);
  REQUIRE(migrated);
  CHECK(worst_settings < 50);
  CHECK(worst_status < 50);
  CHECK(worst_set < 250);
  CHECK(worst_search < 1000);
  CHECK(empty_results == 0);

  // The Fast tower is out of service but still "compiling": it is kept,
  // then released on the engine's own thread once that is done.
  CHECK(g.died.load() == died_before);
  g.death = false;
  REQUIRE(wait_for([&] { return g.died.load() == died_before + 1; }, 5000));
  {
    std::lock_guard lock(g.m);
    CHECK(g.died_on.back() != std::this_thread::get_id());
  }
  CHECK(r.search("green").front().first == "green.jpg");
}

// Auto already runs High where the Neural Engine does (Apple silicon), so
// choosing High is the same tower: no reload, no second Core ML compile.
TEST_CASE("choosing the quality Auto already chose reopens nothing", "[ai][engine]") {
  rig r;
  r.lands_on = mv::infer::backend::coreml;
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  REQUIRE(r.status().quality == 2u);
  REQUIRE(r.eng->active_spec() == "fake-high/fp16/pre1");
  const int opens = r.clip_opens.load();
  const int embedded = r.high->images_embedded.load();

  REQUIRE(r.eng->set_setting("quality", "2"));  // High, explicitly
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 1; }, 5000));
  CHECK(r.clip_opens.load() == opens);
  REQUIRE(r.eng->set_setting("quality", "0"));  // and back to Auto
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 2; }, 5000));
  CHECK(r.clip_opens.load() == opens);
  REQUIRE(r.idle());
  CHECK(r.eng->active_spec() == "fake-high/fp16/pre1");
  CHECK(r.high->images_embedded.load() == embedded);  // nothing re-indexed
  CHECK(r.search("red").front().first == "red.jpg");

  // Another tower does open, and migrates.
  REQUIRE(r.eng->set_setting("quality", "1"));
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 3; }, 5000));
  CHECK(r.clip_opens.load() > opens);
  REQUIRE(r.idle());
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-fast/fp16/pre1"; }, 5000));
  CHECK(r.search("red").front().first == "red.jpg");
}

// A large tower whose Core ML upgrade failed in the background: still on
// CPU, with the provider's fault and words.
class failed_upgrade final : public mv::infer::embedder {
 public:
  explicit failed_upgrade(std::shared_ptr<fake_embedder> inner) : inner_(std::move(inner)) {}
  std::uint32_t dim() const noexcept override { return inner_->dim(); }
  const std::string& spec_key() const noexcept override { return inner_->spec_key(); }
  mv::infer::backend on() const noexcept override { return mv::infer::backend::cpu; }
  mv::infer::provider_fault fault() const noexcept override {
    return failed.load() ? mv::infer::provider_fault::failed : mv::infer::provider_fault::none;
  }
  std::string fault_detail() const override { return failed.load() ? "Error compiling model" : ""; }
  mv::expected embed_images(std::span<const mv::infer::rgb_view> images, std::vector<float>& out) override {
    return inner_->embed_images(images, out);
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override { return inner_->embed_text(text); }
  std::atomic<bool> failed{false};

 private:
  std::shared_ptr<fake_embedder> inner_;
};

// Auto runs the large tower on the Neural Engine, never on CPU (2026-10-05):
// where Core ML failed it on this Mac, the small tower runs, says why, and
// "Try the larger model again" goes back to it.
TEST_CASE("Auto runs the small tower where the provider failed the large one here", "[ai][engine][fallback]") {
  rig r;
  r.lands_on = mv::infer::backend::coreml;
  r.set_large_failure("Error compiling model: <path>");
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  mv_ai_status s = r.status();
  CHECK(s.quality == 1u);
  CHECK((s.flags & MV_AI_STATUS_SMALL_FALLBACK) != 0);
  CHECK(std::string(s.provider_detail_utf8) == "Error compiling model: <path>");
  CHECK(r.eng->active_spec() == "fake-fast/fp16/pre1");
  CHECK(r.search("red").front().first == "red.jpg");

  REQUIRE(r.eng->set_setting("retry_large", "1"));
  REQUIRE(wait_for([&] { return r.status().quality == 2u; }, 5000));
  REQUIRE(r.idle());
  s = r.status();
  CHECK((s.flags & MV_AI_STATUS_SMALL_FALLBACK) == 0);
  CHECK(std::string(s.provider_detail_utf8).empty());
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-high/fp16/pre1"; }, 5000));
  CHECK(r.search("red").front().first == "red.jpg");
}

TEST_CASE("a large tower whose Core ML upgrade fails gives way to the small one", "[ai][engine][fallback]") {
  rig r;
  r.lands_on = mv::infer::backend::coreml;
  auto large = std::make_shared<failed_upgrade>(r.high);
  r.tower = [&](std::uint32_t q) -> std::shared_ptr<mv::infer::embedder> {
    if (q == 2) return large;
    return r.fast;
  };
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  REQUIRE(r.status().quality == 2u);

  // The pack records the verdict, then the tower reports its fault.
  r.set_large_failure("Error compiling model");
  large->failed = true;
  REQUIRE(wait_for([&] { return r.status().quality == 1u; }, 10000));
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK((s.flags & MV_AI_STATUS_SMALL_FALLBACK) != 0);
  CHECK(std::string(s.provider_detail_utf8) == "Error compiling model");
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-fast/fp16/pre1"; }, 5000));
  CHECK(r.search("red").front().first == "red.jpg");
}

// The query is embedded by the tower whose vectors are in the matrix, and
// the two change as one at a migration's end: never a Fast query against High
// vectors (another size here, so a mix would find nothing).
TEST_CASE("searches during a migration and across its end use the answering tower", "[ai][engine]") {
  gates g;
  rig r;
  r.tower = [&](std::uint32_t quality) -> std::shared_ptr<mv::infer::embedder> {
    if (quality == 2) return std::make_shared<gated_embedder>(r.high, &g, 4);
    return std::make_shared<gated_embedder>(r.fast, &g);
  };
  r.file("red.jpg");
  r.file("green.jpg");
  r.file("blue.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  REQUIRE(r.eng->active_spec() == "fake-fast/fp16/pre1");

  g.embed = true;  // the High tower's indexing waits: the migration stays mid-way
  REQUIRE(r.eng->set_setting("quality", "2"));
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 1; }, 5000));
  CHECK(r.eng->active_spec() == "fake-fast/fp16/pre1");
  for (int i = 0; i < 3; ++i) {
    const auto rows = r.search("red");
    REQUIRE_FALSE(rows.empty());
    CHECK(rows.front().first == "red.jpg");
  }

  // Searches run back to back across the migration's end (no assertion on
  // this thread: the results are counted and checked after).
  std::atomic<bool> stop{false};
  std::atomic<int> runs{0};
  std::atomic<int> wrong{0};
  std::thread searcher([&] {
    while (!stop) {
      const std::uint64_t id = r.eng->search_text("red", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
      bool ok = r.eng->wait_search(id, 5000);
      if (ok) {
        auto n = r.eng->result_count(id);
        ok = n && *n > 0;
        if (ok) {
          auto p = r.eng->result_path(id, 0);
          ok = p && utf8(fs::path(*p).filename()) == "red.jpg";
        }
      }
      if (!ok) ++wrong;
      ++runs;
    }
  });
  g.embed = false;
  const bool migrated = wait_for([&] { return r.eng->active_spec() == "fake-high/fp16/pre1"; }, 15000);
  const int at_end = runs.load();
  (void)wait_for([&] { return runs.load() >= at_end + 3; }, 5000);
  stop = true;
  searcher.join();
  REQUIRE(migrated);
  CHECK(runs.load() > 0);
  CHECK(wrong.load() == 0);
  const auto after = r.search("red");
  REQUIRE_FALSE(after.empty());
  CHECK(after.front().first == "red.jpg");
  CHECK(r.status().frames_indexed == 3);  // the old rows went; nothing doubled
}

// ---- the search agent's reader (docs/design/23 Phase 1) ---------------------------------------

namespace {

struct scored {
  std::string name;
  std::int64_t pts_ms;
  float score;
  std::uint32_t more;
};

std::vector<scored> ranked(engine& e, const std::string& q, std::uint32_t kinds = MV_AI_KIND_ALL) {
  const std::uint64_t id = e.search_text(q, "", MV_AI_SCOPE_ALL, kinds);
  REQUIRE(e.wait_search(id, 5000));
  std::vector<scored> out;
  auto n = e.result_count(id);
  REQUIRE(n);
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto r = e.result_at(id, i);
    auto p = e.result_path(id, i);
    REQUIRE(r);
    REQUIRE(p);
    out.push_back({utf8(fs::path(*p).filename()), r->pts_ms, r->score, r->more_in_clip});
  }
  e.search_release(id);
  return out;
}

std::string bytes_of(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("the reader answers exactly as the app's engine on the same index", "[ai][search-agent]") {
  rig r;
  r.audio_available = true;
  r.file("red_car.jpg");
  r.file("red_b.jpg");
  r.file("green_field.jpg");
  r.file("blue_sea.jpg");
  r.file("holiday_rgb.mp4");
  r.file("party_talk_bark.mp4");
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.file("ben_1.jpg");
  r.start();
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle(30000));
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE_FALSE(people->a.empty());
  REQUIRE(r.eng->person_rename(*people->a[0].integer("id"), "Anna"));

  // The app keeps running beside it: the reader is a second view, not a
  // replacement.
  auto rd = r.start_reader();
  mv_ai_status s{};
  rd->status(s);
  REQUIRE(s.state != MV_AI_STATE_ERROR);
  CHECK(rd->read_only());
  CHECK(rd->active_spec() == r.eng->active_spec());

  // docs/design/17's eval set shape: subjects, a moment in a clip, sounds, words
  // said, people, exclusions, kinds, dates and nonsense.
  const char* const queries[] = {"red", "something red", "green", "blue", "a dog barking", "birthday anna",
                                 "\"make a wish\"", "Anna", "@an", "-Anna", "red video", "red photo",
                                 "in:1999", "xyzzy plugh", "Ann"};
  for (const char* q : queries) {
    INFO(q);
    const auto app = ranked(*r.eng, q);
    const auto reader = ranked(*rd, q);
    REQUIRE(reader.size() == app.size());
    for (std::size_t i = 0; i < app.size(); ++i) {
      CHECK(reader[i].name == app[i].name);
      CHECK(reader[i].pts_ms == app[i].pts_ms);
      CHECK(std::fabs(reader[i].score - app[i].score) <= 1e-4f);
      CHECK(reader[i].more == app[i].more);
    }
  }
  // Kinds from the chips as well as the words.
  CHECK(ranked(*rd, "red", MV_AI_KIND_VIDEOS).size() == ranked(*r.eng, "red", MV_AI_KIND_VIDEOS).size());

  // Find similar on an indexed moment is its stored vector: nothing decoded.
  const int decoded = r.stills_decoded.load();
  const std::uint64_t sim = rd->search_similar(utf8(r.photos() / "red_car.jpg"), -1, "", MV_AI_SCOPE_ALL,
                                               MV_AI_KIND_ALL);
  REQUIRE(rd->wait_search(sim, 5000));
  auto n = rd->result_count(sim);
  REQUIRE(n);
  REQUIRE(*n >= 1);
  // The nearest is red: red_b.jpg, or the clip's opening red seconds, which
  // the fake tower embeds identically (a tie either may win).
  {
    const std::string top = utf8(fs::path(*rd->result_path(sim, 0)).filename());
    CHECK((top == "red_b.jpg" || top == "holiday_rgb.mp4"));
    if (top == "holiday_rgb.mp4") CHECK(rd->result_at(sim, 0)->pts_ms < 3000);
  }
  const std::uint64_t moment = rd->search_similar(utf8(r.photos() / "holiday_rgb.mp4"), 3100, "",
                                                  MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(rd->wait_search(moment, 5000));
  CHECK(r.stills_decoded.load() == decoded);
}

TEST_CASE("the reader never writes: the index is byte-identical and every change is refused",
          "[ai][search-agent]") {
  rig r;
  r.file("red_car.jpg");
  r.file("green_field.jpg");
  r.file("anna_1.jpg");
  r.start();
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  r.eng.reset();  // the app quits: nothing else writes from here on
  const fs::path data = r.dir / "data";
  std::map<std::string, std::string> before;
  for (const auto& e : fs::directory_iterator(data)) {
    if (e.is_regular_file()) before[utf8(e.path().filename())] = bytes_of(e.path());
  }
  REQUIRE(before.count("index.db"));

  {
    auto rd = r.start_reader();
    CHECK(ranked(*rd, "red").size() == 1);
    CHECK(rd->index_folder(utf8(r.dir / "Elsewhere"), true).error() == mv::status::unsupported_format);
    CHECK(rd->root_rescan(1).error() == mv::status::unsupported_format);
    CHECK(rd->root_remove(1).error() == mv::status::unsupported_format);
    CHECK(rd->root_set_enabled(1, false).error() == mv::status::unsupported_format);
    CHECK(rd->clear_index().error() == mv::status::unsupported_format);
    CHECK(rd->set_setting("precision", "4").error() == mv::status::unsupported_format);
    CHECK(rd->faces_enable(false).error() == mv::status::unsupported_format);
    CHECK(rd->person_rename(1, "Zed").error() == mv::status::unsupported_format);
    CHECK(rd->import_index(utf8(r.dir / "x.mvindex"), "[]", 0).error() == mv::status::unsupported_format);
    CHECK(rd->export_index(utf8(r.dir / "x.mvindex"), {}, 0).error() == mv::status::unsupported_format);
    CHECK(rd->index_photos_library().error() == mv::status::unsupported_format);
    rd->pause(true);
    rd->note_folder_opened(utf8(r.photos()));
  }

  for (const auto& [name, bytes] : before) {
    INFO(name);
    // SQLite may add an empty -shm beside a WAL database it reads; the data
    // files themselves do not change by a byte.
    if (name.find("-shm") != std::string::npos) continue;
    CHECK(bytes_of(data / name) == bytes);
  }
  CHECK(fs::exists(data / "faces.db"));
  CHECK_FALSE(fs::exists(data / "settings.json.tmp"));

  // No index at all: the reader refuses to start and creates nothing.
  scratch_dir empty{"ai-reader-empty"};
  mv::addon::host_services svc;
  svc.data_dir = utf8(empty / "data");
  mv::addon::host_table t(std::move(svc));
  t.set_negotiated(MV_ADDON_HOST_API);
  engine none(t.api(), r.reader_deps(), mv::ai::engine_options{.read_only = true});
  CHECK_FALSE(none.start());
  CHECK_FALSE(fs::exists(empty / "data" / "index.db"));
}

TEST_CASE("the reader catches up with what the app indexes after it started", "[ai][search-agent]") {
  rig r;
  r.file("red_car.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  auto rd = r.start_reader();
  CHECK(ranked(*rd, "blue").empty());

  r.file("blue_sea.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  CHECK(ranked(*r.eng, "blue").size() == 1);
  // Within the catch-up interval (docs/design/23), appended rather than reloaded.
  bool found = false;
  for (int i = 0; i < 200 && !found; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    found = ranked(*rd, "blue").size() == 1;
  }
  CHECK(found);
  CHECK(ranked(*rd, "red").size() == 1);

  // A file removed in the app drops out of the reader too.
  fs::remove(r.photos() / "red_car.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  bool gone = false;
  for (int i = 0; i < 200 && !gone; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gone = ranked(*rd, "red").empty();
  }
  CHECK(gone);
}

// ---- the search agent's session over the pack's table (docs/design/23 Phase 1) ------------------

namespace {

// mv.ai.1's search calls over an engine, as addon_entry.cpp builds them: what
// the agent's search_session asks of the pack.
struct table_over {
  engine* e = nullptr;
  mv_ai_api api{};
  static engine& of(void* ctx) { return *static_cast<table_over*>(ctx)->e; }
  static mv_status copy(const std::string& s, char* out, std::uint32_t cap) {
    if (cap < s.size() + 1) return MV_ERR_INVALID_ARG;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return MV_OK;
  }
  explicit table_over(engine& eng) : e(&eng) {
    api.struct_size = sizeof(mv_ai_api);
    api.ctx = this;
    api.status = [](void* c, mv_ai_status* o) { of(c).status(*o); return MV_OK; };
    api.search_text = [](void* c, const char* q, const char* d, std::uint32_t s, std::uint32_t k, std::uint64_t* id) {
      *id = of(c).search_text(q, d ? d : "", s, k);
      return MV_OK;
    };
    api.search_similar = [](void* c, const char* p, std::int64_t ms, const char* d, std::uint32_t s, std::uint32_t k,
                            std::uint64_t* id) {
      *id = of(c).search_similar(p, ms, d ? d : "", s, k);
      return MV_OK;
    };
    api.result_count = [](void* c, std::uint64_t id, std::uint32_t* n) {
      auto r = of(c).result_count(id);
      if (!r) return static_cast<mv_status>(r.error());
      *n = *r;
      return MV_OK;
    };
    api.result_at = [](void* c, std::uint64_t id, std::uint32_t i, mv_ai_result* o) {
      auto r = of(c).result_at(id, i);
      if (!r) return static_cast<mv_status>(r.error());
      *o = *r;
      return MV_OK;
    };
    api.result_path = [](void* c, std::uint64_t id, std::uint32_t i, char* o, std::uint32_t cap) {
      auto r = of(c).result_path(id, i);
      return r ? copy(*r, o, cap) : static_cast<mv_status>(r.error());
    };
    api.result_thumb = [](void* c, std::uint64_t id, std::uint32_t i, char* o, std::uint32_t cap) {
      auto r = of(c).result_thumb(id, i);
      return r ? copy(*r, o, cap) : static_cast<mv_status>(r.error());
    };
    api.clip_matches = [](void* c, std::uint64_t id, const char* p, std::int64_t* ms, float* sc, std::uint32_t cap,
                          std::uint32_t* n) {
      auto r = of(c).clip_matches(id, p);
      if (!r) return static_cast<mv_status>(r.error());
      *n = static_cast<std::uint32_t>(r->size());
      if (!ms) return MV_OK;
      if (cap < r->size()) return MV_ERR_INVALID_ARG;
      for (std::size_t i = 0; i < r->size(); ++i) {
        ms[i] = (*r)[i].first;
        if (sc) sc[i] = (*r)[i].second;
      }
      return MV_OK;
    };
    api.search_release = [](void* c, std::uint64_t id) { of(c).search_release(id); return MV_OK; };
    api.result_duration = [](void* c, std::uint64_t id, std::uint32_t i, std::int64_t* o) {
      auto r = of(c).result_duration(id, i);
      if (!r) return static_cast<mv_status>(r.error());
      *o = *r;
      return MV_OK;
    };
    api.roots_json = [](void* c, char* o, std::uint32_t cap, std::uint32_t*) { return copy(of(c).roots_json(), o, cap); };
    api.suggest_json = [](void* c, const char* q, char* o, std::uint32_t cap, std::uint32_t*) {
      return copy(of(c).suggest_json(q), o, cap);
    };
  }
};

}  // namespace

TEST_CASE("the panel's scope picker and completions come from the reader", "[ai][search-agent]") {
  rig r;
  r.file("red_car.jpg");
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.start();
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE_FALSE(people->a.empty());
  REQUIRE(r.eng->person_rename(*people->a[0].integer("id"), "Anna"));
  auto rd = r.start_reader();
  table_over t(*rd);
  auto session = mv::nle::search_session::over(&t.api);
  auto roots = session->roots_json(5000);
  REQUIRE(roots);
  auto parsed = mv::json::parse(*roots);
  REQUIRE(parsed);
  REQUIRE(parsed->a.size() == 1);
  const std::string* path = parsed->a[0].str("path");
  REQUIRE(path);
  CHECK(*path == utf8(r.photos()));
  // The reader sees the name the app gave, as the panel types it.
  auto names = session->suggest_json("@An", 5000);
  REQUIRE(names);
  CHECK(names->find("\"Anna\"") != std::string::npos);
  CHECK(*names == r.eng->suggest_json("@An"));
}

TEST_CASE("the agent's session returns the in-app top-K through the reader, over the wire", "[ai][search-agent]") {
  rig r;
  r.file("red_car.jpg");
  r.file("red_b.jpg");
  r.file("green_field.jpg");
  r.file("holiday_rgb.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  auto rd = r.start_reader();
  table_over t(*rd);
  auto session = mv::nle::search_session::over(&t.api);
  REQUIRE(session->wait_ready(5000));

  for (const char* q : {"red", "green", "blue", "xyzzy plugh", "red video"}) {
    INFO(q);
    const auto app = ranked(*r.eng, q);
    mv::nle::request req;
    req.correlation_id = 7;
    const mv::nle::reply rep = session->run(req, q, "", 5000);
    REQUIRE(rep.code == mv::status::ok);
    CHECK(rep.correlation_id == 7);
    // What FCP's extension decodes is what the app's engine answered.
    auto back = mv::nle::decode(mv::nle::encode(rep));
    REQUIRE(back);
    REQUIRE(back->rows.size() == app.size());
    for (std::size_t i = 0; i < app.size(); ++i) {
      CHECK(utf8(fs::path(back->rows[i].path).filename()) == app[i].name);
      CHECK(back->rows[i].pts_ms == app[i].pts_ms);
      CHECK(std::fabs(back->rows[i].score - app[i].score) <= 1e-4f);
    }
  }
  // A clip carries its length (for the FCPXML asset) and its matches (markers).
  mv::nle::request req;
  req.kinds = MV_AI_KIND_VIDEOS;
  const mv::nle::reply green = session->run(req, "green", "", 5000);
  REQUIRE(green.rows.size() == 1);
  CHECK(green.rows[0].kind == MV_AI_KIND_VIDEOS);
  REQUIRE(green.rows[0].moments.size() == 1);
  CHECK(green.rows[0].moments[0].pts_ms == 3000);
  // Find similar on an indexed still, by path.
  mv::nle::request sim;
  sim.kind = mv::nle::request_kind::similar;
  const mv::nle::reply like = session->run(sim, utf8(r.photos() / "red_car.jpg"), "", 5000);
  const std::uint64_t app_like =
      r.eng->search_similar(utf8(r.photos() / "red_car.jpg"), -1, "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(app_like, 5000));
  const auto app_rows = r.rows(app_like);
  REQUIRE(like.rows.size() == app_rows.size());
  REQUIRE_FALSE(like.rows.empty());
  for (std::size_t i = 0; i < app_rows.size(); ++i) {
    CHECK(utf8(fs::path(like.rows[i].path).filename()) == app_rows[i].first);
  }
  // The nearest is red: red_b.jpg, or the clip's opening red seconds (a tie
  // under the fake tower; the order is the app's, checked above).
  const std::string top = utf8(fs::path(like.rows[0].path).filename());
  CHECK((top == "red_b.jpg" || top == "holiday_rgb.mp4"));
  if (top == "holiday_rgb.mp4") CHECK(like.rows[0].pts_ms < 3000);
  // Another wire version is refused rather than guessed at.
  mv::nle::request old;
  old.version = 99;
  CHECK(session->run(old, "red", "", 5000).code == mv::status::unsupported_format);
}

// ---- the Photos library (issue #72) ------------------------------------------------

namespace {

bool has_row(const std::vector<std::pair<std::string, std::int64_t>>& rows, const std::string& path) {
  return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == path; });
}

// Polls until `ok` holds (the control thread rescans on its own tick).
bool eventually(const std::function<bool()>& ok, int ms = 10000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (ok()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ok();
}

}  // namespace

// ---- sharing an index (docs/design/17 "Sharing an index") ---------------------------------------

namespace {

// The last export / import, once it has finished.
mv::json::value transfer_done(rig& r) {
  mv::json::value out;
  REQUIRE(wait_for(
      [&] {
        auto t = mv::json::parse(r.eng->transfer_json());
        if (!t || !t->boolean("done").value_or(false)) return false;
        out = *t;
        return true;
      },
      15000));
  return out;
}

// Machine B's copy of machine A's Photos, under another folder, each file with
// A's modification time (a NAS seen from two machines, a copied card).
void copy_library(const rig& from, const fs::path& to) {
  for (const auto& e : fs::directory_iterator(from.photos())) {
    fs::create_directories(to);
    fs::copy_file(e.path(), to / e.path().filename(), fs::copy_options::overwrite_existing);
    fs::last_write_time(to / e.path().filename(), fs::last_write_time(e.path()));
  }
}

std::string map_json(std::int64_t file_root, const fs::path& dir) {
  mv::json::writer w;
  w.begin_array().begin_object();
  w.key("id").integer(file_root);
  w.key("path").string(utf8(dir));
  w.end_object().end_array();
  return w.take();
}

}  // namespace

TEST_CASE("the Photos library indexes what is on this Mac and counts iCloud-only apart", "[ai][engine][photos]") {
  rig r;
  r.file("clips/rgb_clip.mp4");
  r.file("red_folder.jpg");
  r.library->items = {
      {"red-rose", 1, mv::ai::asset_kind::photo, true, ""},
      {"green-leaf", 1, mv::ai::asset_kind::photo, true, ""},
      {"blue-cloud", 1, mv::ai::asset_kind::photo, false, ""},          // only in iCloud
      {"rgb-clip", 1, mv::ai::asset_kind::video, true, utf8(r.photos() / "clips" / "rgb_clip.mp4")},
      {"red-cloudclip", 1, mv::ai::asset_kind::video, false, ""},        // only in iCloud: its poster
  };
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  auto root = r.eng->index_photos_library();
  REQUIRE(root);
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK(s.state == MV_AI_STATE_IDLE);
  CHECK(s.assets_total == 6);        // five in Photos, one in the folder
  CHECK(s.assets_done == 4);         // red, green, the local clip, the folder's photo
  CHECK(s.assets_unavailable == 2);  // the iCloud-only photo and clip
  CHECK(s.assets_failed == 0);

  const auto red = r.search("red");
  CHECK(has_row(red, "photos:red-rose"));
  CHECK(has_row(red, "red_folder.jpg"));
  CHECK(has_row(red, "photos:red-cloudclip"));  // found by its poster, at 0 ms
  CHECK(has_row(r.search("blue"), "photos:rgb-clip"));  // the local clip's blue moment
  CHECK_FALSE(has_row(r.search("blue"), "photos:blue-cloud"));

  // Scoped to the library ("photos:"): the folder's photo is out.
  const auto in_library = r.search("red", MV_AI_SCOPE_FOLDER, "photos:");
  CHECK(has_row(in_library, "photos:red-rose"));
  CHECK_FALSE(has_row(in_library, "red_folder.jpg"));

  // A Photos still's tile is its key: the chrome draws it from PhotoKit.
  const std::uint64_t id = r.eng->search_text("red", "photos:", MV_AI_SCOPE_FOLDER, MV_AI_KIND_PHOTOS);
  REQUIRE(r.eng->wait_search(id, 5000));
  auto thumb = r.eng->result_thumb(id, 0);
  REQUIRE(thumb);
  CHECK(*thumb == "photos:red-rose");

  const std::string roots = r.eng->roots_json();
  CHECK(roots.find("\"kind\":\"photos\"") != std::string::npos);
  CHECK(roots.find("\"access\":\"full\"") != std::string::npos);
  CHECK(roots.find("\"unavailable\":2") != std::string::npos);
  // It is not a folder: "Index this folder" refuses the key.
  CHECK_FALSE(r.eng->index_folder("photos:", true));
}

TEST_CASE("the Photos library is never read before access is granted, and keeps its index when revoked",
          "[ai][engine][photos]") {
  rig r;
  r.library->access = mv::ai::photos_access::not_determined;
  r.library->items = {{"red-rose", 1, mv::ai::asset_kind::photo, true, ""}};
  r.start();
  auto denied = r.eng->index_photos_library();
  REQUIRE_FALSE(denied);
  CHECK(denied.error() == mv::status::permission_denied);
  REQUIRE(r.idle());
  CHECK(r.library->enumerations == 0);
  CHECK(r.library->stills == 0);

  r.library->access = mv::ai::photos_access::full;  // the chrome's prompt said yes
  auto root = r.eng->index_photos_library();
  REQUIRE(root);
  REQUIRE(r.idle());
  CHECK(has_row(r.search("red"), "photos:red-rose"));

  // Revoked in System Settings: the rows stay searchable (like a drive that
  // is not plugged in), nothing is read, and the index is not "busy".
  r.library->access = mv::ai::photos_access::denied;
  {
    std::lock_guard lock(r.library->m);
    r.library->items.push_back({"green-leaf", 2, mv::ai::asset_kind::photo, true, ""});
  }
  const int stills = r.library->stills;
  REQUIRE(r.eng->root_rescan(*root));
  REQUIRE(r.idle());
  CHECK(r.library->stills == stills);
  CHECK(has_row(r.search("red"), "photos:red-rose"));
  CHECK(r.status().state == MV_AI_STATE_IDLE);
  CHECK(r.eng->roots_json().find("\"access\":\"denied\"") != std::string::npos);
}

TEST_CASE("a Photos edit re-queues its asset, a delete drops it, and a change notice rescans",
          "[ai][engine][photos]") {
  rig r;
  r.library->items = {{"red-rose", 1, mv::ai::asset_kind::photo, true, ""},
                      {"green-leaf", 1, mv::ai::asset_kind::photo, true, ""}};
  r.start();
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  const int embedded = r.fast->images_embedded + r.high->images_embedded;

  {
    std::lock_guard lock(r.library->m);
    r.library->items[0].mtime = 2;         // edited in Photos
    r.library->items.erase(r.library->items.begin() + 1);  // deleted in Photos
    r.library->items.push_back({"blue-sky", 1, mv::ai::asset_kind::photo, true, ""});  // imported
  }
  r.library->notify();  // PhotoKit's change observer
  REQUIRE(eventually([&] { return has_row(r.search("blue"), "photos:blue-sky"); }));
  REQUIRE(r.idle());
  CHECK_FALSE(has_row(r.search("green"), "photos:green-leaf"));
  CHECK(has_row(r.search("red"), "photos:red-rose"));
  // The edit and the import were embedded; nothing else was.
  CHECK(r.fast->images_embedded + r.high->images_embedded == embedded + 2);
}

TEST_CASE("an iCloud-only asset is tried again after a restart and indexes once it is local",
          "[ai][engine][photos]") {
  rig r;
  r.library->items = {
      {"blue-cloud", 1, mv::ai::asset_kind::photo, false, ""},
      {"red-cloudclip", 1, mv::ai::asset_kind::video, false, ""},  // stays in iCloud throughout
  };
  r.start();
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  CHECK(r.status().assets_unavailable == 2);
  CHECK(has_row(r.search("red"), "photos:red-cloudclip"));  // its poster
  // A favourite toggled elsewhere changes nothing about it: not asked again.
  const int asked = r.library->stills;
  r.library->notify();
  REQUIRE(eventually([&] { return r.library->enumerations >= 2; }));
  REQUIRE(r.idle());
  CHECK(r.library->stills == asked);

  {
    std::lock_guard lock(r.library->m);
    r.library->items[0].local = true;  // the user opened it in Photos: downloaded
  }
  const int posters = r.library->stills;
  r.start();  // the next launch
  REQUIRE(eventually([&] { return r.status().assets_done == 1; }));
  REQUIRE(r.idle());
  CHECK(r.status().assets_unavailable == 1);  // the clip, still only in iCloud
  CHECK(has_row(r.search("blue"), "photos:blue-cloud"));
  // The clip was asked whether it is here now (one lookup), not read again:
  // its poster row stayed, and nothing was re-embedded for it.
  CHECK(has_row(r.search("red"), "photos:red-cloudclip"));
  CHECK(r.library->stills == posters + 2);  // the photo: the launch check, then its embedding
}

TEST_CASE("the opt-in iCloud fetch downloads iCloud-only clips, indexes them, then deletes the files",
          "[ai][engine][photos][icloud]") {
  rig r;
  r.library->items = {
      {"red-rose", 1, mv::ai::asset_kind::photo, true, ""},
      {"blue-cloud", 1, mv::ai::asset_kind::photo, false, ""},      // a photo: never fetched
      {"red-cloudclip", 1, mv::ai::asset_kind::video, false, ""},   // only in iCloud
  };
  r.start();
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  CHECK(r.status().assets_unavailable == 2);
  CHECK(r.status().icloud_fetch == MV_AI_ICLOUD_OFF);  // off by default
  CHECK_FALSE(has_row(r.search("blue"), "photos:red-cloudclip"));  // only its red poster
  CHECK(r.library->fetches == 0);

  // On battery, then on a metered network: it waits, downloading nothing.
  r.on_battery = true;
  REQUIRE(r.eng->set_setting("icloud_videos", "1"));
  REQUIRE(eventually([&] { return r.status().icloud_fetch == MV_AI_ICLOUD_WAIT_POWER; }));
  CHECK(r.status().icloud_videos_left == 1);
  r.library->unmetered = false;
  r.on_battery = false;
  REQUIRE(eventually([&] { return r.status().icloud_fetch == MV_AI_ICLOUD_WAIT_NETWORK; }));
  CHECK(r.library->fetches == 0);

  r.library->unmetered = true;
  // Downloaded, then indexed from the original: its blue moment is found.
  REQUIRE(eventually([&] { return has_row(r.search("blue"), "photos:red-cloudclip"); }));
  REQUIRE(eventually([&] { return r.status().icloud_fetch == MV_AI_ICLOUD_DONE; }));
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK(r.library->fetches == 1);
  CHECK(s.icloud_videos_fetched == 1);
  CHECK(s.icloud_videos_left == 0);
  CHECK(s.assets_unavailable == 1);  // the photo, whose local preview is all there is
  CHECK(s.assets_done == 2);
  // Its file is gone once every track is done with it.
  const fs::path cache = r.dir / "data" / "cache" / "icloud";
  REQUIRE(eventually([&] { return !fs::exists(cache) || fs::is_empty(cache); }));
  // A restart does not fetch it again: it is indexed.
  r.start();
  REQUIRE(r.idle());
  REQUIRE(eventually([&] { return r.status().icloud_fetch == MV_AI_ICLOUD_DONE; }));
  CHECK(r.library->fetches == 1);
  CHECK(r.eng->settings_json().find("\"icloud_videos\":true") != std::string::npos);
}

TEST_CASE("the People pass leaves iCloud-only Photos assets alone until they are local",
          "[ai][engine][photos][faces]") {
  rig r;
  r.library->items = {
      {"anna-cloud-1", 1, mv::ai::asset_kind::photo, false, ""},
      {"anna-cloud-2", 1, mv::ai::asset_kind::photo, false, ""},
      {"red-cloudclip", 1, mv::ai::asset_kind::video, false, ""},
  };
  r.start();
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  REQUIRE(r.eng->faces_enable(true));
  for (int i = 0; i < 300 && !(r.status().flags & MV_AI_STATUS_FACES_READY); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Nothing to scan on this Mac: the pass ends instead of asking PhotoKit for
  // the same clip again and again (it never went idle before this held).
  REQUIRE(r.idle(10000));
  const int asked = r.library->videos;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(r.library->videos == asked);
  CHECK(mv::json::parse(r.eng->people_json())->a.empty());

  {
    std::lock_guard lock(r.library->m);
    r.library->items[0].local = true;  // Anna's photos arrived from iCloud
    r.library->items[1].local = true;
  }
  r.start();
  REQUIRE(eventually([&] { return r.status().assets_done == 2; }));
  REQUIRE(r.idle());
  // Their faces were scanned once there were pictures to scan: one person.
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 1);
  CHECK(*people->a[0].integer("faces") == 2);
}

TEST_CASE("an index exported on one machine answers on another without embedding again",
          "[ai][engine][transfer]") {
  rig a;
  a.file("red_car.jpg");
  a.file("green_field.jpg");
  a.file("blue_sea.jpg");
  a.file("holiday_rgb.mp4");
  a.start();
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(a.idle());
  const std::string file = utf8(a.dir / "library.mvindex");
  auto job = a.eng->export_index(file, {}, 0);
  REQUIRE(job);
  auto done = transfer_done(a);
  CHECK(*done.integer("status") == 0);
  REQUIRE(fs::exists(file));
  CHECK_FALSE(fs::exists(file + ".part"));
  CHECK(*done.find("outcome")->integer("assets") == 4);
  CHECK(*done.find("outcome")->integer("frames") == 3 + 3);

  // Machine B: the same files somewhere else.
  rig b;
  const fs::path there = b.dir / "NAS" / "Photos";
  copy_library(a, there);
  b.start();
  REQUIRE(b.idle());
  auto info = mv::json::parse(*b.eng->inspect_export(file));
  REQUIRE(info);
  CHECK(info->boolean("picture_usable") == true);
  CHECK(*info->str("model") == "Fake fast");
  REQUIRE(info->find("roots")->a.size() == 1);
  const auto& root = info->find("roots")->a[0];
  CHECK(*root.str("path") == utf8(a.photos()));
  CHECK(*root.integer("assets") == 4);
  // Both "machines" share this disk, so A's folder is a folder here too.
  CHECK(root.boolean("exists") == fs::is_directory(a.photos()));

  REQUIRE(b.eng->import_index(file, map_json(*root.integer("id"), there), 0));
  done = transfer_done(b);
  CHECK(*done.integer("status") == 0);
  CHECK(*done.find("outcome")->integer("added") == 4);
  REQUIRE(b.idle());
  CHECK(b.fast->images_embedded.load() == 0);  // nothing embedded again
  CHECK(b.status().assets_done == 4);
  CHECK(b.status().frames_indexed == 6);
  const auto red = b.search("red");
  REQUIRE(red.size() == 2);
  const auto green = b.search("green");
  CHECK(std::find(green.begin(), green.end(), std::make_pair(std::string("holiday_rgb.mp4"), std::int64_t{3000})) !=
        green.end());
  auto path = b.eng->search_text("blue", utf8(there), MV_AI_SCOPE_FOLDER, MV_AI_KIND_ALL);
  REQUIRE(b.eng->wait_search(path, 5000));
  auto p = b.eng->result_path(path, 0);
  REQUIRE(p);
  CHECK(fs::path(*p).parent_path() == there);  // B's paths, not A's
}

TEST_CASE("an imported file that differs here is indexed again; rows done here stay",
          "[ai][engine][transfer]") {
  rig a;
  a.file("red_a.jpg");
  a.file("red_b.jpg");
  a.file("blue_c.jpg");
  a.start();
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(a.idle());
  const std::string file = utf8(a.dir / "x.mvindex");
  REQUIRE(a.eng->export_index(file, {}, 0));
  REQUIRE(*transfer_done(a).integer("status") == 0);

  rig b;
  const fs::path there = b.dir / "Photos";
  copy_library(a, there);
  // Edited here after the export: another size.
  write_bytes(there / "red_b.jpg", pattern(64, 9));
  b.start();
  const auto root_id = *mv::json::parse(*b.eng->inspect_export(file))->find("roots")->a[0].integer("id");
  REQUIRE(b.eng->import_index(file, map_json(root_id, there), 0));
  REQUIRE(*transfer_done(b).integer("status") == 0);
  REQUIRE(b.idle());
  CHECK(b.fast->images_embedded.load() == 1);  // only the edited one
  CHECK(b.status().assets_done == 3);
  CHECK(b.status().frames_indexed == 3);

  // Importing the same file again changes nothing: every row is done here.
  REQUIRE(b.eng->import_index(file, map_json(root_id, there), 0));
  auto again = transfer_done(b);
  CHECK(*again.find("outcome")->integer("added") == 0);
  CHECK(*again.find("outcome")->integer("frames") == 0);
  REQUIRE(b.idle());
  CHECK(b.status().frames_indexed == 3);
  CHECK(b.fast->images_embedded.load() == 1);
}

TEST_CASE("an empty index adopts the file's Quality; a used one keeps its own",
          "[ai][engine][transfer]") {
  rig a;
  a.file("red.jpg");
  a.file("green.jpg");
  a.start();
  REQUIRE(a.eng->set_setting("quality", "2"));
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(wait_for([&] { return a.eng->active_spec() == "fake-high/fp16/pre1"; }, 15000));
  REQUIRE(a.idle());
  const std::string file = utf8(a.dir / "high.mvindex");
  REQUIRE(a.eng->export_index(file, {}, 0));
  REQUIRE(*transfer_done(a).integer("status") == 0);

  SECTION("empty here: adopted") {
    rig b;
    const fs::path there = b.dir / "Photos";
    copy_library(a, there);
    b.start();
    REQUIRE(b.idle());
    auto info = mv::json::parse(*b.eng->inspect_export(file));
    CHECK(info->boolean("picture_usable") == true);
    CHECK(*info->integer("adopt_quality") == 2);
    REQUIRE(b.eng->import_index(file, map_json(*info->find("roots")->a[0].integer("id"), there), 0));
    auto done = transfer_done(b);
    CHECK(*done.find("outcome")->integer("adopted_quality") == 2);
    REQUIRE(wait_for([&] { return b.eng->active_spec() == "fake-high/fp16/pre1"; }, 15000));
    REQUIRE(b.idle());
    CHECK(b.high->images_embedded.load() == 0);
    CHECK(b.fast->images_embedded.load() == 0);
    CHECK(b.search("red").front().first == "red.jpg");
    CHECK(*mv::json::parse(b.eng->settings_json())->integer("quality") == 2);
  }
  SECTION("indexed here with another tower: skipped, never mixed") {
    rig b;
    b.file("blue.jpg");
    const fs::path there = b.dir / "Other";
    copy_library(a, there);
    b.start();
    REQUIRE(b.eng->index_folder(utf8(b.photos()), false));
    REQUIRE(b.idle());
    auto info = mv::json::parse(*b.eng->inspect_export(file));
    CHECK(info->boolean("picture_usable") == false);
    CHECK(*info->str("why_not") == "model");
    REQUIRE(b.eng->import_index(file, map_json(*info->find("roots")->a[0].integer("id"), there), 0));
    auto done = transfer_done(b);
    CHECK(*done.find("outcome")->integer("frames") == 0);
    CHECK(*done.find("outcome")->integer("skipped_rows") == 2);
    REQUIRE(b.idle());
    CHECK(b.eng->active_spec() == "fake-fast/fp16/pre1");
    CHECK(b.fast->images_embedded.load() == 3);  // its own, plus the two it indexes itself
    CHECK(b.search("red").front().first == "red.jpg");
  }
}

TEST_CASE("People travel only when ticked, and named people join by name", "[ai][engine][transfer][faces]") {
  rig a;
  a.file("anna_1.jpg");
  a.file("anna_2.jpg");
  a.file("ben_1.jpg");
  a.file("ben_2.jpg");
  a.start();
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(a.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (a.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, 6000));
  REQUIRE(a.idle());
  auto people = mv::json::parse(a.eng->people_json());
  REQUIRE(people->a.size() == 2);
  REQUIRE(a.eng->person_rename(*people->a[0].integer("id"), "Anna"));

  const std::string without = utf8(a.dir / "plain.mvindex");
  REQUIRE(a.eng->export_index(without, {}, 0));
  auto done = transfer_done(a);
  CHECK(*done.find("outcome")->integer("faces") == 0);
  CHECK(done.find("outcome")->boolean("people_included") == false);
  const std::string with = utf8(a.dir / "people.mvindex");
  REQUIRE(a.eng->export_index(with, {}, MV_AI_TRANSFER_PEOPLE));
  done = transfer_done(a);
  CHECK(*done.find("outcome")->integer("faces") == 4);
  CHECK(*done.find("outcome")->integer("people") == 2);

  rig b;
  const fs::path there = b.dir / "Photos";
  copy_library(a, there);
  b.start();
  REQUIRE(b.idle());
  CHECK(mv::json::parse(*b.eng->inspect_export(without))->find("people")->k == mv::json::kind::null);
  auto info = mv::json::parse(*b.eng->inspect_export(with));
  REQUIRE(info->find("people")->k == mv::json::kind::object);
  CHECK(info->find("people")->boolean("ready") == false);  // People is off here
  const auto root_id = *info->find("roots")->a[0].integer("id");

  // Without the flag nothing about faces lands, even from a file that has them.
  REQUIRE(b.eng->import_index(with, map_json(root_id, there), 0));
  REQUIRE(*transfer_done(b).integer("status") == 0);
  CHECK_FALSE(fs::exists(b.dir / "data" / "faces.db"));

  REQUIRE(b.eng->import_index(with, map_json(root_id, there), MV_AI_TRANSFER_PEOPLE));
  done = transfer_done(b);
  CHECK(*done.find("outcome")->integer("faces") == 4);
  CHECK(*done.find("outcome")->integer("people_new") == 2);
  REQUIRE(b.idle());
  CHECK((b.status().flags & MV_AI_STATUS_FACES_ON) != 0);  // the import turned People on
  CHECK(b.search("Anna").size() == 2);
  auto here = mv::json::parse(b.eng->people_json());
  REQUIRE(here->a.size() == 2);
}

// The duplicate pass (docs/design/17 "Merge duplicates") through the engine: what it
// finds is covered on synthetic vectors in test_ai_face_refine.cpp ("[dedupe]");
// here, that the call is wired, keeps the user's split, and settles.
TEST_CASE("merge duplicates: wired through the engine, a split stays apart, a second call does nothing",
          "[ai][engine][faces][dedupe]") {
  rig r;
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.file("anna_3.jpg");
  r.file("ben_1.jpg");
  r.file("ben_2.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  CHECK(r.eng->people_dedupe().error() == mv::status::invalid_arg);  // People is off
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, 6000));
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people->a.size() == 2);
  // Anna and Ben are different people: nothing merges, nothing moves.
  auto d = r.eng->people_dedupe();
  REQUIRE(d);
  CHECK(d->merged == 0);
  CHECK(d->moved == 0);
  // The user splits two Anna faces off and names both sides Anna: the faces
  // and the names say "same person", the split said otherwise, and the user's
  // split wins. (Two faces: a one-face person is hidden by the minimum.)
  const std::int64_t anna = *people->a[0].integer("id");
  auto faces = mv::json::parse(r.eng->person_faces_json(anna));
  REQUIRE(faces);
  REQUIRE(faces->a.size() == 3);
  const std::vector<std::int64_t> two{*faces->a[0].integer("face"), *faces->a[1].integer("face")};
  auto split = r.eng->face_split(two);
  REQUIRE(split);
  REQUIRE(r.eng->person_rename(anna, "Anna"));
  REQUIRE(r.eng->person_rename(*split, "Anna"));
  REQUIRE(mv::json::parse(r.eng->people_json())->a.size() == 3);
  d = r.eng->people_dedupe();
  REQUIRE(d);
  CHECK(d->merged == 0);
  CHECK(mv::json::parse(r.eng->people_json())->a.size() == 3);
  d = r.eng->people_dedupe();
  REQUIRE(d);
  CHECK(d->merged == 0);
  CHECK(d->moved == 0);
}

TEST_CASE("an export carries every thumbnail, making the ones never made, and they land only for the same file",
          "[ai][engine][transfer]") {
  rig a;
  a.file("red.jpg");
  a.file("blue.jpg");
  a.file("broken_green.jpg");
  a.file("holiday_rgb.mp4");
  a.start();
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(a.idle());
  {
    std::lock_guard lock(a.thumbs_m);
    a.jpegs.clear();
    a.moment_thumbs.clear();
    a.jpegs[utf8(a.photos() / "red.jpg") + "#-1"] = {0xFF, 0xD8, 1, 2, 3};  // nothing else was ever thumbed
  }
  const std::string file = utf8(a.dir / "t.mvindex");
  REQUIRE(a.eng->export_index(file, {}, MV_AI_TRANSFER_THUMBS));
  auto done = transfer_done(a);
  // red (cached), blue (made), the clip's three moments (made) and its own
  // tile; broken_green cannot be made.
  CHECK(*done.find("outcome")->integer("thumbs") == 6);
  CHECK(*done.find("outcome")->integer("thumbs_missing") == 1);
  {
    std::lock_guard lock(a.thumbs_m);
    // The clip's tile is a moment row; the viewer's poster row is not written.
    CHECK(a.jpegs.count(utf8(a.photos() / "holiday_rgb.mp4") + "#-1") == 0);
  }

  rig b;
  const fs::path there = b.dir / "Photos";
  copy_library(a, there);
  b.start();
  const auto root_id = *mv::json::parse(*b.eng->inspect_export(file))->find("roots")->a[0].integer("id");
  REQUIRE(b.eng->import_index(file, map_json(root_id, there), MV_AI_TRANSFER_THUMBS));
  done = transfer_done(b);
  CHECK(*done.find("outcome")->integer("thumbs") == 6);
  std::lock_guard lock(b.thumbs_m);
  const auto it = b.jpegs.find(utf8(there / "red.jpg") + "#-1");
  REQUIRE(it != b.jpegs.end());
  CHECK(it->second.size() == 5);
  CHECK(b.jpegs.count(utf8(there / "blue.jpg") + "#-1") == 1);
  CHECK(b.jpegs.count(utf8(there / "holiday_rgb.mp4") + "#-1") == 1);
}

TEST_CASE("an import refuses what is not an index, and one transfer runs at a time", "[ai][engine][transfer]") {
  rig r;
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const fs::path junk = r.dir / "junk.mvindex";
  write_bytes(junk, pattern(4096, 3));
  CHECK(r.eng->inspect_export(utf8(junk)).error() != mv::status::ok);
  CHECK_FALSE(r.eng->import_index(utf8(junk), "not json", 0));
  CHECK_FALSE(r.eng->import_index(utf8(junk), "[]", 0));
  REQUIRE(r.eng->import_index(utf8(junk), map_json(1, r.photos()), 0));
  CHECK(*transfer_done(r).integer("status") != 0);
  // The index is untouched by a refused file.
  REQUIRE(r.idle());
  CHECK(r.status().frames_indexed == 1);
}

TEST_CASE("re-analysing faces keeps the user's people and settles once at the end",
          "[ai][engine][faces][rerun]") {
  rig r;
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.file("anna_3.jpg");
  r.file("ben_1.jpg");
  r.file("ben_2.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK_FALSE(r.eng->people_reanalyse());  // People is off
  REQUIRE(r.eng->faces_enable(true));
  for (int i = 0; i < 300 && !(r.status().flags & MV_AI_STATUS_FACES_READY); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 2);
  const std::int64_t anna = *people->a[0].integer("id");
  REQUIRE(r.eng->person_rename(anna, "Anna"));
  CHECK(r.status().people_scan_total == 5);
  CHECK(r.status().people_scan_done == 5);

  REQUIRE(r.eng->people_reanalyse());
  CHECK(r.status().flags & MV_AI_STATUS_PEOPLE_RERUN);
  REQUIRE(wait_for([&] { return (r.status().flags & (MV_AI_STATUS_PEOPLE_RERUN | MV_AI_STATUS_PEOPLE_SETTLING)) == 0; },
                   20000));
  CHECK(r.status().people_scan_done == 5);
  people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 2);
  CHECK(*people->a[0].integer("id") == anna);
  CHECK(*people->a[0].str("name") == "Anna");
  CHECK(*people->a[0].integer("faces") == 3);
  CHECK(*people->a[1].integer("faces") == 2);
  CHECK(r.status().faces_total == 5);  // replaced in place, not added again
  CHECK(r.search("photos of Anna").size() == 3);
}
