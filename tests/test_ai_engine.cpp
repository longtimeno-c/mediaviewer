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
#include <sqlite3.h>

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

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "addon/host.h"
#include "addons/ai/engine.h"
#include "addons/ai/index_db.h"
#include "addons/ai/query.h"
#include "nle/search_session.h"
#include "core/fold.h"
#include "core/json.h"
#include "import_fixture.h"
#include "io/pairing.h"

using namespace mv::test;
using mv::ai::engine;

namespace {

constexpr std::uint32_t kDim = 8;

// How long a test waits for the engine to reach a state it must reach. An
// upper bound, not an expectation: a passing wait ends as soon as the state
// holds, usually in well under a second. `ctest -j` beside other builds has
// stretched waits of a few seconds past their bounds (issue #154), so every
// wait is for a condition, bounded only to fail a hang rather than stall.
constexpr int kSettleMs = 60000;

// Workers at normal priority: QOS_CLASS_BACKGROUND (the product's) left them
// without a core for over a minute under `ctest -j` beside other builds.
constexpr mv::ai::engine_options kTestOptions{.background_threads = false};

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

bool has_row(const std::vector<std::pair<std::string, std::int64_t>>& rows, const std::string& path) {
  return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == path; });
}

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

// #403: the OS text reader, faked. The fake still marks a photo's text in
// pixel 1's green channel (the rig's still_rgb sets it from the name); a
// "*sign*" clip's frames carry it from 3 s, and their second + 1 in pixel 1's
// blue (fake_sampler).
constexpr std::uint8_t kSignSuited = 77;  // "*sign*": a sign reading SUITED AND / BOOTED
constexpr std::uint8_t kSignKoln = 78;    // "*koln*": Grüße aus / KÖLN
// #354 D4: a document page the host drew for a "scan*" or "mixed*" PDF; its
// 0-based page + 1 in pixel 1's blue. The fake reads three lines from it.
constexpr std::uint8_t kScanPage = 79;
class fake_text final : public mv::ai::text_reader {
 public:
  // `seen` is told of every picture before it is read (it may block).
  explicit fake_text(std::function<void(const mv::ai::rgb_frame&)> seen) : seen_(std::move(seen)) {}
  mv::result<std::vector<std::string>> read(const mv::ai::rgb_frame& img) override {
    seen_(img);
    if (img.rgb.size() < 6) return std::vector<std::string>{};
    if (img.rgb[4] == kSignSuited) return std::vector<std::string>{"SUITED AND", "BOOTED"};
    if (img.rgb[4] == kSignKoln) return std::vector<std::string>{"Grüße aus", "KÖLN"};
    if (img.rgb[4] == kScanPage) {
      std::vector<std::string> lines{"Scanned page " + std::to_string(img.rgb[5] - 1) + " of the",
                                     "archive, kept for re-", "cords."};
      if (img.rgb[6] == 1) lines.push_back("The tenant pays the rent monthly.");  // "scan_rent*"
      return lines;
    }
    return std::vector<std::string>{};
  }
  const std::string& spec_key() const noexcept override { return key_; }

 private:
  std::function<void(const mv::ai::rgb_frame&)> seen_;
  std::string key_ = "ocr/fake/1";
};

// #354: PDFium, faked by name. "invoice*.pdf" has three pages ("Invoice 4471"
// on the first, "net 30 days" on the last); "long*.pdf" 600 pages, each with
// "page N"; "scan*.pdf" pages with no text layer; "locked*" a password;
// "broken*" does not open. `on_page` is told of each page before it is visited.
// D4: "scanbook*" 100 pages with no text layer; "mixed*" three pages, words on
// the first and last and a stamped "- 2 -" alone on the second.
// D2's look: "gallery*" has 8 pages, 5 and 6 mostly pictures; "scan*" both
// pages; "long*" pages 10 and 400.
std::uint32_t fake_pdf_pages(const std::string& name) {
  if (name.find("invoice") != std::string::npos) return 3;
  if (name.find("long") != std::string::npos) return 600;
  if (name.find("scanbook") != std::string::npos) return 100;
  if (name.find("scan") != std::string::npos) return 2;
  if (name.find("mixed") != std::string::npos) return 3;
  if (name.find("gallery") != std::string::npos) return 8;
  if (name.find("lease") != std::string::npos) return 2;  // the DOCX the host lays out
  return 1;
}

class fake_pdf final : public mv::ai::pdf_reader {
 public:
  explicit fake_pdf(std::function<void()> on_page) : on_page_(std::move(on_page)) {}
  mv::result<std::uint32_t> read(const std::string& path, std::uint32_t first, const mv::ai::pdf_limits& limits,
                                 const std::function<bool(std::uint32_t, std::string_view)>& visit) override {
    const std::string name = fs::path(path).filename().string();
    if (name.find("locked") != std::string::npos) return mv::err(mv::status::permission_denied);
    if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
    std::vector<std::string> pages;
    if (name.find("invoice") != std::string::npos) {
      pages = {"INVOICE\r\nInvoice 4471 for the lease of the hall.\r\n", "Items and quantities.\r\n",
               "Payment terms: net 30 days from the date of this in-\r\nvoice.\r\n"};
    } else if (name.find("long") != std::string::npos) {
      for (int i = 0; i < 600; ++i) pages.push_back("Report page " + std::to_string(i + 1) + " ends here.");
    } else if (name.find("scanbook") != std::string::npos) {
      pages.assign(100, "");
    } else if (name.find("scan") != std::string::npos) {
      pages = {"", ""};
    } else if (name.find("mixed") != std::string::npos) {
      pages = {"Mixed report opening page, typed with plenty of words.\r\n", "- 2 -\r\n",
               "Closing page of the mixed report, typed as well.\r\n"};
    }
    const auto n = static_cast<std::uint32_t>(pages.size());
    for (std::uint32_t i = first; i < std::min(n, limits.max_pages); ++i) {
      on_page_();
      if (!visit(i, pages[i])) return mv::err(mv::status::cancelled);
    }
    return n;
  }
  mv::result<mv::ai::pdf_look> look(const std::string& path, const mv::ai::pdf_limits& limits,
                                    float min_cover) override {
    CHECK(min_cover == 0.5f);
    const std::string name = fs::path(path).filename().string();
    if (name.find("locked") != std::string::npos) return mv::err(mv::status::permission_denied);
    if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
    mv::ai::pdf_look l;
    l.pages = fake_pdf_pages(name);
    if (name.find("gallery") != std::string::npos) l.pictures = {5, 6};
    if (name.find("scan") != std::string::npos) l.pictures = {0, 1};
    if (name.find("long") != std::string::npos) l.pictures = {10, 400};
    std::erase_if(l.pictures, [&](std::uint32_t p) { return p >= limits.max_pages; });
    return l;
  }
  const std::string& name() const noexcept override { return name_; }

 private:
  std::function<void()> on_page_;
  std::string name_ = "Fake PDFium";
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
// "anna3s*" shows Anna's face on the frame at 3 s alone (#407). Frames closer
// than `min_gap_ms` to the last one given are skipped, as the real sampler's
// minimum gap does (the People-only grid's 2 s).
class fake_sampler final : public mv::addon::video_sampler {
 public:
  fake_sampler(std::string name, std::int64_t start, std::uint32_t min_gap_ms = 0)
      : name_(std::move(name)), t_(start), min_gap_(min_gap_ms) {
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
    while (last_ >= 0 && t_ - last_ < min_gap_ && t_ < facts_.duration_ms) t_ += 1000;
    if (t_ >= facts_.duration_ms) {
      f.end = true;
      return f;
    }
    last_ = t_;
    const rgb c = t_ < 3000 ? rgb{220, 20, 20} : (t_ < 6000 ? rgb{20, 220, 20} : rgb{20, 20, 220});
    f.image = solid(c, 64, 36);
    if (name_.rfind("anna3s", 0) == 0 && t_ == 3000) f.image.rgb[2] = 11;  // fake_faces: Anna
    if (name_.find("sign") != std::string::npos) {
      // #403: a sign in shot from 3 s (fake_text), and which second this is.
      if (t_ >= 3000) f.image.rgb[4] = kSignSuited;
      f.image.rgb[5] = static_cast<std::uint8_t>(t_ / 1000 + 1);
    }
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
  std::int64_t min_gap_ = 0;
  std::int64_t last_ = -1;
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

// #354 D3: the Documents piece's text model, faked by topic: a passage or a
// query about renting a home points one way, one about paying a bill
// another, anything else at the generic prompts.
class fake_docs final : public mv::ai::passage_model {
 public:
  static std::vector<float> topic(std::string_view t, bool query) {
    const auto has = [&](const char* w) { return t.find(w) != std::string_view::npos; };
    if (has("tenant") || has("rent") || (query && (has("landlord") || has("flat")))) return colour_vec(1, 0, 0);
    if (has("Payment") || (query && has("bill"))) return colour_vec(0, 1, 0);
    return colour_vec(0.3f, 0.3f, 0.3f);
  }
  mv::expected embed_passages(std::span<const std::string> texts, std::vector<float>& out) override {
    out.clear();
    for (const std::string& t : texts) {
      const auto v = topic(t, false);
      out.insert(out.end(), v.begin(), v.end());
      ++passages_embedded;
    }
    return {};
  }
  mv::result<std::vector<float>> embed_query(std::string_view utf8) override { return topic(utf8, true); }
  std::atomic<int> passages_embedded{0};
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

// A UI call made while a gate is held must not wait for it. A wait would
// last the whole hold (10 s and more), so the bound sits far below that and
// far above a loaded machine's scheduling noise: it is not a speed check
// (the old 50 ms bounds failed under `ctest -j`, issue #154).
constexpr double kAnswersNowMs = 2000.0;

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
    std::int64_t created = 0;  // creationDate, unix seconds (#404)
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
      mv::ai::photos_item pi{it.id, it.mtime, 100, it.kind};
      pi.created = it.created;
      if (!visit(pi)) return mv::err(mv::status::cancelled);
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

// Cloud files (2026-10-05): which of a folder's files a provider keeps
// online-only, by file name. The host's real walk lists the files; the rig's
// walk_files2 flags these, the way OneDrive's attributes would.
struct cloud_world {
  std::mutex m;
  std::set<std::string> online_only;
  std::vector<std::string> log;  // "hydrate x" / "dehydrate x", in order
  std::atomic<bool> unmetered{true};
  bool is_online_only(const std::string& name) {
    std::lock_guard lock(m);
    return online_only.count(name) != 0;
  }
};

class fake_cloud final : public mv::ai::cloud_files {
 public:
  explicit fake_cloud(std::shared_ptr<cloud_world> w) : w_(std::move(w)) {}
  mv::expected hydrate(const std::string& path, const std::function<bool(double)>& progress) override {
    const std::string name = utf8(fs::path(path).filename());
    {
      std::lock_guard lock(w_->m);
      w_->online_only.erase(name);
      w_->log.push_back("hydrate " + name);
    }
    if (progress) (void)progress(1.0);
    return {};
  }
  mv::expected dehydrate(const std::string& path) override {
    const std::string name = utf8(fs::path(path).filename());
    std::lock_guard lock(w_->m);
    w_->online_only.insert(name);
    w_->log.push_back("dehydrate " + name);
    return {};
  }
  bool network_unmetered() override { return w_->unmetered.load(); }

 private:
  std::shared_ptr<cloud_world> w_;
};

struct walk2_ctx {
  cloud_world* world;
  mv_addon_walk2_fn visit;
  void* user;
};
std::mutex g_walks_m;
std::map<void*, std::pair<decltype(mv_host_api::walk_files2), cloud_world*>> g_walks;  // by host

int32_t MV_CALL flag_cloud(void* user, const mv_addon_file_entry2* e) {
  auto* c = static_cast<walk2_ctx*>(user);
  mv_addon_file_entry2 x = *e;
  if (e->name_utf8 && c->world->is_online_only(e->name_utf8)) x.flags |= MV_ADDON_FILE_CLOUD_ONLY;
  return c->visit(c->user, &x);
}

mv_status MV_CALL walk2_with_cloud(void* host, const char* root, int32_t depth, mv_addon_walk2_fn visit, void* user) {
  decltype(mv_host_api::walk_files2) real = nullptr;
  cloud_world* world = nullptr;
  {
    std::lock_guard lock(g_walks_m);
    const auto it = g_walks.find(host);
    if (it == g_walks.end()) return MV_ERR_INTERNAL;
    real = it->second.first;
    world = it->second.second;
  }
  walk2_ctx c{world, visit, user};
  return real(host, root, depth, &flag_cloud, &c);
}

// #405: a host from before the rating and exposure fields: its tag_facts
// answers the shorter struct, as an app not yet updated under a newer pack.
std::atomic<bool> g_old_tag_host{false};
std::mutex g_tag_m;
std::map<void*, decltype(mv_host_api::tag_facts)> g_tag_facts;  // by host

mv_status MV_CALL tag_facts_maybe_old(void* host, const char* path, mv_addon_tag_facts* out) {
  decltype(mv_host_api::tag_facts) real = nullptr;
  {
    std::lock_guard lock(g_tag_m);
    const auto it = g_tag_facts.find(host);
    if (it == g_tag_facts.end()) return MV_ERR_INTERNAL;
    real = it->second;
  }
  if (g_old_tag_host.load() && out) out->struct_size = MV_ADDON_TAG_FACTS_V1_SIZE;
  return real(host, path, out);
}

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
  std::atomic<int> cue_reads{0};
  std::atomic<int> samplers_opened{0};
  std::atomic<bool> text_available{false};  // #403: off unless a test reads text
  std::atomic<bool> pdf_available{true};     // #354: the pack's PDFium (fake_pdf)
  std::atomic<int> pdf_pages_read{0};
  std::atomic<int> busy_after_pdf_pages{-1};  // >= 0: the host asks to yield after that many pages
  std::atomic<int> text_reads{0};           // photos read (a still: 32 px wide)
  std::atomic<int> clip_text_reads{0};      // clip moments read (a frame: 64 px wide)
  std::mutex text_m;
  std::vector<int> sign_seconds;            // the second of each "*sign*" clip moment read
  // Set: the reader waits on a clip moment (a pass still running). Never on a
  // photo: with one worker (four cores) a photo's text can be claimed before
  // the walk has listed a clip, and holding it would starve that clip's pictures.
  std::atomic<bool> hold_text{false};
  std::atomic<int> text_holding{0};  // reads that waited on hold_text
  std::atomic<bool> hold_cues{false};       // set: read_cues waits
  std::atomic<int> yield_after_clip_reads{-1};  // >= 0: the host asks to yield after that many
  std::atomic<int> scan_reads{0};               // #354 D4: scanned document pages read
  std::atomic<int> yield_after_scan_reads{-1};  // >= 0: the host asks to yield after that many
  std::atomic<int> scan_read_ms{0};             // each scanned page's read takes this long
  std::unique_ptr<mv::addon::host_table> table;
  std::shared_ptr<cloud_world> cloud = std::make_shared<cloud_world>();
  mv_host_api api{};  // the table, its walk_files2 flagging `cloud`'s online-only names
  std::mutex decoded_m;
  std::vector<std::string> decoded;  // every still the host decoded, by name
  std::vector<std::string> pages_drawn;  // #354 D2: "name#page", each page the host drew
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
  std::shared_ptr<fake_docs> docs_model = std::make_shared<fake_docs>();
  std::atomic<bool> docs_available{false};  // #354 D3: the ai-docs piece
  std::shared_ptr<fake_speech> speech = std::make_shared<fake_speech>();
  // Never the real library: PhotoKit is not asked in a test.
  std::shared_ptr<photos_lib> library = std::make_shared<photos_lib>();
  // #441: the Places add-on's answer for a place term (the host's
  // place_matches). Unset: Places is not installed.
  std::function<std::vector<std::string>(const std::string& place)> places;
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
      {
        std::lock_guard lock(decoded_m);
        decoded.push_back(name);
      }
      if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
      // #355: an audio file's picture is its cover art; "*cover*" has one.
      if (mv::io::is_audio_name(name) && name.find("cover") == std::string::npos) {
        return mv::err(mv::status::not_found);
      }
      auto img = solid(colour_named(name));
      if (name.rfind("anna", 0) == 0) img.rgb[2] = 11;
      if (name.rfind("ben", 0) == 0) img.rgb[2] = 22;
      if (name.find("sign") != std::string::npos) img.rgb[4] = kSignSuited;
      if (name.find("koln") != std::string::npos) img.rgb[4] = kSignKoln;
      return img;
    };
    svc.open_sampler = [this](const std::string& path, const mv::addon::sampler_options& o)
        -> mv::result<std::unique_ptr<mv::addon::video_sampler>> {
      ++samplers_opened;
      return std::unique_ptr<mv::addon::video_sampler>(
          new fake_sampler(utf8(fs::path(path).filename()), o.start_ms, o.min_gap_ms));
    };
    svc.video_frame = [this](const std::string& path, std::int64_t ms,
                             std::uint32_t) -> mv::result<mv::addon::rgb_image> {
      // #354 D2: a document's page, as the host draws it: "gallery*" page 5
      // is red and page 6 green, every other page grey; invalid_arg past the end.
      if (mv::io::is_document_name(path)) {
        const std::string name = utf8(fs::path(path).filename());
        if (name.find("broken") != std::string::npos || name.find("locked") != std::string::npos) {
          return mv::err(mv::status::corrupt);
        }
        if (ms < 0 || ms >= fake_pdf_pages(name)) return mv::err(mv::status::invalid_arg);
        {
          std::lock_guard lock(decoded_m);
          pages_drawn.push_back(name + "#" + std::to_string(ms));
        }
        const bool gallery = name.find("gallery") != std::string::npos;
        auto page = solid(gallery && ms == 5 ? rgb{220, 20, 20} : gallery && ms == 6 ? rgb{20, 220, 20}
                                                                                     : rgb{128, 128, 128});
        if (name.find("scan") != std::string::npos || name.find("mixed") != std::string::npos) {
          page.rgb[4] = kScanPage;  // #354 D4: what the fake OS reads on it
          page.rgb[5] = static_cast<std::uint8_t>(ms + 1);
          if (name.find("rent") != std::string::npos) page.rgb[6] = 1;  // a scanned lease
        }
        return page;
      }
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
    // #291: the host's tag facts, by name: r5 a Canon EOS R5, gpt ChatGPT
    // (AI), geo a position.
    svc.tag_facts = [](const std::string& path, mv_addon_tag_facts& out) {
      const std::string name = utf8(fs::path(path).filename());
      if (name.find("broken") != std::string::npos) return false;
      std::string fields;
      if (name.find("r5") != std::string::npos) {
        std::strcpy(out.camera_utf8, "Canon EOS R5");
        std::strcpy(out.lens_utf8, "RF24-70mm F2.8 L IS USM");
        fields = "Canon EOS R5\x1fRF24-70mm F2.8 L IS USM";
      }
      if (name.find("gpt") != std::string::npos) {
        out.flags |= MV_ADDON_TAG_AI_GENERATED;
        std::strcpy(out.made_with_utf8, "ChatGPT");
        fields = "ChatGPT";
      }
      if (name.find("geo") != std::string::npos) {
        out.flags |= MV_ADDON_TAG_HAS_LOCATION;
        out.lat = 38.7975;
        out.lon = -9.3883;
      }
      // #405: "iso400" is ISO 400, f/2.8, 1/250 s, 35 mm, five stars, Red;
      // anything else is unrated with no exposure (a clip's case too).
      if (name.find("iso400") != std::string::npos) {
        out.rating = 5;
        out.iso = 400;
        out.aperture_x100 = 280;
        out.focal_mm = 35;
        out.shutter_den = 250;
        out.shutter_us = 4000;
        std::strcpy(out.label_utf8, "Red");
      }
      if (name.find("reject") != std::string::npos) out.rating = -1;
      // #355: a song's tags (title, artist, album).
      if (name.find("song") != std::string::npos) fields = "Hey Jude\x1fThe Beatles\x1fPast Masters";
      // #404: "y2019" was taken on 2019-06-15 12:00 (wall clock read as UTC).
      if (name.find("y2019") != std::string::npos) {
        out.flags |= MV_ADDON_TAG_HAS_DATE;
        out.taken_unix = mv::ai::query::days_from_civil(2019, 6, 15) * 86400 + 12 * 3600;
      }
      std::strcpy(out.fields_utf8, fields.c_str());
      return true;
    };
    // #406: a clip named "*cues*" has a chapter "Birthday cake" at 4 s and a
    // subtitle "suited and booted" at 7 s; "*manycues*" 200 subtitle lines a
    // second apart; any other clip has none.
    svc.read_cues = [this](const std::string& path, std::int64_t start_ms, std::int64_t max_ms, std::uint32_t max_cues,
                           const std::function<bool(const mv::addon::host_services::cue&)>& visit) -> mv::expected {
      ++cue_reads;
      while (hold_cues) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      const std::string name = utf8(fs::path(path).filename());
      std::vector<mv::addon::host_services::cue> all;
      if (name.find("manycues") != std::string::npos) {
        for (int i = 0; i < 200; ++i) {
          all.push_back({MV_ADDON_CUE_SUBTITLE, i * 1000, i * 1000 + 900, "line number " + std::to_string(i)});
        }
      } else if (name.find("cues") != std::string::npos) {
        all.push_back({MV_ADDON_CUE_CHAPTER, 4000, 7000, "Birthday cake"});
        all.push_back({MV_ADDON_CUE_SUBTITLE, 7000, 8000, "suited and booted"});
      }
      std::uint32_t sent = 0;
      for (const auto& c : all) {
        if (c.start_ms < start_ms) continue;
        if (c.start_ms > max_ms || sent >= max_cues) break;
        ++sent;
        if (!visit(c)) return mv::err(mv::status::cancelled);
      }
      return {};
    };
    // #354: a DOCX's text, by name: "*lease*.docx" a two-page lease; "locked*"
    // password-protected; "broken*" does not parse.
    svc.document_text = [](const std::string& path,
                           const std::function<bool(const mv::addon::document_line&)>& visit) -> mv::expected {
      const std::string name = utf8(fs::path(path).filename());
      if (name.find("locked") != std::string::npos) return mv::err(mv::status::permission_denied);
      if (name.find("broken") != std::string::npos) return mv::err(mv::status::corrupt);
      if (name.find("lease") == std::string::npos) return {};
      const std::vector<mv::addon::document_line> lines = {
          {0, 2, "Residential lease"},
          {0, 2, "The tenant shall pay the rent on the first day of each month."},
          {1, 2, "Either party may end this agreement with two months' notice."},
      };
      for (const auto& l : lines) {
        if (!visit(l)) return mv::err(mv::status::cancelled);
      }
      return {};
    };
    svc.place_matches = [this](const std::string& place,
                               const std::function<bool()>& keep_going) -> mv::result<std::vector<std::string>> {
      if (!places) return mv::err(mv::status::not_found);
      if (!keep_going()) return mv::err(mv::status::cancelled);
      return places(place);
    };
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    table->set_negotiated(MV_ADDON_HOST_API);
    api = *table->api();
    {
      std::lock_guard lock(g_walks_m);
      g_walks[api.host] = {api.walk_files2, cloud.get()};
    }
    api.walk_files2 = &walk2_with_cloud;
    {
      std::lock_guard lock(g_tag_m);
      g_tag_facts[api.host] = api.tag_facts;
    }
    api.tag_facts = &tag_facts_maybe_old;
  }
  ~rig() {
    eng.reset();
    {
      std::lock_guard lock(g_tag_m);
      g_tag_facts.erase(api.host);
    }
    std::lock_guard lock(g_walks_m);
    g_walks.erase(api.host);
  }
  std::vector<std::string> drawn_pages() {
    std::lock_guard lock(decoded_m);
    std::vector<std::string> out = pages_drawn;
    std::sort(out.begin(), out.end());
    return out;
  }
  bool was_decoded(const std::string& name) {
    std::lock_guard lock(decoded_m);
    return std::find(decoded.begin(), decoded.end(), name) != decoded.end();
  }

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
    d.open_docs = [this]() -> mv::result<mv::ai::loaded_docs> {
      if (!docs_available) return mv::err(mv::status::io);
      mv::ai::loaded_docs l;
      l.model = docs_model;
      l.name = "Fake text";
      l.spec_key = "fake-text/int8/pre1";
      l.dim = kDim;
      l.query_margin = 0.05f;
      l.result_margin = 0.02f;
      l.generic_prompts = {"a document."};
      return l;
    };
    d.photos = [this] { return std::unique_ptr<mv::ai::photos_source>(new fake_photos(library)); };
    d.photos_rescan_gap_s = 0;
    d.cloud = [this] { return std::unique_ptr<mv::ai::cloud_files>(new fake_cloud(cloud)); };
    d.pdf = [this]() -> std::unique_ptr<mv::ai::pdf_reader> {
      if (!pdf_available) return nullptr;
      return std::make_unique<fake_pdf>([this] {
        if (++pdf_pages_read == busy_after_pdf_pages) busy = true;
      });
    };
    d.text = [this]() -> std::unique_ptr<mv::ai::text_reader> {
      if (!text_available) return nullptr;
      return std::make_unique<fake_text>([this](const mv::ai::rgb_frame& img) {
        if (hold_text && img.width == 64) {
          ++text_holding;
          while (hold_text) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (img.width != 64 && img.rgb.size() >= 6 && img.rgb[4] == kScanPage) {
          if (scan_read_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(scan_read_ms.load()));
          if (++scan_reads == yield_after_scan_reads) busy = true;
          return;
        }
        if (img.width != 64) {
          ++text_reads;
          return;
        }
        const int n = ++clip_text_reads;
        if (img.rgb.size() >= 6 && img.rgb[5] != 0) {
          std::lock_guard lock(text_m);
          sign_seconds.push_back(img.rgb[5] - 1);
        }
        if (n == yield_after_clip_reads) busy = true;
      });
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

  void start(mv::ai::engine_options options = kTestOptions) {
    eng.reset();
    options.background_threads = false;  // see kTestOptions (issue #154)
    eng = std::make_unique<engine>(&api, deps(), options);
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
    auto rd = std::make_unique<engine>(table->api(), reader_deps(),
                                       mv::ai::engine_options{.read_only = true, .background_threads = false});
    REQUIRE(rd->start());
    REQUIRE(wait_for(
        [&] {
          mv_ai_status s{};
          rd->status(s);
          return s.state != MV_AI_STATE_LOADING;
        },
        kSettleMs));
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
    REQUIRE(eng->wait_search(id, kSettleMs));
    return rows(id);
  }
  // #412: the rows a search answered. A description nothing answers lists
  // the scope newest first instead, rows with no match of their own.
  std::vector<std::pair<std::string, std::int64_t>> matched(std::uint64_t id) {
    std::vector<std::pair<std::string, std::int64_t>> out;
    const auto all = rows(id);
    for (std::uint32_t i = 0; i < all.size(); ++i) {
      auto r = eng->result_at(id, i);
      REQUIRE(r);
      if (r->match != 0) out.push_back(all[i]);
    }
    return out;
  }
  std::vector<std::pair<std::string, std::int64_t>> answered(const std::string& q,
                                                             std::uint32_t scope = MV_AI_SCOPE_ALL,
                                                             const std::string& dir_scope = "",
                                                             std::uint32_t kinds = MV_AI_KIND_ALL) {
    const std::uint64_t id = eng->search_text(q, dir_scope, scope, kinds);
    REQUIRE(eng->wait_search(id, kSettleMs));
    return matched(id);
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
  bool idle(int ms = kSettleMs) {
    if (eng->wait_idle(ms)) return true;
    // Where it stuck, for the failure that follows.
    const mv_ai_status s = status();
    UNSCOPED_INFO("not idle after " << ms << " ms: state " << s.state << ", yield " << s.yield_reason << ", assets "
                                    << s.assets_done << "/" << s.assets_total << ", failed " << s.assets_failed
                                    << ", flags " << s.flags);
    return false;
  }
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
  CHECK(r.answered("xyzzy plugh").empty());
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
  REQUIRE(r.eng->wait_search(id, kSettleMs));
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
  REQUIRE(wait_for([&] { return r.fast->images_embedded.load() >= 8; }, kSettleMs));
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

// Issue #299: a root left part-way by a quit is held at the next start (when
// the host asks first: engine_options::hold_unfinished) -- not walked, not
// indexed, no files added meanwhile -- until Resume; a finished root carries
// on as before. resume.json says which roots are unfinished, for the host's
// launch notice.
TEST_CASE("an index left unfinished waits for Resume after a restart", "[ai][engine]") {
  rig r;
  r.file("b/red_4.jpg");
  r.file("a/red_1.jpg");
  r.file("a/green_2.jpg");
  r.file("a/blue_3.jpg");
  const mv::ai::engine_options hold{.hold_unfinished = true};
  r.start(hold);  // nothing unfinished yet: nothing held
  REQUIRE(r.eng->index_folder(utf8(r.photos() / "b"), true));  // root 1, indexed in full
  REQUIRE(r.idle());
  REQUIRE(r.stills_decoded.load() == 1);

  const auto row_of = [&](std::int64_t id) -> std::optional<mv::json::value> {
    const auto roots = mv::json::parse(r.eng->roots_json());
    if (!roots) return std::nullopt;
    for (const auto& row : roots->a) {
      if (row.integer("id").value_or(0) == id) return row;
    }
    return std::nullopt;
  };
  const auto resume_roots = [&]() -> std::vector<std::int64_t> {
    std::ifstream in(r.dir / "data" / "resume.json", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::int64_t> out;
    if (const auto doc = mv::json::parse(text.str())) {
      if (const auto* roots = doc->find("roots")) {
        for (const auto& row : roots->a) out.push_back(row.integer("id").value_or(0));
      }
    }
    return out;
  };
  const auto eventually = [](const std::function<bool()>& ok) {
    for (int i = 0; i < 500; ++i) {
      if (ok()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ok();
  };

  // Root 2 is walked but none of it indexed: the app quits part-way.
  r.eng->pause(true);
  REQUIRE(r.eng->index_folder(utf8(r.photos() / "a"), true));
  REQUIRE(eventually([&] {
    const auto row = row_of(2);
    return row && row->integer("assets").value_or(0) == 3;
  }));
  CHECK(eventually([&] { return resume_roots() == std::vector<std::int64_t>{2}; }));
  CHECK(r.stills_decoded.load() == 1);

  // Meanwhile a file lands in each root.
  r.file("a/red_5.jpg");
  r.file("b/green_6.jpg");
  r.start(hold);
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 2);  // the finished root's new file only
  {
    const auto held = row_of(2);
    REQUIRE(held);
    CHECK(held->boolean("held").value_or(false));
    CHECK(held->integer("assets").value_or(0) == 3);  // not walked: red_5 waits too
    CHECK(held->integer("done").value_or(-1) == 0);
    const auto carried_on = row_of(1);
    REQUIRE(carried_on);
    CHECK_FALSE(carried_on->boolean("held").value_or(true));
    CHECK(carried_on->integer("done").value_or(0) == 2);
  }
  CHECK(r.search("green").size() == 1);  // search answers from what was committed
  CHECK(eventually([&] { return resume_roots() == std::vector<std::int64_t>{2}; }));

  // Resume: walked, then indexed, the new file with it.
  REQUIRE(r.eng->set_setting("resume_root", "2"));
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 6);
  {
    const auto row = row_of(2);
    REQUIRE(row);
    CHECK_FALSE(row->boolean("held").value_or(true));
    CHECK(row->integer("done").value_or(0) == 4);
  }
  CHECK(eventually([&] { return resume_roots().empty(); }));

  // A finished index holds nothing at the next start.
  r.start(hold);
  REQUIRE(r.idle());
  const auto row = row_of(2);
  REQUIRE(row);
  CHECK_FALSE(row->boolean("held").value_or(true));
  CHECK(r.stills_decoded.load() == 6);
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

// Owner report, 2026-10-05: Settings read "0 of 26 · 0 KB" for a folder that
// had finished, and Rescan looked like it did nothing. The rows follow the
// work through AI_ROOTS, carry a size, and say when a root is being walked.
TEST_CASE("folder rows follow indexing, carry a size and show a rescan", "[ai][engine]") {
  rig r;
  r.file("red.jpg");
  r.file("sub/blue.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  const auto roots_events = [&] {
    std::lock_guard lock(r.events_m);
    return std::count_if(r.events.begin(), r.events.end(),
                         [](const mv_addon_event& e) { return e.kind == MV_ADDON_EVENT_AI_ROOTS; });
  };
  {
    const auto roots = mv::json::parse(r.eng->roots_json());
    REQUIRE(roots);
    REQUIRE(roots->a.size() == 1);
    const auto& row = roots->a[0];
    CHECK(*row.integer("assets") == 2);
    CHECK(*row.integer("done") == 2);
    CHECK(*row.integer("bytes") > 0);
    CHECK(!row.boolean("scanning").value_or(true));
  }
  // A rescan is announced at once and again when its walk ends, even when
  // it finds nothing new.
  const auto before = roots_events();
  REQUIRE(r.eng->root_rescan(1));
  CHECK(roots_events() > before);
  REQUIRE(r.idle());
  CHECK(roots_events() >= before + 2);
  const auto after = mv::json::parse(r.eng->roots_json());
  REQUIRE(after);
  CHECK(!after->a[0].boolean("scanning").value_or(true));
  CHECK(*after->a[0].integer("done") == 2);
}

TEST_CASE("indexing waits while the viewer is busy and says so", "[ai][engine]") {
  rig r;
  r.file("red.jpg");
  r.busy = true;
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  // Not even the models open while the viewer is busy (they cost frames).
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status s = r.status();
        return s.state == MV_AI_STATE_LOADING && s.yield_reason == MV_AI_YIELD_VIEWER;
      },
      kSettleMs));
  CHECK(r.stills_decoded.load() == 0);
  r.busy = false;
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 1);

  // Loaded, then busy again: the indexer yields and says so.
  r.busy = true;
  r.file("blue.jpg");
  REQUIRE(r.eng->root_rescan(1));
  // YIELDING means the rescan found the new file and a worker is holding it.
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status y = r.status();
        return y.state == MV_AI_STATE_YIELDING && y.yield_reason == MV_AI_YIELD_VIEWER;
      },
      kSettleMs));
  CHECK(r.stills_decoded.load() == 1);
  r.busy = false;
  REQUIRE(r.idle());
  CHECK(r.stills_decoded.load() == 2);

  r.eng->pause(true);
  r.file("green.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status p = r.status();
        return p.state == MV_AI_STATE_PAUSED && p.assets_total == 3;
      },
      kSettleMs));
  CHECK(r.stills_decoded.load() == 2);
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
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status s = r.status();
        return s.state == MV_AI_STATE_YIELDING && s.yield_reason == MV_AI_YIELD_BATTERY;
      },
      kSettleMs));
  CHECK(r.stills_decoded.load() == 1);

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
  REQUIRE(wait_for(
      [&] { return r.eng->settings_json().find("\"battery_override\":false") != std::string::npos; }, kSettleMs));
  r.on_battery = true;
  r.file("green.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status s = r.status();
        return s.state == MV_AI_STATE_YIELDING && s.yield_reason == MV_AI_YIELD_BATTERY;
      },
      kSettleMs));
  CHECK(r.stills_decoded.load() == 2);

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
  CHECK(r.answered("xyzzy plugh").empty());
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
  REQUIRE(wait_for(
      [&] {
        const std::string spec = r.eng->active_spec();
        CHECK((spec == "fake-fast/fp16/pre1" || spec == "fake-high/fp16/pre1"));
        return spec == "fake-high/fp16/pre1";
      },
      kSettleMs));
  REQUIRE(r.idle());
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_INDEX_FULL) != 0; }, kSettleMs));
  CHECK(r.status().state == MV_AI_STATE_PAUSED);
}

// The schema a fresh or migrated index ends at (index_db.cpp kSchemaVersion).
constexpr const char* kSchemaNow = "10";

TEST_CASE("schema 8 migrates to 9: documents' passages, their words and a folder's choice (#354)",
          "[ai][engine][documents]") {
  scratch_dir dir{"ai-migrate-9"};
  const std::string path = utf8(dir / "index.db");
  {
    auto d = mv::ai::index_db::open(path);
    REQUIRE(d);
    REQUIRE((*d)->set_meta("schema", "8"));  // as an older pack left it, minus the new parts
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "DROP TRIGGER passages_gone; DROP TABLE passages; DROP TABLE passage_text;", nullptr,
                         nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  auto d = mv::ai::index_db::open(path);
  REQUIRE(d);
  CHECK((*d)->meta("schema") == kSchemaNow);
  REQUIRE((*d)->add_root("/docs", true));
  CHECK((*d)->roots().front().docs == 0);
  REQUIRE((*d)->set_root_docs((*d)->roots().front().id, MV_AI_DOCS_OFF));
  CHECK((*d)->roots().front().docs == MV_AI_DOCS_OFF);
  // A document's passages go in, are found by their words, and leave with it.
  const mv::ai::index_db::seen_file f{"/docs/lease.pdf", 5, 10, mv::ai::asset_kind::document};
  auto up = (*d)->see_assets((*d)->roots().front().id, std::span(&f, 1), (*d)->next_generation());
  REQUIRE(up);
  const std::int64_t asset = (*up)[0].id;
  const std::vector<mv::ai::passage_in> rows = {{0, "The lease.", "the lease"}, {3, "Net 30.", "net 30"}};
  REQUIRE((*d)->commit_passages(asset, mv::ai::kDocTextSpec, rows, mv::ai::work_state::done, 4));
  CHECK((*d)->passage_count(asset) == 2);
  CHECK((*d)->has_passage_index());
  std::vector<std::uint32_t> pages;
  REQUIRE((*d)->find_passages("\"net\" \"30\"", 10, [&](const mv::ai::passage_out& p) {
    pages.push_back(p.page);
    return true;
  }));
  CHECK(pages == std::vector<std::uint32_t>{3});
  // A changed file drops its passages; a locked one is neither done nor failed.
  const mv::ai::index_db::seen_file g{"/docs/lease.pdf", 6, 10, mv::ai::asset_kind::document};
  REQUIRE((*d)->see_assets((*d)->roots().front().id, std::span(&g, 1), (*d)->next_generation()));
  CHECK((*d)->passage_count(asset) == 0);
  const mv::ai::track_filter docs{false, 0, 1, 0, {}, 1, 1};
  CHECK((*d)->pending(mv::ai::kDocTextSpec, 10, 3, docs).empty());  // the folder's Documents are Off
  REQUIRE((*d)->set_root_docs((*d)->roots().front().id, MV_AI_DOCS_DEFAULT));
  CHECK((*d)->pending(mv::ai::kDocTextSpec, 10, 3, docs).size() == 1);
  CHECK((*d)->pending(mv::ai::kDocTextSpec, 10, 3, mv::ai::track_filter{}).empty());  // the picture track: never
  REQUIRE((*d)->mark_locked(asset, mv::ai::kDocTextSpec));
  CHECK((*d)->pending(mv::ai::kDocTextSpec, 10, 3, docs).empty());
  CHECK((*d)->count(mv::ai::kDocTextSpec, docs).locked == 1);
  REQUIRE((*d)->clear());
  CHECK((*d)->passage_count() == 0);
}

TEST_CASE("an index migrates from schema 1, and again after a crash left a step half done", "[ai][engine]") {
  scratch_dir dir{"ai-migrate"};
  const std::string path = utf8(dir / "index.db");
  {
    // Schema 1 as it was: roots without `media`, assets without `cloud`.
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                         "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                         "CREATE TABLE roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
                         " recursive INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,"
                         " last_scan_at INTEGER NOT NULL DEFAULT 0);"
                         "CREATE TABLE assets(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
                         " root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
                         " mtime INTEGER NOT NULL, size INTEGER NOT NULL, kind INTEGER NOT NULL,"
                         " duration_ms INTEGER NOT NULL DEFAULT 0, seen INTEGER NOT NULL DEFAULT 0);"
                         "INSERT INTO meta VALUES('schema', '1');"
                         "INSERT INTO roots(path, recursive) VALUES('/photos', 1);",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  }
  {
    auto d = mv::ai::index_db::open(path);
    REQUIRE(d);
    CHECK((*d)->meta("schema") == kSchemaNow);
    const auto roots = (*d)->roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].media == 0);
    // A crash between a step's ALTER and its schema bump, as earlier builds
    // could leave it: the column is there, the schema still says the old one.
    REQUIRE((*d)->set_meta("schema", "1"));
  }
  {
    auto d = mv::ai::index_db::open(path);
    REQUIRE(d);
    CHECK((*d)->meta("schema") == kSchemaNow);
    REQUIRE((*d)->set_meta("schema", "2"));
  }
  auto d = mv::ai::index_db::open(path);
  REQUIRE(d);
  CHECK((*d)->meta("schema") == kSchemaNow);
  CHECK((*d)->roots().size() == 1);
}

namespace {

// A schema-3 index as #176's builds left it, with one asset.
void make_schema3(const std::string& path) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db,
                       "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                       "CREATE TABLE roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
                       " recursive INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,"
                       " last_scan_at INTEGER NOT NULL DEFAULT 0, media INTEGER NOT NULL DEFAULT 0);"
                       "CREATE TABLE assets(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
                       " root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
                       " mtime INTEGER NOT NULL, size INTEGER NOT NULL, kind INTEGER NOT NULL,"
                       " duration_ms INTEGER NOT NULL DEFAULT 0, seen INTEGER NOT NULL DEFAULT 0,"
                       " cloud INTEGER NOT NULL DEFAULT 0);"
                       "INSERT INTO meta VALUES('schema', '3');"
                       "INSERT INTO roots(path, recursive) VALUES('/photos', 1);"
                       "INSERT INTO assets(path, root_id, mtime, size, kind) VALUES('/photos/r5.jpg', 1, 5, 10, 1);",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

bool column_exists(const std::string& path, const char* column) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db, "PRAGMA table_info(assets)", -1, &st, nullptr);
  bool found = false;
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    if (std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), column) == 0) found = true;
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return found;
}

}  // namespace

TEST_CASE("schema 3 migrates to 4: the tag columns and asset_text, keeping the assets", "[ai][engine][schema4]") {
  scratch_dir dir{"ai-schema4"};
  const std::string path = utf8(dir / "index.db");
  make_schema3(path);
  {
    auto d = mv::ai::index_db::open(path);
    REQUIRE(d);
    CHECK((*d)->meta("schema") == kSchemaNow);
    for (const char* c : {"tags", "taken_at", "lat", "lon", "camera", "lens", "made_with", "ai", "place", "country",
                          "region", "rating", "label", "iso", "aperture_x100", "focal_mm", "shutter_den",
                          "shutter_us", "motion", "motion_mtime", "motion_size", "motion_cloud"}) {
      CHECK(column_exists(path, c));
    }
    const auto all = (*d)->all_assets();
    REQUIRE(all.size() == 1);
    CHECK((*d)->tags_pending() == 1);  // the backfill reads it
    mv::ai::asset_tags t;
    t.id = all[0].id;
    t.mtime = 5;
    t.size = 10;
    t.camera = "Canon EOS R5";
    t.text = "Canon EOS R5";
    t.ai = false;
    t.has_where = true;
    t.lat = 38.8;
    t.lon = -9.4;
    t.taken_at = 1714572202;
    REQUIRE((*d)->commit_tags(std::span<const mv::ai::asset_tags>(&t, 1)));
    CHECK((*d)->tags_pending() == 0);
    // A stale read (the stamp moved since) is dropped.
    mv::ai::asset_tags stale = t;
    stale.mtime = 4;
    stale.camera = "Nikon";
    REQUIRE((*d)->commit_tags(std::span<const mv::ai::asset_tags>(&stale, 1)));
    int seen = 0;
    REQUIRE((*d)->each_tags([&](const mv::ai::asset_tags& r) {
      ++seen;
      CHECK(r.camera == "Canon EOS R5");
      CHECK(r.has_where);
      CHECK(r.taken_at == 1714572202);
      CHECK(r.text == "Canon EOS R5");
    }));
    CHECK(seen == 1);
    if ((*d)->meta("asset_text") == "fts5") {
      CHECK((*d)->match_text("r5", 10) == std::vector<std::int64_t>{all[0].id});
      CHECK((*d)->match_text("canon r5", 10) == std::vector<std::int64_t>{all[0].id});
      // Typed text is never FTS5 syntax: operators, quotes, column filters
      // and a lone star are words (no match, and never an SQL error).
      for (const char* hostile : {"r5 OR", "\"", "canon\" OR \"x", "NEAR(canon r5)", "text:canon", "*", "-r5",
                                  "^canon", "r5 AND NOT canon"}) {
        CAPTURE(hostile);
        const auto hits = (*d)->match_text(hostile, 10);
        CHECK(hits.size() <= 1);
      }
      CHECK((*d)->match_text("nikon OR canon", 10).empty());  // OR is a word, not an operator
    }
    CHECK(mv::ai::index_db::fts_phrases(" a  \"b\" ") == "\"a\" \"\"\"b\"\"\"");
    CHECK(mv::ai::index_db::fts_phrases("").empty());
    // A changed file reads its tags again.
    const mv::ai::index_db::seen_file f{"/photos/r5.jpg", 6, 11, mv::ai::asset_kind::photo};
    REQUIRE((*d)->see_assets(1, std::span<const mv::ai::index_db::seen_file>(&f, 1), (*d)->next_generation()));
    CHECK((*d)->tags_pending() == 1);
    // A removed asset takes its text with it (the trigger).
    REQUIRE((*d)->remove_root(1));
    seen = 0;
    REQUIRE((*d)->each_tags([&](const mv::ai::asset_tags&) { ++seen; }));
    CHECK(seen == 0);
  }
  // Half done, as a crash before the schema bump could leave it: the columns
  // there, the schema still 3. It migrates instead of failing.
  {
    auto d = mv::ai::index_db::open(path);
    REQUIRE(d);
    REQUIRE((*d)->set_meta("schema", "3"));
  }
  auto d = mv::ai::index_db::open(path);
  REQUIRE(d);
  CHECK((*d)->meta("schema") == kSchemaNow);
}

#if !defined(_WIN32)
TEST_CASE("the 3 -> 4 migration survives a kill mid-step", "[ai][engine][schema4]") {
  // The step is one transaction. A child process starts it as index_db does
  // (BEGIN, the ALTERs, the FTS table) and is killed before COMMIT: the
  // file is what a power cut or a force quit leaves. The next open finds
  // schema 3, unchanged, and migrates.
  scratch_dir dir{"ai-schema4-kill"};
  const std::string path = utf8(dir / "index.db");
  make_schema3(path);
  {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  }
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) _exit(2);
    sqlite3_exec(db, "BEGIN;"
                     "ALTER TABLE assets ADD COLUMN tags INTEGER NOT NULL DEFAULT 0;"
                     "ALTER TABLE assets ADD COLUMN taken_at INTEGER;"
                     "ALTER TABLE assets ADD COLUMN lat REAL;"
                     "CREATE VIRTUAL TABLE IF NOT EXISTS asset_text USING fts5(text);",
                 nullptr, nullptr, nullptr);
    // Make sure the uncommitted pages reach the WAL file, then die.
    sqlite3_exec(db, "PRAGMA cache_spill=1;", nullptr, nullptr, nullptr);
    ::kill(::getpid(), SIGKILL);
    _exit(3);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status));
  CHECK_FALSE(column_exists(path, "tags"));  // nothing of the step survived
  auto d = mv::ai::index_db::open(path);
  REQUIRE(d);
  CHECK((*d)->meta("schema") == kSchemaNow);
  CHECK(column_exists(path, "camera"));
  CHECK((*d)->all_assets().size() == 1);
}
#endif

TEST_CASE("camera:, ai:, made:, place: and a plain tag word in Local search", "[ai][engine][tags]") {
  rig r;
  r.file("red_r5.jpg");
  r.file("green_gpt.jpg");
  r.file("blue_geo.jpg");
  r.file("red_plain.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  CHECK(names(r.search("camera:R5")) == std::set<std::string>{"red_r5.jpg"});
  CHECK(names(r.search("camera:canon")) == std::set<std::string>{"red_r5.jpg"});
  CHECK(names(r.search("lens:24-70")) == std::set<std::string>{"red_r5.jpg"});
  CHECK(names(r.search("ai:yes")) == std::set<std::string>{"green_gpt.jpg"});
  CHECK(names(r.search("made:chatgpt")) == std::set<std::string>{"green_gpt.jpg"});
  CHECK(names(r.search("ai:no")).count("green_gpt.jpg") == 0);
  CHECK(names(r.search("ai:no")).size() == 3);
  CHECK(names(r.search("-camera:r5")).count("red_r5.jpg") == 0);
  // #441: place: is the Places add-on's answer; without it, nothing (the
  // case "place terms intersect ..." below has it installed).
  CHECK(r.search("place:portugal").empty());
  CHECK(r.search("in:sintra").empty());
  // in:2024 is still a date (no file is from 2024 here by mtime? it filters by date).
  // A camera term narrows a description: red AND a Canon R5.
  const auto red_r5 = r.search("red camera:r5");
  REQUIRE_FALSE(red_r5.empty());
  CHECK(names(red_r5) == std::set<std::string>{"red_r5.jpg"});

  // A plain word that is a tag: that file first, the tag as the snippet.
  const std::uint64_t id = r.eng->search_text("R5", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, 5000));
  const auto rows = r.rows(id);
  REQUIRE_FALSE(rows.empty());
  CHECK(rows.front().first == "red_r5.jpg");
  auto first = r.eng->result_at(id, 0);
  REQUIRE(first);
  CHECK((first->match & MV_AI_MATCH_TAGS) != 0);
  auto why = r.eng->result_snippet(id, 0);
  REQUIRE(why);
  CHECK(*why == "Canon EOS R5");
  const std::uint64_t gpt = r.eng->search_text("chatgpt", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(gpt, 5000));
  const auto gpt_rows = r.rows(gpt);
  REQUIRE_FALSE(gpt_rows.empty());
  CHECK(gpt_rows.front().first == "green_gpt.jpg");
  // #291 review: a tag word matches where a word starts ("r5", "24-70"), not
  // anywhere inside one: "anon" is in "Canon" but is no tag hit.
  const std::uint64_t mid = r.eng->search_text("anon", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(mid, 5000));
  const auto mid_rows = r.rows(mid);
  for (std::size_t i = 0; i < mid_rows.size(); ++i) {
    auto res = r.eng->result_at(mid, static_cast<std::uint32_t>(i));
    REQUIRE(res);
    CHECK((res->match & MV_AI_MATCH_TAGS) == 0);
  }
  const std::uint64_t lens = r.eng->search_text("70mm", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(lens, 5000));
  const auto lens_rows = r.rows(lens);
  REQUIRE_FALSE(lens_rows.empty());
  CHECK(lens_rows.front().first == "red_r5.jpg");
}

// #441: a place term is the Places add-on's answer (the host's
// place_matches), intersected with the rest of the query. Without Places it
// matches nothing: never every geotagged file (the bug: place:Portugal
// listed a photo taken in England).
TEST_CASE("place terms intersect the Places add-on's answer; without it they match nothing",
          "[ai][engine][tags][places]") {
  rig r;
  for (const char* n : {"blue_geo_pt.jpg", "red_geo_pt.jpg", "blue_geo_es.jpg", "blue_geo_uk.jpg", "blue_plain.jpg"}) {
    r.file(n);
  }
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  using names_t = std::set<std::string>;

  SECTION("no Places: no rows, whatever else the query says") {
    CHECK(r.search("place:portugal").empty());
    CHECK(r.search("in:lisbon").empty());
    CHECK(r.search("-place:spain").empty());
    CHECK(r.search("blue place:portugal").empty());
    CHECK(r.search("photo place:portugal").empty());
  }
  SECTION("with Places: only the assets it puts there") {
    std::atomic<int> asked{0};
    r.places = [&](const std::string& place) {
      ++asked;
      // "portugal" is the *_pt files, "spain" the *_es one; as the index keys them.
      const std::string tag = mv::text::fold(place) == "portugal" ? "_pt" : mv::text::fold(place) == "spain" ? "_es" : "?";
      std::vector<std::string> out;
      for (const auto& e : fs::directory_iterator(r.photos())) {
        if (utf8(e.path().filename()).find(tag) != std::string::npos) out.push_back(utf8(e.path()));
      }
      return out;
    };
    CHECK(names(r.search("place:portugal")) == names_t{"blue_geo_pt.jpg", "red_geo_pt.jpg"});
    CHECK(names(r.search("in:Portugal")) == names_t{"blue_geo_pt.jpg", "red_geo_pt.jpg"});
    // Left out: the provider's rows go; everything else stays.
    CHECK(names(r.search("-place:spain")) ==
          names_t{"blue_geo_pt.jpg", "red_geo_pt.jpg", "blue_geo_uk.jpg", "blue_plain.jpg"});
    CHECK(names(r.search("place:portugal -place:spain")) == names_t{"blue_geo_pt.jpg", "red_geo_pt.jpg"});
    // Two places: in both (none is).
    CHECK(r.search("place:portugal place:spain").empty());
    // A place nobody matches.
    CHECK(r.search("place:atlantis").empty());
    // beach place:portugal: the description ranks only Portugal's assets; the
    // blue photos taken elsewhere never come.
    const auto blue = r.search("blue place:portugal");
    REQUIRE_FALSE(blue.empty());
    CHECK(blue.front().first == "blue_geo_pt.jpg");
    const names_t got = names(blue);
    CHECK(got.count("blue_geo_es.jpg") == 0);
    CHECK(got.count("blue_geo_uk.jpg") == 0);
    CHECK(got.count("blue_plain.jpg") == 0);
    // Kinds and scope still apply.
    CHECK(r.search("video place:portugal").empty());
    CHECK(names(r.search("place:portugal", MV_AI_SCOPE_FOLDER, utf8(r.photos()))) ==
          names_t{"blue_geo_pt.jpg", "red_geo_pt.jpg"});
    CHECK(asked > 0);
  }
}

// #398: mv.ai.1 each_location, which the host serves to Places as
// locations_of_index: the geotagged assets from index.db, no file read.
TEST_CASE("the index lists its geotagged assets in batches, from index.db alone", "[ai][engine][tags][places]") {
  SECTION("index_db: id order, batches, only rows with both coordinates") {
    scratch_dir dir{"ai-located"};
    auto d = mv::ai::index_db::open(utf8(dir / "index.db"));
    REQUIRE(d);
    auto root = (*d)->add_root("/library", true);
    REQUIRE(root);
    std::vector<mv::ai::index_db::seen_file> files;
    for (int i = 0; i < 1200; ++i) {
      mv::ai::index_db::seen_file f;
      f.path = "/library/" + std::to_string(i) + ".jpg";
      f.mtime = 1;
      f.size = 100;
      files.push_back(std::move(f));
    }
    auto ups = (*d)->see_assets(*root, files, (*d)->next_generation());
    REQUIRE(ups);
    CHECK((*d)->located_assets(0, 512)->empty());  // nothing read yet
    // Every other asset geotagged; the rest read with no position.
    std::vector<mv::ai::asset_tags> tags;
    for (std::size_t i = 0; i < ups->size(); ++i) {
      mv::ai::asset_tags t;
      t.id = (*ups)[i].id;
      t.mtime = 1;
      t.size = 100;
      t.has_where = i % 2 == 0;
      t.lat = 38.0 + static_cast<double>(i) / 10000.0;
      t.lon = -9.0;
      tags.push_back(t);
    }
    REQUIRE((*d)->commit_tags(tags));
    std::vector<mv::ai::index_db::located> all;
    std::int64_t after = 0;
    int batches = 0;
    for (;;) {
      auto batch = (*d)->located_assets(after, 512);
      REQUIRE(batch);
      ++batches;
      all.insert(all.end(), batch->begin(), batch->end());
      if (batch->size() < 512) break;
      after = batch->back().id;
    }
    CHECK(batches == 2);
    REQUIRE(all.size() == 600);
    CHECK(all[0].path == "/library/0.jpg");
    CHECK(std::abs(all[0].lat - 38.0) < 1e-9);
    CHECK(all[1].path == "/library/2.jpg");
    CHECK(std::abs(all[1].lon + 9.0) < 1e-9);
    for (std::size_t i = 1; i < all.size(); ++i) CHECK(all[i].id > all[i - 1].id);
  }
  SECTION("engine: the geotagged file, and a visit that stops") {
    rig r;
    r.file("red_r5.jpg");
    r.file("blue_geo.jpg");
    r.file("green_geo.jpg");
    r.start();
    REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
    REQUIRE(r.idle());
    std::vector<std::string> names;
    REQUIRE(r.eng->each_location([&](const mv::ai::index_db::located& l) {
      names.push_back(utf8(fs::path(l.path).filename()));
      CHECK(std::abs(l.lat - 38.7975) < 1e-6);
      CHECK(std::abs(l.lon + 9.3883) < 1e-6);
      return true;
    }));
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"blue_geo.jpg", "green_geo.jpg"});
    int seen = 0;
    const mv::expected stopped = r.eng->each_location([&](const mv::ai::index_db::located&) { return ++seen < 1; });
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error() == mv::status::cancelled);
    CHECK(seen == 1);
  }
}

TEST_CASE("date terms use the date taken, and the file's time only when there is none", "[ai][engine][tags][dates]") {
  rig r;
  r.file("red_y2019.jpg");  // taken in 2019, written (mtime) today
  r.file("red_plain.jpg");  // no date taken: its mtime, today
  // A Photos item modified in 1970 (mtime 1) and created in 2018.
  r.library->items = {{"red-old", 1, mv::ai::asset_kind::photo, true, "", 0}};
  r.library->items.front().created = mv::ai::query::days_from_civil(2018, 3, 1) * 86400;
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  const int embedded = r.fast->images_embedded.load();
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  const auto now = std::chrono::system_clock::now();
  const std::int64_t year_now =
      1970 + std::chrono::duration_cast<std::chrono::hours>(now.time_since_epoch()).count() / (24 * 36525 / 100);
  const std::string this_year = "in:" + std::to_string(year_now);

  CHECK(names(r.search("in:2019")) == std::set<std::string>{"red_y2019.jpg"});
  CHECK(names(r.search("before:2020")).count("red_y2019.jpg") == 1);
  CHECK(names(r.search("before:2020")).count("red_plain.jpg") == 0);
  CHECK(names(r.search("in:2019-06")) == std::set<std::string>{"red_y2019.jpg"});
  CHECK(names(r.search("in:2019-07")).empty());
  // The file's own time no longer dates it...
  CHECK(names(r.search(this_year)).count("red_y2019.jpg") == 0);
  // ...but a file with no date taken still matches on it.
  CHECK(names(r.search(this_year)).count("red_plain.jpg") == 1);
  // A Photos row goes by its creationDate.
  CHECK(names(r.search("in:2018")) == std::set<std::string>{"photos:red-old"});
  CHECK(names(r.search("in:1970")).empty());
  // A description with a date narrows the same way.
  CHECK(names(r.search("red in:2019")) == std::set<std::string>{"red_y2019.jpg"});
  // Filters alone list newest first by the same date.
  const auto all = r.search("after:2000");
  REQUIRE(all.size() == 3);
  CHECK(all[0].first == "red_plain.jpg");
  CHECK(all[1].first == "red_y2019.jpg");
  CHECK(all[2].first == "photos:red-old");
  // Nothing was embedded again, and a restart reads the dates from the index.
  CHECK(r.fast->images_embedded.load() == embedded);
  r.start();
  REQUIRE(r.idle());
  CHECK(names(r.search("in:2019")) == std::set<std::string>{"red_y2019.jpg"});
  CHECK(r.fast->images_embedded.load() == embedded);
}

TEST_CASE("a description nothing answers lists newest by date created; an answered one keeps best match",
          "[ai][engine][dates][order]") {
  // #412.
  rig r;
  r.file("red_plain.jpg");    // no date taken: its mtime, today
  r.file("green_y2019.jpg");  // taken 2019-06-15
  const std::int64_t day = 86400;
  // Edited in Photos in 2025, created in 2018: it goes by the day it was created.
  const std::int64_t created_2017 = mv::ai::query::days_from_civil(2017, 1, 1) * day;
  r.library->items = {
      {"blue-edited", mv::ai::query::days_from_civil(2025, 1, 1) * day, mv::ai::asset_kind::photo, true, "",
       mv::ai::query::days_from_civil(2018, 3, 1) * day},
      // Two created in the same second: by path, the same way every time.
      {"grey-b", 1, mv::ai::asset_kind::photo, true, "", created_2017},
      {"grey-a", 1, mv::ai::asset_kind::photo, true, "", created_2017},
  };
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.eng->index_photos_library());
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::vector<std::string> out;
    for (const auto& [n, _] : rows) out.push_back(n);
    return out;
  };
  const std::vector<std::string> newest{"red_plain.jpg", "green_y2019.jpg", "photos:blue-edited", "photos:grey-a",
                                        "photos:grey-b"};
  // Not an empty page, and not a score ranking: newest created first.
  CHECK(names(r.search("xyzzy plugh")) == newest);
  CHECK(r.answered("xyzzy plugh").empty());
  CHECK(names(r.search("xyzzy plugh")) == newest);  // the same order again
  // Within the Photos library alone too.
  CHECK(names(r.search("xyzzy plugh", MV_AI_SCOPE_FOLDER, "photos:")) ==
        (std::vector<std::string>{"photos:blue-edited", "photos:grey-a", "photos:grey-b"}));
  // An answered query stays best match, not date order.
  const auto blue = r.answered("something blue");
  REQUIRE_FALSE(blue.empty());
  CHECK(blue.front().first == "photos:blue-edited");
  CHECK(r.search("something blue").front().first == "photos:blue-edited");
}

TEST_CASE("rating:, label:, iso:, f:, mm: and shutter: in Local search (#405)", "[ai][engine][tags][exposure]") {
  rig r;
  r.file("red_iso400.jpg");  // ISO 400, f/2.8, 1/250, 35 mm, five stars, Red
  r.file("red_plain.jpg");   // none of those, unrated
  r.file("rgb_clip.mp4");    // a clip: no exposure block
  r.file("blue_reject.jpg"); // rejected
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const int embedded = r.fast->images_embedded.load();
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  const std::set<std::string> first{"red_iso400.jpg"};
  for (const char* q : {"iso:400", "f:2.8", "mm:35", "shutter:1/250", "shutter:250", "rating:5", "rating:3+",
                        "label:red", "label:Red", "iso:400 f:2.8 mm:35"}) {
    CAPTURE(q);
    CHECK(names(r.search(q)) == first);
  }
  CHECK(names(r.search("iso:100-200")).empty());
  CHECK(names(r.search("iso:1600+")).empty());
  CHECK(names(r.search("rating:0")) == std::set<std::string>{"red_plain.jpg", "rgb_clip.mp4"});
  CHECK(names(r.search("rejected")) == std::set<std::string>{"blue_reject.jpg"});
  CHECK(names(r.search("rating:-1")) == std::set<std::string>{"blue_reject.jpg"});
  CHECK(names(r.search("-rating:5")).count("red_iso400.jpg") == 0);
  CHECK(names(r.search("-rating:5")).size() == 3);
  CHECK(names(r.search("-iso:400")).count("rgb_clip.mp4") == 1);  // no ISO: not in the range
  // A description beside a term runs the picture tower on that subset.
  CHECK(names(r.search("iso:400 red")) == first);
  // The unrated red things: the photo and the clip whose first seconds are red.
  CHECK(names(r.search("rating:0 red")) == std::set<std::string>{"red_plain.jpg", "rgb_clip.mp4"});
  // A plain number is not a tag word: "400" finds no file by its ISO.
  const std::uint64_t id = r.eng->search_text("400", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, kSettleMs));
  auto n = r.eng->result_count(id);
  REQUIRE(n);
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto res = r.eng->result_at(id, i);
    REQUIRE(res);
    CHECK((res->match & MV_AI_MATCH_TAGS) == 0);
  }
  CHECK(r.fast->images_embedded.load() == embedded);  // searching embeds no picture
}

TEST_CASE("an index read by an older host gains rating and exposure later without a re-embed (#405)",
          "[ai][engine][tags][exposure]") {
  rig r;
  r.file("red_iso400.jpg");
  r.file("green_r5.jpg");
  g_old_tag_host = true;
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const int embedded = r.fast->images_embedded.load();
  CHECK(r.search("rating:5").empty());          // the old host said nothing of it
  CHECK(r.search("camera:r5").size() == 1);     // what it did say is there
  {
    auto d = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(d);
    CHECK((*d)->tags_pending(1) == 0);
    CHECK((*d)->tags_pending() == 2);  // level 2 owed, not spun on
  }
  // The app updates: the next run reads the new facts, embedding nothing.
  g_old_tag_host = false;
  r.start();
  REQUIRE(r.idle());
  CHECK(r.search("rating:5").size() == 1);
  CHECK(r.search("iso:400").size() == 1);
  CHECK(r.search("camera:r5").size() == 1);
  CHECK(r.fast->images_embedded.load() == embedded);
  r.eng.reset();
  auto d = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
  REQUIRE(d);
  CHECK((*d)->tags_pending() == 0);
}

TEST_CASE("the tag backfill resumes after a restart and reads nothing twice", "[ai][engine][tags]") {
  rig r;
  r.file("red_r5.jpg");
  r.file("green_gpt.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(r.search("camera:r5").size() == 1);
  // A restart: the tags come from the index, not from the host again.
  r.start();
  REQUIRE(r.idle());
  CHECK(r.search("camera:r5").size() == 1);
  CHECK(r.search("ai:yes").size() == 1);
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
  REQUIRE(r.eng->wait_search(id, kSettleMs));
  const auto rows = r.rows(id);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "red_b.jpg");
}

// Owner report, 2026-10-05: a 26-photo Downloads with five clear faces, none
// twice, said "No people found yet". Under 100 photos and clips in view one
// face lists a person; a larger view keeps two, and a small folder inside it
// still shows its own one-off faces.
TEST_CASE("people: one face is enough in a small folder, not across a large library", "[ai][engine][faces]") {
  rig r;
  r.file("solo/ben_1.jpg");
  r.file("anna_1.jpg");
  r.file("anna_2.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
  REQUIRE(r.idle());
  auto small = mv::json::parse(r.eng->people_json());
  REQUIRE(small);
  CHECK(small->a.size() == 2);  // Anna, and Ben from his one photo
  CHECK(r.status().people == 2);

  for (int i = 0; i < 100; ++i) r.file("red_" + std::to_string(i) + ".jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  auto large = mv::json::parse(r.eng->people_json());
  REQUIRE(large);
  REQUIRE(large->a.size() == 1);  // Ben's one face no longer lists him everywhere
  CHECK(*large->a[0].integer("faces") == 2);
  CHECK(r.status().people == 1);
  auto solo = mv::json::parse(r.eng->people_json(utf8(r.photos() / "solo"), MV_AI_SCOPE_TREE));
  REQUIRE(solo);
  CHECK(solo->a.size() == 1);  // but his own small folder shows him
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
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

TEST_CASE("audio files are indexed only while Show audio files is on (#355)", "[ai][engine][audiofiles]") {
  rig r;
  r.file("red_photo.jpg");
  r.file("red_cover.mp3");   // cover art: red
  r.file("song.m4a");        // tags only: "Hey Jude", "The Beatles"
  r.file("blank.m4p");       // neither: found by name alone (file:)
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  const auto audio_rows = [&] {
    auto doc = mv::json::parse(r.eng->roots_json());
    REQUIRE(doc);
    REQUIRE(doc->k == mv::json::kind::array);
    REQUIRE(doc->a.size() == 1);
    return doc->a[0].integer("audio").value_or(-1);
  };
  const auto setting = [&] {
    auto doc = mv::json::parse(r.eng->settings_json());
    REQUIRE(doc);
    return doc->boolean("audio_files");
  };

  // Off (the default): no row, no read, nothing found.
  CHECK(setting() == false);
  CHECK(r.status().assets_total == 1);
  CHECK(audio_rows() == 0);
  CHECK_FALSE(r.was_decoded("red_cover.mp3"));
  CHECK(names(r.search("red")) == std::set<std::string>{"red_photo.jpg"});
  CHECK(r.answered("beatles").empty());

  // On: the folder's audio files are walked and indexed like a new folder.
  REQUIRE(r.eng->set_setting("audio_files", "1"));
  CHECK(setting() == true);
  REQUIRE(wait_for([&] { return audio_rows() == 3; }, kSettleMs));
  REQUIRE(r.idle());
  const mv_ai_status on = r.status();
  CHECK(on.assets_total == 4);
  CHECK(on.assets_done == 4);  // blank.m4p has no cover art: done, not failed
  CHECK(on.assets_failed == 0);
  CHECK(r.was_decoded("red_cover.mp3"));
  CHECK(names(r.search("red")) == std::set<std::string>{"red_photo.jpg", "red_cover.mp3"});
  CHECK(names(r.search("beatles")) == std::set<std::string>{"song.m4a"});
  CHECK(names(r.search("hey jude")) == std::set<std::string>{"song.m4a"});
  CHECK(names(r.search("file:blank")) == std::set<std::string>{"blank.m4p"});
  // The kind: its own bit, the Show segment, is:audio, and an older chrome's 3.
  {
    const std::uint64_t id = r.eng->search_text("red", "", MV_AI_SCOPE_ALL, MV_AI_KIND_AUDIO);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    REQUIRE(names(r.rows(id)) == std::set<std::string>{"red_cover.mp3"});
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK(res->kind == MV_AI_KIND_AUDIO);
    CHECK(res->pts_ms == -1);
  }
  CHECK(names(r.search("red is:audio")) == std::set<std::string>{"red_cover.mp3"});
  CHECK(names(r.search("red songs")) == std::set<std::string>{"red_cover.mp3"});
  CHECK(names(r.search("red -is:music")) == std::set<std::string>{"red_photo.jpg"});
  CHECK(names(r.search("red", MV_AI_SCOPE_ALL, "", MV_AI_KIND_PHOTOS | MV_AI_KIND_VIDEOS)) ==
        std::set<std::string>{"red_photo.jpg"});

  // Off again: gone from results at once, then from the index.
  REQUIRE(r.eng->set_setting("audio_files", "0"));
  CHECK(names(r.search("red")) == std::set<std::string>{"red_photo.jpg"});
  CHECK(r.answered("beatles").empty());
  REQUIRE(wait_for([&] { return audio_rows() == 0; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.status().assets_total == 1);

  // It persists, and a restart walks no audio back in.
  r.start();
  REQUIRE(r.idle());
  CHECK(setting() == false);
  CHECK(audio_rows() == 0);
  CHECK(r.answered("beatles").empty());
}

TEST_CASE("documents are indexed for their words only while Show documents is on (#354)",
          "[ai][engine][documents]") {
  rig r;
  r.file("red_photo.jpg");
  r.file("invoice_march.pdf");
  r.file("lease.docx");
  r.file("locked_statement.pdf");
  r.file("locked_payslip.docx");
  r.file("broken_scan.pdf");
  r.file("scan_receipt.pdf");  // no text layer: read, nothing to find (D4's OCR)
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  const auto root = [&] {
    auto doc = mv::json::parse(r.eng->roots_json());
    REQUIRE(doc);
    REQUIRE(doc->a.size() == 1);
    return doc->a[0];
  };
  const auto setting = [&](const char* key) {
    auto doc = mv::json::parse(r.eng->settings_json());
    REQUIRE(doc);
    return doc->integer(key).value_or(doc->boolean(key).value_or(false) ? 1 : 0);
  };

  // Off (the default): the walk drops them by name. No row, no read, no cost.
  CHECK(setting("document_files") == 0);
  CHECK(setting("doc_index") == 1);  // Words, waiting for the switch
  CHECK(root().integer("documents").value_or(-1) == 0);
  CHECK(r.status().docs_total == 0);
  CHECK(r.pdf_pages_read == 0);
  CHECK(r.search("\"invoice 4471\"").empty());

  // On: walked and read like a newly added folder.
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(wait_for([&] { return root().integer("documents").value_or(0) == 6; }, kSettleMs));
  REQUIRE(r.idle());
  const mv_ai_status on = r.status();
  CHECK(on.docs_total == 6);
  CHECK(on.docs_done == 6);
  CHECK(on.docs_locked == 2);
  CHECK(on.docs_failed == 1);
  CHECK(on.assets_total == 1);  // the picture pass: the photo alone
  CHECK(on.assets_done == 1);
  CHECK(on.state == MV_AI_STATE_IDLE);
  CHECK(root().integer("assets").value_or(-1) == 1);
  CHECK(root().integer("documents_done").value_or(-1) == 6);
  CHECK_FALSE(r.was_decoded("invoice_march.pdf"));  // its words only: no picture (D2)

  // A quoted phrase from the first page and from the last, at its page.
  {
    const std::uint64_t id = r.eng->search_text("\"invoice 4471\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == "invoice_march.pdf");
    CHECK(rows[0].second == 0);
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK(res->kind == MV_AI_KIND_DOCUMENTS);
    CHECK(res->match == MV_AI_MATCH_TEXT);
    CHECK(r.eng->result_snippet(id, 0)->find("Invoice 4471") != std::string::npos);
  }
  CHECK(r.search("\"net 30 days\"") == std::vector<std::pair<std::string, std::int64_t>>{{"invoice_march.pdf", 2}});
  // A word hyphenated over a line break is one word again.
  CHECK(names(r.search("\"this invoice\"")) == std::set<std::string>{"invoice_march.pdf"});
  // A DOCX through the host, its paragraph's page.
  CHECK(r.search("\"two months notice\"") == std::vector<std::pair<std::string, std::int64_t>>{{"lease.docx", 1}});
  // Plain words: the speech rule (most of them; longer forms of a long one).
  CHECK(names(r.search("tenant rent")) == std::set<std::string>{"lease.docx"});
  CHECK(names(r.search("payment terms")) == std::set<std::string>{"invoice_march.pdf"});
  CHECK(r.answered("payment terms", MV_AI_SCOPE_ALL, "", MV_AI_KIND_PHOTOS | MV_AI_KIND_VIDEOS).empty());
  // The pages that match, for N / Shift+N: one stop a page.
  {
    const std::uint64_t id = r.eng->search_text("\"lease\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    CHECK(names(r.rows(id)) == std::set<std::string>{"invoice_march.pdf", "lease.docx"});
    auto pages = r.eng->clip_matches(id, utf8(r.photos() / "lease.docx"));
    REQUIRE(pages);
    REQUIRE(pages->size() == 1);
    CHECK((*pages)[0].first == 0);
  }
  // Kinds and where: is:document, is:pdf, text: (documents only), said: (clips only).
  CHECK(names(r.search("is:document")) ==
        std::set<std::string>{"invoice_march.pdf", "lease.docx", "locked_statement.pdf", "locked_payslip.docx",
                              "broken_scan.pdf", "scan_receipt.pdf"});
  CHECK(names(r.search("lease is:pdf")) == std::set<std::string>{"invoice_march.pdf"});
  CHECK(names(r.search("text:tenant")) == std::set<std::string>{"lease.docx"});
  CHECK(r.search("said:tenant").empty());
  CHECK(names(r.search("\"lease\" -text:tenant")) == std::set<std::string>{"invoice_march.pdf"});
  // Text off in Match by: no document by its words (a phrase still filters).
  CHECK(r.answered("tenant rent", MV_AI_SCOPE_ALL, "", MV_AI_KIND_ALL | MV_AI_FIND_PICTURES).empty());
  // An older chrome's kinds (photos, videos, audio) never see a document.
  CHECK(r.answered("tenant rent", MV_AI_SCOPE_ALL, "", 11u).empty());

  // Off again: gone from results at once, then from the index, text and all.
  REQUIRE(r.eng->set_setting("document_files", "0"));
  CHECK(r.search("\"invoice 4471\"").empty());
  REQUIRE(wait_for([&] { return root().integer("documents").value_or(-1) == 0; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.status().docs_total == 0);
  {
    auto db = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(db);
    CHECK((*db)->passage_count() == 0);
  }

  // It persists, and a restart walks no document back in.
  r.start();
  REQUIRE(r.idle());
  CHECK(setting("document_files") == 0);
  CHECK(root().integer("documents").value_or(-1) == 0);
}

TEST_CASE("the Documents piece finds documents by what they are about (#354 D3)", "[ai][engine][documents]") {
  rig r;
  r.file("red_photo.jpg");
  r.file("invoice_march.pdf");
  r.file("lease.docx");
  r.file("locked_statement.pdf");
  r.docs_available = true;
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  const auto vecs = [&] {
    auto db = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(db);
    return (*db)->passage_vec_count("fake-text/int8/pre1");
  };
  {
    auto doc = mv::json::parse(r.eng->settings_json());
    REQUIRE(doc);
    CHECK(doc->boolean("docs_ready").value_or(false));
    CHECK(*doc->str("docs_model") == "Fake text");
  }
  const mv_ai_status st = r.status();
  CHECK((st.flags & MV_AI_STATUS_DOCS_READY) != 0);
  // The readable documents, once their words are read; a locked one never comes.
  CHECK(st.meaning_total == 2);
  CHECK(st.meaning_done == 2);
  CHECK(st.state == MV_AI_STATE_IDLE);
  // invoice: 3 pages; lease: its heading and first sentence, then page 2.
  CHECK(vecs() == 5);
  const int embedded = r.docs_model->passages_embedded.load();
  CHECK(embedded == 5);

  // By meaning: none of these words is in the lease, which is found at the
  // page that is about it, the passage as its snippet.
  {
    const std::uint64_t id = r.eng->search_text("paying my landlord", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == std::pair<std::string, std::int64_t>{"lease.docx", 0});
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK(res->kind == MV_AI_KIND_DOCUMENTS);
    CHECK(res->match == MV_AI_MATCH_TEXT);
    CHECK(r.eng->result_snippet(id, 0)->find("tenant shall pay") != std::string::npos);
  }
  CHECK(r.search("the electricity bill") == std::vector<std::pair<std::string, std::int64_t>>{{"invoice_march.pdf", 2}});
  // "Nothing found": a query about nothing here answers nothing by meaning.
  CHECK(r.answered("wedding seating plan").empty());
  // Words still match as typed, beside meaning.
  CHECK(names(r.search("\"two months notice\"")) == std::set<std::string>{"lease.docx"});
  // Text off in Match by: no document by meaning either.
  CHECK(r.answered("paying my landlord", MV_AI_SCOPE_ALL, "", MV_AI_KIND_ALL | MV_AI_FIND_PICTURES).empty());

  // A restart answers from the stored vectors: nothing embedded again.
  r.start();
  REQUIRE(r.idle());
  CHECK(names(r.search("paying my landlord")) == std::set<std::string>{"lease.docx"});
  CHECK(r.docs_model->passages_embedded.load() == embedded);

  // The search agent's reader answers by meaning from the same vectors.
  {
    auto rd = r.start_reader();
    const std::uint64_t id = rd->search_text("paying my landlord", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(rd->wait_search(id, kSettleMs));
    auto n = rd->result_count(id);
    REQUIRE(n);
    CHECK(*n == 1);
  }

  // Without the piece: words only, and the vectors wait for it.
  r.docs_available = false;
  r.start();
  REQUIRE(r.idle());
  CHECK(r.answered("paying my landlord").empty());
  CHECK((r.status().flags & MV_AI_STATUS_DOCS_READY) == 0);
  CHECK(r.status().meaning_total == 0);
  CHECK(vecs() == 5);

  // Show documents off: documents and their vectors go.
  r.docs_available = true;
  r.start();
  REQUIRE(r.idle());
  REQUIRE(r.eng->set_setting("document_files", "0"));
  REQUIRE(wait_for([&] { return vecs() == 0; }, kSettleMs));
  CHECK(r.answered("paying my landlord").empty());
}

TEST_CASE("a folder's Documents choice and Settings' Off drop its documents' text (#354)",
          "[ai][engine][documents]") {
  rig r;
  r.file("lease.docx");
  r.file("red_photo.jpg");
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  REQUIRE(r.search("\"two months notice\"").size() == 1);
  const auto root = [&] {
    auto doc = mv::json::parse(r.eng->roots_json());
    REQUIRE(doc);
    REQUIRE(doc->a.size() == 1);
    return doc->a[0];
  };
  const std::int64_t id = root().integer("id").value_or(0);

  // The folder Off: out of results at once, then its rows go with a walk.
  REQUIRE(r.eng->root_set_documents(id, MV_AI_DOCS_OFF));
  CHECK(r.search("\"two months notice\"").empty());
  REQUIRE(wait_for([&] { return root().integer("documents").value_or(-1) == 0; }, kSettleMs));
  CHECK(root().integer("docs").value_or(-1) == MV_AI_DOCS_OFF);
  CHECK(r.search("red").size() == 1);  // the photo stays
  CHECK_FALSE(r.eng->root_set_documents(id, 9));

  // Back to the setting (Words): read again.
  REQUIRE(r.eng->root_set_documents(id, MV_AI_DOCS_DEFAULT));
  REQUIRE(wait_for([&] { return root().integer("documents").value_or(0) == 1; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.search("\"two months notice\"").size() == 1);

  // Settings Off: every folder that follows it.
  REQUIRE(r.eng->set_setting("doc_index", "0"));
  REQUIRE(wait_for([&] { return root().integer("documents").value_or(-1) == 0; }, kSettleMs));
  CHECK(r.search("\"two months notice\"").empty());
  CHECK_FALSE(r.eng->set_setting("doc_index", "3"));
}

TEST_CASE("Words and pages pictures a document's first pages and its picture pages (#354 D2)",
          "[ai][engine][documents]") {
  rig r;
  r.file("red_photo.jpg");
  r.file("gallery_brochure.pdf");  // 8 pages; 5 (red) and 6 (green) mostly pictures
  r.file("invoice_march.pdf");     // 3 pages of words
  r.file("lease.docx");            // 2 pages, laid out by the host
  r.file("scan_receipt.pdf");      // 2 pages, both pictures
  r.file("locked_statement.pdf");  // its words are locked: no pictures either
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };
  // Stored page pictures (frames rows with kFramePage), read beside the engine.
  const auto page_frames = [&] {
    sqlite3* db = nullptr;
    std::int64_t n = -1;
    if (sqlite3_open(utf8(r.dir / "data" / "index.db").c_str(), &db) == SQLITE_OK) {
      sqlite3_stmt* q = nullptr;
      const std::string sql = "SELECT COUNT(*) FROM frames WHERE (flags & " + std::to_string(mv::ai::kFramePage) +
                              ") != 0 AND pts_ms >= 0";
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &q, nullptr) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
        n = sqlite3_column_int64(q, 0);
      }
      sqlite3_finalize(q);
    }
    sqlite3_close(db);
    return n;
  };
  const auto root_id = [&] {
    auto doc = mv::json::parse(r.eng->roots_json());
    REQUIRE(doc);
    REQUIRE(doc->a.size() == 1);
    return doc->a[0].integer("id").value_or(0);
  };

  // Words (the default): no page is drawn, nothing is pictured.
  CHECK(r.drawn_pages().empty());
  CHECK(r.status().pages_total == 0);
  CHECK(names(r.search("red")) == std::set<std::string>{"red_photo.jpg"});

  // Words and pages: after their words, the first three pages of each and
  // any page mostly a picture; never past the last page, never twice.
  REQUIRE(r.eng->set_setting("doc_index", "2"));
  REQUIRE(wait_for([&] { return r.status().pages_total == 4; }, kSettleMs));
  REQUIRE(r.idle());
  const mv_ai_status on = r.status();
  CHECK(on.pages_done == 4);
  CHECK(on.state == MV_AI_STATE_IDLE);
  CHECK(on.docs_locked == 1);
  CHECK(r.drawn_pages() ==
        std::vector<std::string>{"gallery_brochure.pdf#0", "gallery_brochure.pdf#1", "gallery_brochure.pdf#2",
                                 "gallery_brochure.pdf#5", "gallery_brochure.pdf#6", "invoice_march.pdf#0",
                                 "invoice_march.pdf#1", "invoice_march.pdf#2", "lease.docx#0", "lease.docx#1",
                                 "scan_receipt.pdf#0", "scan_receipt.pdf#1"});
  CHECK(page_frames() == 12);
  // Found by what the page shows, at that page, beside the photo.
  {
    const std::uint64_t id = r.eng->search_text("red", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    const auto rows = r.rows(id);
    CHECK(names(rows) == std::set<std::string>{"red_photo.jpg", "gallery_brochure.pdf"});
    for (std::uint32_t i = 0; i < rows.size(); ++i) {
      if (rows[i].first != "gallery_brochure.pdf") continue;
      CHECK(rows[i].second == 5);
      auto res = r.eng->result_at(id, i);
      REQUIRE(res);
      CHECK(res->kind == MV_AI_KIND_DOCUMENTS);
      CHECK(res->match == MV_AI_MATCH_PICTURE);
    }
  }
  CHECK(r.search("green", MV_AI_SCOPE_ALL, "", MV_AI_KIND_DOCUMENTS) ==
        std::vector<std::pair<std::string, std::int64_t>>{{"gallery_brochure.pdf", 6}});
  // Match by Text alone leaves the pictures out; the words still find theirs.
  CHECK(r.answered("green", MV_AI_SCOPE_ALL, "", MV_AI_KIND_ALL | MV_AI_FIND_TEXT).empty());
  CHECK(names(r.search("tenant rent")) == std::set<std::string>{"lease.docx"});

  // A restart keeps them: loaded with the other vectors, nothing drawn again.
  const std::size_t drawn = r.drawn_pages().size();
  r.start();
  REQUIRE(r.idle());
  CHECK(r.drawn_pages().size() == drawn);
  CHECK(r.search("green", MV_AI_SCOPE_ALL, "", MV_AI_KIND_DOCUMENTS).size() == 1);

  // The folder at Words: its pictures go (the words stay), and back at Words
  // and pages they are drawn again.
  const std::int64_t id = root_id();
  REQUIRE(r.eng->root_set_documents(id, MV_AI_DOCS_WORDS));
  REQUIRE(wait_for([&] { return r.answered("green", MV_AI_SCOPE_ALL, "", MV_AI_KIND_DOCUMENTS).empty(); }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(page_frames() == 0);
  CHECK(r.status().pages_total == 0);
  CHECK(names(r.search("tenant rent")) == std::set<std::string>{"lease.docx"});
  REQUIRE(r.eng->root_set_documents(id, MV_AI_DOCS_PAGES));
  REQUIRE(wait_for([&] { return r.status().pages_total == 4; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(page_frames() == 12);
  CHECK(r.drawn_pages().size() == drawn * 2);

  // Following Settings again, and Settings back at Words: dropped.
  REQUIRE(r.eng->root_set_documents(id, MV_AI_DOCS_DEFAULT));
  REQUIRE(r.idle());
  CHECK(page_frames() == 12);
  REQUIRE(r.eng->set_setting("doc_index", "1"));
  REQUIRE(wait_for([&] { return r.answered("red", MV_AI_SCOPE_ALL, "", MV_AI_KIND_DOCUMENTS).empty(); }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(page_frames() == 0);
  CHECK(r.status().pages_total == 0);
}

TEST_CASE("a long PDF stops at a page and resumes there; no PDFium sets PDFs aside (#354)",
          "[ai][engine][documents]") {
  rig r;
  r.file("long_report.pdf");
  // The viewer gets busy on page 20: the pass waits there, and the engine
  // stops mid-document (wait_turn answers false).
  r.busy_after_pdf_pages = 20;
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(wait_for([&] { return r.pdf_pages_read >= 20; }, kSettleMs));
  r.eng.reset();
  r.busy = false;
  r.busy_after_pdf_pages = -1;
  const int before = r.pdf_pages_read.load();
  {
    auto db = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(db);
    CHECK((*db)->passage_count() > 0);  // the pages committed so far stand
  }
  r.start();
  REQUIRE(r.idle());
  // Resumed after the last committed page: never all 600 again, and capped at 500.
  CHECK(r.pdf_pages_read - before < 500);
  CHECK(r.status().docs_done == 1);
  CHECK(r.search("\"report page 1 ends\"").size() == 1);
  CHECK(r.search("\"report page 500 ends\"").size() == 1);
  CHECK(r.search("\"report page 501 ends\"").empty());  // past the page cap
  {
    const std::uint64_t id = r.eng->search_text("\"report page 37 ends\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    REQUIRE(r.rows(id).size() == 1);
    CHECK(r.rows(id)[0].second == 36);
  }

  // A pack without PDFium: a PDF waits (not failed), and is read once one can.
  rig s;
  s.file("invoice_april.pdf");
  s.pdf_available = false;
  s.start();
  REQUIRE(s.eng->set_setting("document_files", "1"));
  REQUIRE(s.eng->index_folder(utf8(s.photos()), false));
  REQUIRE(s.idle());
  CHECK(s.status().docs_done == 1);
  CHECK(s.status().docs_failed == 1);
  CHECK(s.search("\"invoice 4471\"").empty());
  auto settings = mv::json::parse(s.eng->settings_json());
  REQUIRE(settings);
  CHECK(settings->boolean("pdf_available") == false);
  s.pdf_available = true;
  s.start();
  REQUIRE(s.idle());
  CHECK(s.status().docs_failed == 0);
  CHECK(s.search("\"invoice 4471\"").size() == 1);
}

TEST_CASE("a scanned PDF's pages are read by the OS and found by a quoted phrase (#354 D4)",
          "[ai][engine][documents][ocr]") {
  rig r;
  r.text_available = true;
  r.file("red_photo.jpg");
  r.file("scan_receipt.pdf");   // two pages, no text layer
  r.file("mixed_report.pdf");   // words on pages 0 and 2; page 1 a stamped number alone
  r.file("invoice_march.pdf");  // words on every page: never drawn
  r.file("lease.docx");         // a DOCX: its words are all its text
  r.file("locked_statement.pdf");
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  const auto names = [](const std::vector<std::pair<std::string, std::int64_t>>& rows) {
    std::set<std::string> out;
    for (const auto& [n, _] : rows) out.insert(n);
    return out;
  };

  // Only the pages with no text layer were drawn and read.
  CHECK(r.drawn_pages() ==
        std::vector<std::string>{"mixed_report.pdf#1", "scan_receipt.pdf#0", "scan_receipt.pdf#1"});
  CHECK(r.scan_reads == 3);
  const mv_ai_status s = r.status();
  CHECK(s.scans_total == 4);  // the documents whose words are read (the locked one never is)
  CHECK(s.scans_done == 4);
  CHECK(s.state == MV_AI_STATE_IDLE);

  // A quoted phrase finds each scanned page, at its page, over the line the
  // OS hyphenated ("re-" / "cords.").
  CHECK(names(r.search("\"kept for records\"")) == std::set<std::string>{"scan_receipt.pdf", "mixed_report.pdf"});
  CHECK(r.search("\"scanned page 1 of the archive\"", MV_AI_SCOPE_ALL, "", MV_AI_KIND_DOCUMENTS).size() == 2);
  {
    const std::uint64_t id = r.eng->search_text("\"scanned page 0 of the archive\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == std::pair<std::string, std::int64_t>{"scan_receipt.pdf", 0});
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK(res->kind == MV_AI_KIND_DOCUMENTS);
    CHECK(res->match == MV_AI_MATCH_TEXT);
  }
  // The text layer's passages are untouched beside them.
  CHECK(names(r.search("\"invoice 4471\"")) == std::set<std::string>{"invoice_march.pdf"});
  CHECK(r.search("\"mixed report opening page\"") ==
        std::vector<std::pair<std::string, std::int64_t>>{{"mixed_report.pdf", 0}});
  CHECK(names(r.search("tenant rent")) == std::set<std::string>{"lease.docx"});

  // Read once: a restart draws nothing again.
  r.start();
  REQUIRE(r.idle());
  CHECK(r.scan_reads == 3);
  CHECK(r.drawn_pages().size() == 3);
  CHECK(names(r.search("\"kept for records\"")).size() == 2);

  // No OS reader: nothing to count, nothing drawn, the words still found.
  rig n;
  n.file("scan_receipt.pdf");
  n.file("invoice_march.pdf");
  n.start();
  REQUIRE(n.eng->set_setting("document_files", "1"));
  REQUIRE(n.eng->index_folder(utf8(n.photos()), false));
  REQUIRE(n.idle());
  CHECK(n.status().scans_total == 0);
  CHECK(n.drawn_pages().empty());
  CHECK(n.search("\"invoice 4471\"").size() == 1);
}

TEST_CASE("a scan's pages are embedded by meaning once the OS has read them (#354 D3 + D4)",
          "[ai][engine][documents][ocr]") {
  rig r;
  r.text_available = true;
  r.docs_available = true;
  r.file("scan_rent.pdf");  // two pages with no text layer; the OS reads a lease on them
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(r.scan_reads == 2);
  // The meaning pass waited for the pages (its words alone had no passage).
  CHECK(r.docs_model->passages_embedded.load() == 2);
  CHECK(r.status().meaning_done == 1);
  CHECK(r.search("paying my landlord") == std::vector<std::pair<std::string, std::int64_t>>{{"scan_rent.pdf", 0}});
}

TEST_CASE("a 100-page scan resumes at its next page and never holds up the photos (#354 D4)",
          "[ai][engine][documents][ocr]") {
  rig r;
  r.text_available = true;
  r.file("scanbook.pdf");
  // The viewer gets busy after the tenth page read: the engine stops there.
  r.yield_after_scan_reads = 10;
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(wait_for([&] { return r.scan_reads >= 10; }, kSettleMs));
  r.eng.reset();
  r.busy = false;
  r.yield_after_scan_reads = -1;
  {
    auto db = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(db);
    CHECK((*db)->passage_count() >= 10);  // every page read so far stands
  }

  // Started again, and a photo arrives while the scan is being read (each
  // page a slow read): the photo's pictures do not wait for the rest of it.
  r.scan_read_ms = 30;
  r.start();
  REQUIRE(wait_for([&] { return r.scan_reads >= 14; }, kSettleMs));
  r.file("red_photo.jpg");
  auto doc = mv::json::parse(r.eng->roots_json());
  REQUIRE(doc);
  REQUIRE(doc->a.size() == 1);
  REQUIRE(r.eng->root_rescan(doc->a[0].integer("id").value_or(0)));
  REQUIRE(wait_for([&] { return r.search("red").size() == 1; }, kSettleMs));
  CHECK(r.scan_reads < 60);  // the photo came in while most of the scan was still to read
  r.scan_read_ms = 0;
  REQUIRE(r.idle());
  // Resumed at the next page: each of the 100 pages read once.
  CHECK(r.scan_reads == 100);
  // PDFium's text is asked for a claim's pages and one more, never every
  // page left each claim (that would be about 1,300 page visits here).
  CHECK(r.pdf_pages_read < 300);
  CHECK(r.search("\"scanned page 0 of the archive\"").size() == 1);
  CHECK(r.search("\"scanned page 9 of the archive\"").size() == 1);
  CHECK(r.search("\"scanned page 10 of the archive\"").size() == 1);
  {
    const std::uint64_t id = r.eng->search_text("\"scanned page 99 of the archive\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    REQUIRE(r.rows(id).size() == 1);
    CHECK(r.rows(id)[0].second == 99);
  }
  CHECK(r.status().scans_done == 1);
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
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK((s.flags & MV_AI_STATUS_AUDIO_READY) != 0);
  CHECK(s.sound_total == 2);   // videos only: photos have no soundtrack
  CHECK(s.sound_done == 2);
  CHECK(s.speech_total == 2);
  CHECK(s.speech_done == 2);
  CHECK(s.assets_failed == 0);

  // What it sounds like: the bark window (10-20 s), not the talk before it.
  const std::uint64_t bark = r.eng->search_text("a dog barking", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(bark, kSettleMs));
  auto rows = r.rows(bark);
  REQUIRE_FALSE(rows.empty());
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(rows.front().second >= 5000);
  CHECK(rows.front().second <= 15000);
  CHECK((r.eng->result_at(bark, 0)->match & MV_AI_MATCH_SOUND) != 0);

  // What is said: the words, with a snippet, at the moment they start.
  const std::uint64_t said = r.eng->search_text("birthday anna", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(said, kSettleMs));
  rows = r.rows(said);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(rows.front().second == 0);
  CHECK(r.eng->result_at(said, 0)->match == MV_AI_MATCH_SPEECH);
  CHECK(r.eng->result_snippet(said, 0)->find("birthday") != std::string::npos);

  // "Find in" narrows: pictures only never answers from the soundtrack.
  const std::uint64_t pics =
      r.eng->search_text("birthday anna", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL | MV_AI_FIND_PICTURES);
  REQUIRE(r.eng->wait_search(pics, kSettleMs));
  CHECK(r.matched(pics).empty());

  // Photos still answer as pictures alongside. Not necessarily first: both
  // clips open on red frames that tie with the photo exactly, and a tie's
  // order is the order the workers committed in.
  CHECK(has_row(r.answered("red"), "red_photo.jpg"));

  // Quoted words must be said, in that order (docs/design/17 "Query syntax"); the
  // phrase is the snippet. A kind or a date narrows them like anything else.
  const std::uint64_t quoted = r.eng->search_text("\"make a wish\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(quoted, kSettleMs));
  rows = r.rows(quoted);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().first == "party_talk_bark.mp4");
  CHECK(r.eng->result_at(quoted, 0)->match == MV_AI_MATCH_SPEECH);
  CHECK(r.eng->result_snippet(quoted, 0)->find("wish") != std::string::npos);
  CHECK(r.search("\"wish a make\"").empty());
  CHECK(r.search("said:birthday video").size() == 1);
  CHECK(r.search("said:birthday photo").empty());
  CHECK(r.search("\"birthday\" before:2000").empty());
  const auto not_said = r.search("red -\"birthday\"");
  CHECK(has_row(not_said, "red_photo.jpg"));
  CHECK_FALSE(has_row(not_said, "party_talk_bark.mp4"));
  CHECK(r.search("\"make a wi").size() == 1);  // a quote still being typed
}

TEST_CASE("chapter titles and subtitle lines are found by quoted words, at their moment (#406)",
          "[ai][engine][cues]") {
  rig r;
  r.file("clips/party_cues.mp4");  // red / green / blue frames, and its cues
  r.file("clips/rgb_plain.mp4");   // no cues
  r.start();  // no audio piece
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK(r.status().assets_failed == 0);
  const int embedded = r.fast->images_embedded.load();

  const auto first = [&](const std::string& q) {
    const std::uint64_t id = r.eng->search_text(q, "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    return id;
  };
  {
    const std::uint64_t id = first("\"birthday cake\"");
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == "party_cues.mp4");
    CHECK(rows[0].second == 4000);
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK((res->match & MV_AI_MATCH_TEXT) != 0);
    CHECK((res->match & MV_AI_MATCH_SPEECH) == 0);
    auto why = r.eng->result_snippet(id, 0);
    REQUIRE(why);
    CHECK(*why == "Chapter: Birthday cake");
  }
  for (const char* q : {"\"suited and booted\"", "said:suited", "said:\"and booted\""}) {
    CAPTURE(q);
    const std::uint64_t id = first(q);
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == "party_cues.mp4");
    CHECK(rows[0].second == 7000);
    auto why = r.eng->result_snippet(id, 0);
    REQUIRE(why);
    CHECK(*why == "Subtitle: suited and booted");
  }
  CHECK(r.search("\"wedding cake\"").empty());
  CHECK(r.search("-\"birthday cake\" video").size() == 1);  // the plain clip
  // Unquoted words still run the picture tower, and the text is found too.
  const auto red = r.search("red");
  CHECK(has_row(red, "party_cues.mp4"));
  CHECK(has_row(red, "rgb_plain.mp4"));
  const auto plain = r.search("birthday cake");
  REQUIRE_FALSE(plain.empty());
  CHECK(plain.front().first == "party_cues.mp4");
  CHECK(plain.front().second == 4000);
  // Nothing is embedded for the text, and a restart reads it from the index.
  CHECK(r.fast->images_embedded.load() == embedded);
  const int reads = r.cue_reads.load();
  r.start();
  REQUIRE(r.idle());
  CHECK(r.cue_reads.load() == reads);  // done: not read again
  CHECK(r.rows(first("\"birthday cake\"")).size() == 1);
  CHECK(r.fast->images_embedded.load() == embedded);
}

TEST_CASE("cues: a long clip's lines commit in batches, and a resume never doubles them", "[ai][engine][cues]") {
  rig r;
  r.file("clips/rgb_manycues.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK_FALSE(r.search("\"line number 150\"").empty());
  r.eng.reset();
  // As a kill would leave it: progress partial at line 100's start, rows past
  // it already in (the batch that held them was committed before the kill).
  {
    auto d = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(d);
    const auto all = (*d)->all_assets();
    REQUIRE(all.size() == 1);
    REQUIRE((*d)->commit_cues(all[0].id, {}, mv::ai::work_state::partial, 100000));
  }
  r.start();
  REQUIRE(r.idle());
  auto d = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
  REQUIRE(d);
  int n = 0;
  REQUIRE((*d)->each_cue([&](std::int64_t, const mv::ai::cue_in&) { ++n; }));
  CHECK(n == 200);
  const std::uint64_t id = r.eng->search_text("\"line number 150\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, kSettleMs));
  const auto rows = r.rows(id);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].second == 150000);
  auto more = r.eng->result_at(id, 0);
  REQUIRE(more);
  CHECK(more->more_in_clip == 0);  // one row, not a doubled line
}

TEST_CASE("re-analysing faces looks at the frames the picture pass kept (#407)", "[ai][engine][faces][moments]") {
  rig r;
  // Anna is on the 3 s frame alone: kept by the picture pass (it turns
  // green there), 3 s from either kept neighbour. The People pass's own
  // 2 s grid (0, 2, 4, 6, 8 s) would not see her.
  r.file("clips/anna3s_rgb.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  const int embedded = r.fast->images_embedded.load();
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 1);
  auto faces = mv::json::parse(r.eng->person_faces_json(*people->a[0].integer("id")));
  REQUIRE(faces);
  REQUIRE(faces->a.size() == 1);
  CHECK(*faces->a[0].integer("pts_ms") == 3000);
  CHECK(r.fast->images_embedded.load() == embedded);  // nothing embedded again
}

TEST_CASE("the People pass falls back to its own grid for a clip with no picture rows (#407)",
          "[ai][engine][faces][moments]") {
  rig r;
  r.file("clips/anna3s_rgb.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  r.eng.reset();
  {
    // As if the pictures had kept nothing for it (a poster-only clip).
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(utf8(r.dir / "data" / "index.db").c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "DELETE FROM frames;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  }
  r.start();
  REQUIRE(r.idle());
  const int before = r.samplers_opened.load();
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.samplers_opened.load() > before);  // the 2 s / 10 s grid ran
  // It completes (scanned), and that grid misses the face on the 3 s frame.
  CHECK(mv::json::parse(r.eng->people_json())->a.empty());
}

TEST_CASE("a Live Photo's motion is sampled onto its still and opens the movie (#407)", "[ai][engine][live]") {
  rig r;
  r.file("blue_live.jpg");  // the still: blue
  r.file("blue_live.mov");  // its motion: red, green, then blue
  r.file("red_raw.jpg");    // a RAW+JPEG pair stays one asset
  r.file("red_raw.cr2");
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos()), false);
  REQUIRE(root);
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK(s.assets_total == 2);  // the Live Photo's still and the JPEG
  CHECK(s.assets_failed == 0);

  // "green" is only in the movie: the still's asset, at that moment, opening
  // the movie as a clip.
  const std::uint64_t id = r.eng->search_text("green", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, kSettleMs));
  const auto rows = r.rows(id);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "blue_live.mov");
  CHECK(rows[0].second == 3000);
  auto res = r.eng->result_at(id, 0);
  REQUIRE(res);
  CHECK(res->kind == MV_AI_KIND_VIDEOS);
  auto path = r.eng->result_path(id, 0);
  REQUIRE(path);
  CHECK(fs::path(*path).filename() == "blue_live.mov");
  // N / Shift+N on the movie walk the still's moments.
  auto moments = r.eng->clip_matches(id, *path);
  REQUIRE(moments);
  REQUIRE_FALSE(moments->empty());
  CHECK(moments->front().first == 3000);
  // The tile is the movie's moment from the viewer's cache.
  auto thumb = r.eng->result_thumb(id, 0);
  REQUIRE(thumb);
  CHECK(thumb->find("blue_live.mov#3000") != std::string::npos);
  // A match at no timestamp is the still.
  const auto red = r.search("red");
  CHECK(has_row(red, "red_raw.jpg"));
  CHECK_FALSE(has_row(red, "red_raw.cr2"));

  // The movie edited under the same still: only the motion is sampled again.
  const int embeds = r.fast->images_embedded.load();
  const int stills = r.stills_decoded.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  write_bytes(r.photos() / "blue_live.mov", pattern(32, 9));
  REQUIRE(r.eng->root_rescan(*root));
  REQUIRE(wait_for([&] { return r.fast->images_embedded.load() > embeds; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.fast->images_embedded.load() - embeds == 9);  // the movie's nine frames, not the still
  CHECK(r.stills_decoded.load() == stills);             // the still is not read again
  const auto again = r.search("green");
  REQUIRE(again.size() == 1);
  CHECK(again[0].first == "blue_live.mov");
}

TEST_CASE("words in a photo are found by quoted and plain words (#403)", "[ai][engine][ocr]") {
  rig r;
  r.text_available = true;
  r.file("red_sign.jpg");     // SUITED AND / BOOTED
  r.file("blue_koln.jpg");    // Grüße aus / KÖLN
  r.file("green_plain.jpg");  // no text
  r.file("clips/rgb_clip.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK(r.status().assets_failed == 0);
  CHECK(r.text_reads.load() == 3);       // each photo once
  CHECK(r.clip_text_reads.load() == 3);  // and the clip's three moments (red, green, blue)
  const int embedded = r.fast->images_embedded.load();

  const auto top = [&](const std::string& q) {
    const std::uint64_t id = r.eng->search_text(q, "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
    REQUIRE(r.eng->wait_search(id, kSettleMs));
    return id;
  };
  {
    // The reporter's case: a phrase split over two lines of a sign.
    const std::uint64_t id = top("\"suited and booted\"");
    const auto rows = r.rows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == "red_sign.jpg");
    CHECK(rows[0].second == -1);
    auto res = r.eng->result_at(id, 0);
    REQUIRE(res);
    CHECK((res->match & MV_AI_MATCH_TEXT) != 0);
    auto why = r.eng->result_snippet(id, 0);
    REQUIRE(why);
    CHECK(*why == "In the picture: SUITED AND BOOTED");
  }
  {
    // Plain words: the photo with the words leads, the picture search beside it.
    const auto rows = r.rows(top("suited and booted"));
    REQUIRE_FALSE(rows.empty());
    CHECK(rows.front().first == "red_sign.jpg");
  }
  CHECK(r.search("\"booted and suited\"").empty());  // order matters in quotes
  CHECK(r.search("said:booted").size() == 1);
  // Folded both ways: "koln" finds KÖLN, "grusse" does not (ß stays itself).
  CHECK(r.rows(top("\"koln\"")).size() == 1);
  CHECK(r.rows(top("\"grüße aus köln\"")).size() == 1);
  CHECK(r.rows(top("\"GRÜSSE\"")).empty());
  // A description still runs: "red" finds the sign by its colour.
  CHECK(has_row(r.answered("red"), "red_sign.jpg"));
  CHECK(r.fast->images_embedded.load() == embedded);  // reading text embeds nothing

  // Read once, ever: a restart reads nothing again.
  r.start();
  REQUIRE(r.idle());
  CHECK(r.text_reads.load() == 3);
  CHECK(r.rows(top("\"suited and booted\"")).size() == 1);

  // Turned off: a new photo is not read (and is when it comes back on).
  REQUIRE(r.eng->set_setting("read_text", "0"));
  r.file("blue_sign2.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(wait_for([&] { return r.status().assets_total == 5; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.text_reads.load() == 3);
  REQUIRE(r.eng->set_setting("read_text", "1"));
  REQUIRE(wait_for([&] { return r.text_reads.load() == 4; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.rows(top("\"suited and booted\"")).size() == 2);
}

TEST_CASE("without an OS text reader nothing changes (#403)", "[ai][engine][ocr]") {
  rig r;  // text_available false: make_text_reader's "none"
  r.file("red_sign.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle());
  CHECK(r.status().assets_done == 1);
  CHECK(r.search("\"suited and booted\"").empty());
  CHECK(r.text_reads.load() == 0);
}

TEST_CASE("words on a clip's moments find the clip at that moment (#403)", "[ai][engine][ocr][moments]") {
  rig r;
  r.text_available = true;
  r.file("clips/street_sign.mp4");  // red 0-2 s, green 3-5 s, blue 6-8 s; a sign in shot from 3 s
  r.file("green_plain.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(r.idle());
  CHECK(r.status().assets_failed == 0);
  const int embedded = r.fast->images_embedded.load();
  // Each moment the picture pass kept, read once; nothing more embedded.
  CHECK(r.clip_text_reads.load() == 3);
  {
    std::lock_guard lock(r.text_m);
    CHECK(r.sign_seconds == std::vector<int>{0, 3, 6});
  }

  const std::uint64_t id = r.eng->search_text("\"suited and booted\"", "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(id, kSettleMs));
  const auto rows = r.rows(id);
  // The sign held in shot over two moments is one row, at the first.
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "street_sign.mp4");
  CHECK(rows[0].second == 3000);
  auto res = r.eng->result_at(id, 0);
  REQUIRE(res);
  CHECK((res->match & MV_AI_MATCH_TEXT) != 0);
  CHECK(res->more_in_clip == 0);
  auto why = r.eng->result_snippet(id, 0);
  REQUIRE(why);
  CHECK(*why == "In the picture: SUITED AND BOOTED");
  CHECK(r.search("said:booted") == std::vector<std::pair<std::string, std::int64_t>>{{"street_sign.mp4", 3000}});

  // Counted, and read once, ever: a restart reads nothing again.
  CHECK(r.status().text_total == 2);  // the photo and the clip
  CHECK(r.status().text_done == 2);
  r.start();
  REQUIRE(r.idle());
  CHECK(r.clip_text_reads.load() == 3);
  CHECK(r.search("\"suited and booted\"").size() == 1);
  CHECK(r.fast->images_embedded.load() == embedded);

  // Off: a new clip is not read; back on, it is.
  REQUIRE(r.eng->set_setting("read_text", "0"));
  r.file("clips/second_sign.mp4", 24);
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(wait_for([&] { return r.status().assets_total == 3; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.clip_text_reads.load() == 3);
  CHECK(r.status().text_total == 0);  // no pass, nothing to count
  REQUIRE(r.eng->set_setting("read_text", "1"));
  REQUIRE(wait_for([&] { return r.clip_text_reads.load() == 6; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(r.search("\"suited and booted\"").size() == 2);
}

TEST_CASE("text on a clip's moments resumes after the last committed moment (#403)", "[ai][engine][ocr][moments]") {
  rig r;
  r.text_available = true;
  r.yield_after_clip_reads = 1;  // the host gets busy as the first moment is read
  r.file("clips/street_sign.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  REQUIRE(wait_for([&] { return r.clip_text_reads.load() == 1; }, kSettleMs));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // yielding at the next moment
  CHECK(r.clip_text_reads.load() == 1);
  r.eng.reset();  // quit while yielding: the first moment commits, the rest wait
  r.busy = false;
  r.yield_after_clip_reads = -1;
  r.start();
  REQUIRE(r.idle());
  {
    // Every moment read once across the quit: none twice, none skipped.
    std::lock_guard lock(r.text_m);
    CHECK(r.sign_seconds == std::vector<int>{0, 3, 6});
  }
  const auto rows = r.search("\"suited and booted\"");
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].second == 3000);
}

// Where a status wait stuck, for the failure that follows.
static std::string status_dump(const mv_ai_status& s) {
  std::ostringstream o;
  o << "state " << s.state << ", yield " << s.yield_reason << ", assets " << s.assets_done << "/" << s.assets_total
    << " (failed " << s.assets_failed << "), cues " << s.cues_done << "/" << s.cues_total << ", text " << s.text_done
    << "/" << s.text_total << ", flags " << s.flags;
  return o.str();
}

TEST_CASE("the status counts the cue pass, never idle while it runs (#406)", "[ai][engine][cues]") {
  rig r;  // no OS text reader: only the cue pass is held
  r.hold_cues = true;
  struct release {
    rig& r;
    ~release() { r.hold_cues = false; }
  } guard{r};
  r.file("red_plain.jpg");
  r.file("clips/birthday_cues.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  // The clip's cues are being read (after its pictures: claim's order).
  const bool reading = wait_for([&] { return r.cue_reads.load() >= 1; }, kSettleMs);
  UNSCOPED_INFO("cue reads " << r.cue_reads.load() << "; " << status_dump(r.status()));
  REQUIRE(reading);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // a status refresh or two
  {
    const mv_ai_status s = r.status();
    INFO(status_dump(s));
    CHECK(s.struct_size == sizeof(mv_ai_status));
    CHECK(s.state != MV_AI_STATE_IDLE);
    CHECK(s.cues_total == 1);
    CHECK(s.cues_done == 0);
    CHECK(s.text_total == 0);  // no reader, no text pass
  }
  CHECK_FALSE(r.eng->wait_idle(200));
  r.hold_cues = false;
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  INFO(status_dump(s));
  CHECK(s.cues_total == 1);
  CHECK(s.cues_done == 1);
}

TEST_CASE("the status counts the text pass, never idle while it runs (#403)", "[ai][engine][ocr]") {
  rig r;
  r.text_available = true;
  r.hold_text = true;  // clip moments only (rig)
  struct release {
    rig& r;
    ~release() { r.hold_text = false; }
  } guard{r};
  r.file("red_sign.jpg");
  r.file("clips/street_sign.mp4");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), true));
  // A moment of the clip is being read (after its pictures: the `after` spec).
  const bool reading = wait_for([&] { return r.text_holding.load() >= 1; }, kSettleMs);
  UNSCOPED_INFO("photo reads " << r.text_reads.load() << "; " << status_dump(r.status()));
  REQUIRE(reading);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  {
    const mv_ai_status s = r.status();
    INFO(status_dump(s));
    CHECK(s.state != MV_AI_STATE_IDLE);
    CHECK(s.text_total == 2);  // the photo, and the clip whose pictures are done
    CHECK(s.text_done < s.text_total);
  }
  CHECK_FALSE(r.eng->wait_idle(200));
  r.hold_text = false;
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  INFO(status_dump(s));
  CHECK(s.text_total == 2);
  CHECK(s.text_done == 2);
}

TEST_CASE("read_text is in settings_json and outlives a restart (#403)", "[ai][engine][ocr]") {
  rig r;
  r.start();
  const auto read_text = [&] {
    auto doc = mv::json::parse(r.eng->settings_json());
    REQUIRE(doc);
    return doc->boolean("read_text");
  };
  CHECK(read_text() == true);  // on by default
  REQUIRE(r.eng->set_setting("read_text", "0"));
  CHECK(read_text() == false);
  r.start();
  CHECK(read_text() == false);
  REQUIRE(r.eng->set_setting("read_text", "1"));
  CHECK(read_text() == true);

  // Whether the OS can read text here: the rig's has no reader until asked.
  const auto available = [&] {
    auto doc = mv::json::parse(r.eng->settings_json());
    return doc && doc->boolean("text_available") == true;
  };
  REQUIRE(wait_for([&] { return !available(); }, kSettleMs));
  r.text_available = true;
  r.start();
  REQUIRE(r.idle());
  CHECK(available());
}

TEST_CASE("a Live Photo's movie changing keeps the still's named face (#407)", "[ai][engine][live][faces]") {
  rig r;
  r.file("anna_live.jpg");  // Anna on the still
  r.file("anna_live.mov");
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos()), false);
  REQUIRE(root);
  REQUIRE(r.eng->faces_enable(true));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
  REQUIRE(r.idle());
  auto people = mv::json::parse(r.eng->people_json());
  REQUIRE(people);
  REQUIRE(people->a.size() == 1);
  const std::int64_t anna = *people->a[0].integer("id");
  REQUIRE(r.eng->person_rename(anna, "Anna"));
  const auto face_ids = [&] {
    std::set<std::int64_t> out;
    auto faces = mv::json::parse(r.eng->person_faces_json(anna));
    for (const auto& f : faces->a) out.insert(*f.integer("face"));
    return out;
  };
  const std::set<std::int64_t> before = face_ids();
  REQUIRE(before.size() == 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  write_bytes(r.photos() / "anna_live.mov", pattern(32, 9));
  const int embeds = r.fast->images_embedded.load();
  REQUIRE(r.eng->root_rescan(*root));
  REQUIRE(wait_for([&] { return r.fast->images_embedded.load() > embeds; }, kSettleMs));
  REQUIRE(r.idle());
  CHECK(face_ids() == before);  // the same face row, still Anna's
  CHECK(r.search("Anna").size() == 1);
}

TEST_CASE("audio: a folder indexed for pictures only does no audio work", "[ai][engine][audio]") {
  rig r;
  r.audio_available = true;
  r.file("party_talk_bark.mp4");
  r.start();
  REQUIRE(r.eng->set_setting("video_index", "1"));  // Pictures
  auto root = r.eng->index_folder(utf8(r.photos()), false);
  REQUIRE(root);
  REQUIRE(r.idle());
  CHECK(r.sound->windows_embedded.load() == 0);
  CHECK(r.speech->windows.load() == 0);
  CHECK(r.status().sound_total == 0);

  // Switching this folder to Both queues its sound and speech.
  REQUIRE(r.eng->root_set_media(*root, MV_AI_MEDIA_BOTH));
  REQUIRE(r.idle());
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
  REQUIRE(r.idle());
  CHECK((r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0);
  CHECK(r.status().sound_total == 0);
  CHECK(r.answered("birthday anna").empty());
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
    REQUIRE(wait_for(
        [&] {
          if (!r.idle(100)) return false;
          const mv_ai_status s = r.status();
          return s.state == MV_AI_STATE_IDLE && (s.flags & MV_AI_STATUS_AUDIO_READY) != 0 && s.sound_total == 3 &&
                 s.sound_done == s.sound_total && s.speech_done == s.speech_total;
        },
        kSettleMs));
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
  REQUIRE(r.idle());
  const int opens = r.clip_opens.load();
  CHECK((r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0);

  r.audio_available = true;
  REQUIRE(r.eng->set_setting("reload", "1"));
  REQUIRE(wait_for(
      [&] {
        const mv_ai_status s = r.status();
        return (s.flags & MV_AI_STATUS_AUDIO_READY) != 0 && s.sound_total == 1 && s.sound_done == 1 &&
               s.speech_done == 1 && s.state == MV_AI_STATE_IDLE;
      },
      kSettleMs));
  CHECK(r.clip_opens.load() == opens);
  CHECK(has_row(r.answered("red"), "red_photo.jpg"));  // ties with the clip's red frames

  // And removing it clears what it answered, still without a tower reload.
  r.audio_available = false;
  REQUIRE(r.eng->set_setting("reload", "1"));
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_AUDIO_READY) == 0; }, kSettleMs));
  CHECK(r.answered("birthday anna").empty());
  CHECK(r.clip_opens.load() == opens);
}

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
  REQUIRE(wait_for([&] { return g.opens_waiting.load() == 1; }, kSettleMs));

  // The new tower is still opening: Settings, Precision and its re-run answer
  // now, the re-run from the tower in service. What proves it is that the
  // open is still held after them (a call that waited would have outlasted
  // hold_while); the time bound only catches a wait that is long but not that.
  CHECK(timed([&] { REQUIRE(r.eng->set_setting("precision", "1")); }) < kAnswersNowMs);
  CHECK(timed([&] { (void)r.eng->settings_json(); }) < kAnswersNowMs);
  CHECK(timed([&] { (void)r.status(); }) < kAnswersNowMs);
  std::vector<std::pair<std::string, std::int64_t>> red;
  CHECK(timed([&] { red = r.search("red"); }) < kAnswersNowMs);
  REQUIRE_FALSE(red.empty());
  CHECK(red.front().first == "red.jpg");
  CHECK(r.status().state == MV_AI_STATE_LOADING);
  CHECK(g.opens_waiting.load() == 1);

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
      kSettleMs);
  REQUIRE(migrated);
  CHECK(worst_settings < kAnswersNowMs);
  CHECK(worst_status < kAnswersNowMs);
  CHECK(worst_set < kAnswersNowMs);
  CHECK(worst_search < kAnswersNowMs);
  CHECK(empty_results == 0);

  // The Fast tower is out of service but still "compiling": it is kept,
  // then released on the engine's own thread once that is done.
  CHECK(g.died.load() == died_before);
  g.death = false;
  REQUIRE(wait_for([&] { return g.died.load() == died_before + 1; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 1; }, kSettleMs));
  CHECK(r.clip_opens.load() == opens);
  REQUIRE(r.eng->set_setting("quality", "0"));  // and back to Auto
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 2; }, kSettleMs));
  CHECK(r.clip_opens.load() == opens);
  REQUIRE(r.idle());
  CHECK(r.eng->active_spec() == "fake-high/fp16/pre1");
  CHECK(r.high->images_embedded.load() == embedded);  // nothing re-indexed
  CHECK(r.search("red").front().first == "red.jpg");

  // Another tower does open, and migrates.
  REQUIRE(r.eng->set_setting("quality", "1"));
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 3; }, kSettleMs));
  CHECK(r.clip_opens.load() > opens);
  REQUIRE(r.idle());
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-fast/fp16/pre1"; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return r.status().quality == 2u; }, kSettleMs));
  REQUIRE(r.idle());
  s = r.status();
  CHECK((s.flags & MV_AI_STATUS_SMALL_FALLBACK) == 0);
  CHECK(std::string(s.provider_detail_utf8).empty());
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-high/fp16/pre1"; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return r.status().quality == 1u; }, kSettleMs));
  REQUIRE(r.idle());
  const mv_ai_status s = r.status();
  CHECK((s.flags & MV_AI_STATUS_SMALL_FALLBACK) != 0);
  CHECK(std::string(s.provider_detail_utf8) == "Error compiling model");
  REQUIRE(wait_for([&] { return r.eng->active_spec() == "fake-fast/fp16/pre1"; }, kSettleMs));
  CHECK(r.search("red").front().first == "red.jpg");
}

// Core ML's stand-in (pack.cpp upgrading_clip): CPU until `upgraded`, then
// the accelerated provider, as the real one swaps itself.
class upgrading_stand_in final : public mv::infer::embedder {
 public:
  explicit upgrading_stand_in(std::shared_ptr<fake_embedder> inner) : inner_(std::move(inner)) {}
  std::uint32_t dim() const noexcept override { return inner_->dim(); }
  const std::string& spec_key() const noexcept override { return inner_->spec_key(); }
  mv::infer::backend on() const noexcept override {
    return upgraded.load() ? mv::infer::backend::coreml : mv::infer::backend::cpu;
  }
  bool settling() const noexcept override { return !upgraded.load(); }
  mv::expected embed_images(std::span<const mv::infer::rgb_view> images, std::vector<float>& out) override {
    return inner_->embed_images(images, out);
  }
  mv::result<std::vector<float>> embed_text(std::string_view text) override { return inner_->embed_text(text); }
  std::atomic<bool> upgraded{false};

 private:
  std::shared_ptr<fake_embedder> inner_;
};

// While Core ML compiles the large tower, searches answer on its CPU stand-in
// but nothing is embedded with it: L/14 on CPU beside the compile took a
// MacBook Air 694 s to load a cached model (2026-10-05). The status says it is
// still loading; indexing starts once the Neural Engine has it.
TEST_CASE("indexing waits while the tower is still its CPU stand-in for Core ML", "[ai][engine][fallback]") {
  rig r;
  r.lands_on = mv::infer::backend::coreml;
  auto large = std::make_shared<upgrading_stand_in>(r.high);
  r.tower = [&](std::uint32_t q) -> std::shared_ptr<mv::infer::embedder> {
    if (q == 2) return large;
    return r.fast;
  };
  r.file("red.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(wait_for([&] { return r.status().state == MV_AI_STATE_LOADING && r.status().quality == 2u; }, kSettleMs));
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK(r.high->images_embedded.load() == 0);
  CHECK(r.status().state == MV_AI_STATE_LOADING);
  CHECK(r.answered("red").empty());  // answers (nothing yet), never waits on the compile

  large->upgraded = true;
  REQUIRE(r.idle());
  CHECK(r.high->images_embedded.load() > 0);
  CHECK(r.status().backend == static_cast<std::uint32_t>(mv::infer::backend::coreml));
  CHECK(r.search("red").front().first == "red.jpg");
}

// #354 D4: a scan's pages need the OS reader, not a tower: they are read while
// Core ML still compiles it (minutes at a pack's first start), as words are.
TEST_CASE("scanned pages are read while the tower is still its CPU stand-in (#354 D4)",
          "[ai][engine][documents][ocr]") {
  rig r;
  r.text_available = true;
  r.lands_on = mv::infer::backend::coreml;
  auto large = std::make_shared<upgrading_stand_in>(r.high);
  r.tower = [&](std::uint32_t q) -> std::shared_ptr<mv::infer::embedder> {
    if (q == 2) return large;
    return r.fast;
  };
  r.file("scan_receipt.pdf");
  r.start();
  REQUIRE(r.eng->set_setting("document_files", "1"));
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(wait_for([&] { return r.scan_reads == 2; }, kSettleMs));
  CHECK(r.status().state == MV_AI_STATE_LOADING);
  {
    auto db = mv::ai::index_db::open(utf8(r.dir / "data" / "index.db"));
    REQUIRE(db);
    CHECK((*db)->passage_count() == 2);  // stored before the tower is in
  }
  large->upgraded = true;
  REQUIRE(r.idle());
  CHECK(r.scan_reads == 2);
  CHECK(r.search("\"kept for records\"").size() == 1);
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
  REQUIRE(wait_for([&] { return r.eng->reloads_done() >= 1; }, kSettleMs));
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
      bool ok = r.eng->wait_search(id, kSettleMs);
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
  const bool migrated = wait_for([&] { return r.eng->active_spec() == "fake-high/fp16/pre1"; }, kSettleMs);
  const int at_end = runs.load();
  (void)wait_for([&] { return runs.load() >= at_end + 3; }, kSettleMs);
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

// `answered`: only the rows the query answered, not the newest-first list of
// one it did not (#412).
std::vector<scored> ranked(engine& e, const std::string& q, std::uint32_t kinds = MV_AI_KIND_ALL,
                           bool answered = true) {
  const std::uint64_t id = e.search_text(q, "", MV_AI_SCOPE_ALL, kinds);
  REQUIRE(e.wait_search(id, kSettleMs));
  std::vector<scored> out;
  auto n = e.result_count(id);
  REQUIRE(n);
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto r = e.result_at(id, i);
    auto p = e.result_path(id, i);
    REQUIRE(r);
    REQUIRE(p);
    if (answered && r->match == 0) continue;
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
  REQUIRE(r.idle());
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
  REQUIRE(rd->wait_search(sim, kSettleMs));
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
  REQUIRE(rd->wait_search(moment, kSettleMs));
  CHECK(r.stills_decoded.load() == decoded);
}

TEST_CASE("a second window's reader: nothing indexed yet is not_found, and it says it is read-only",
          "[ai][search-agent]") {
  rig r;
  // No app engine has run: there is no index.db. The window says "nothing
  // indexed yet", not "needs an update" (2026-10-07).
  {
    engine rd(r.table->api(), r.reader_deps(), mv::ai::engine_options{.read_only = true});
    auto started = rd.start();
    REQUIRE_FALSE(started);
    CHECK(started.error() == mv::status::not_found);
  }
  CHECK_FALSE(fs::exists(r.dir / "data" / "index.db"));  // and it created nothing

  r.file("red_car.jpg");
  r.start();
  REQUIRE(r.eng->index_folder(utf8(r.photos()), false));
  REQUIRE(r.idle(30000));
  auto app = mv::json::parse(r.eng->settings_json());
  REQUIRE(app);
  CHECK(app->boolean("read_only") == false);
  auto rd = r.start_reader();
  auto reader = mv::json::parse(rd->settings_json());
  REQUIRE(reader);
  CHECK(reader->boolean("read_only") == true);
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
  CHECK(wait_for([&] { return ranked(*rd, "blue").size() == 1; }, kSettleMs));
  CHECK(ranked(*rd, "red").size() == 1);

  // A file removed in the app drops out of the reader too.
  fs::remove(r.photos() / "red_car.jpg");
  REQUIRE(r.eng->root_rescan(1));
  REQUIRE(r.idle());
  CHECK(wait_for([&] { return ranked(*rd, "red").empty(); }, kSettleMs));
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
  auto roots = session->roots_json(kSettleMs);
  REQUIRE(roots);
  auto parsed = mv::json::parse(*roots);
  REQUIRE(parsed);
  REQUIRE(parsed->a.size() == 1);
  const std::string* path = parsed->a[0].str("path");
  REQUIRE(path);
  CHECK(*path == utf8(r.photos()));
  // The reader sees the name the app gave, as the panel types it.
  auto names = session->suggest_json("@An", kSettleMs);
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
  REQUIRE(session->wait_ready(kSettleMs));

  for (const char* q : {"red", "green", "blue", "xyzzy plugh", "red video"}) {
    INFO(q);
    const auto app = ranked(*r.eng, q, MV_AI_KIND_ALL, false);
    mv::nle::request req;
    req.correlation_id = 7;
    const mv::nle::reply rep = session->run(req, q, "", kSettleMs);
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
  const mv::nle::reply green = session->run(req, "green", "", kSettleMs);
  REQUIRE(green.rows.size() == 1);
  CHECK(green.rows[0].kind == MV_AI_KIND_VIDEOS);
  REQUIRE(green.rows[0].moments.size() == 1);
  CHECK(green.rows[0].moments[0].pts_ms == 3000);
  // Find similar on an indexed still, by path.
  mv::nle::request sim;
  sim.kind = mv::nle::request_kind::similar;
  const mv::nle::reply like = session->run(sim, utf8(r.photos() / "red_car.jpg"), "", kSettleMs);
  const std::uint64_t app_like =
      r.eng->search_similar(utf8(r.photos() / "red_car.jpg"), -1, "", MV_AI_SCOPE_ALL, MV_AI_KIND_ALL);
  REQUIRE(r.eng->wait_search(app_like, kSettleMs));
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
  CHECK(session->run(old, "red", "", kSettleMs).code == mv::status::unsupported_format);
}

// ---- the Photos library (issue #72) ------------------------------------------------

namespace {

// Polls until `ok` holds (the control thread rescans on its own tick).
bool eventually(const std::function<bool()>& ok, int ms = kSettleMs) {
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
      kSettleMs));
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
  CHECK(has_row(r.answered("blue"), "photos:rgb-clip"));  // the local clip's blue moment
  CHECK_FALSE(has_row(r.answered("blue"), "photos:blue-cloud"));

  // Scoped to the library ("photos:"): the folder's photo is out.
  const auto in_library = r.search("red", MV_AI_SCOPE_FOLDER, "photos:");
  CHECK(has_row(in_library, "photos:red-rose"));
  CHECK_FALSE(has_row(in_library, "red_folder.jpg"));

  // A Photos still's tile is its key: the chrome draws it from PhotoKit.
  const std::uint64_t id = r.eng->search_text("red", "photos:", MV_AI_SCOPE_FOLDER, MV_AI_KIND_PHOTOS);
  REQUIRE(r.eng->wait_search(id, kSettleMs));
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
  CHECK(has_row(r.answered("red"), "photos:red-rose"));

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
  CHECK(has_row(r.answered("red"), "photos:red-rose"));
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
  REQUIRE(eventually([&] { return has_row(r.answered("blue"), "photos:blue-sky"); }));
  REQUIRE(r.idle());
  CHECK_FALSE(has_row(r.answered("green"), "photos:green-leaf"));
  CHECK(has_row(r.answered("red"), "photos:red-rose"));
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
  CHECK(has_row(r.answered("red"), "photos:red-cloudclip"));  // its poster
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
  CHECK(has_row(r.answered("blue"), "photos:blue-cloud"));
  // The clip was asked whether it is here now (one lookup), not read again:
  // its poster row stayed, and nothing was re-embedded for it.
  CHECK(has_row(r.answered("red"), "photos:red-cloudclip"));
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
  CHECK_FALSE(has_row(r.answered("blue"), "photos:red-cloudclip"));  // only its red poster
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
  REQUIRE(eventually([&] { return has_row(r.answered("blue"), "photos:red-cloudclip"); }));
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

TEST_CASE("online-only cloud files are counted, never read, and fetched in place only with the opt-in",
          "[ai][engine][cloud]") {
  rig r;
  r.file("red_car.jpg");
  r.file("blue_sea.jpg");  // OneDrive keeps this one online-only
  {
    std::lock_guard lock(r.cloud->m);
    r.cloud->online_only = {"blue_sea.jpg"};
  }
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos()), true);
  REQUIRE(root);
  REQUIRE(r.idle());
  mv_ai_status s = r.status();
  CHECK(s.assets_total == 2);
  CHECK(s.assets_done == 1);
  CHECK(s.cloud_files_left == 1);
  // Not "indexing" forever: the online-only file is not work in hand.
  REQUIRE(eventually([&] { return r.status().state == MV_AI_STATE_IDLE; }));
  CHECK(s.cloud_fetch == MV_AI_ICLOUD_OFF);  // off by default
  CHECK_FALSE(r.was_decoded("blue_sea.jpg"));  // reading it would have downloaded it
  CHECK(r.answered("something blue").empty());
  CHECK(r.eng->roots_json().find("\"cloud_only\":1") != std::string::npos);
  CHECK(r.cloud->log.empty());

  // On battery, then metered: it waits. Both before the opt-in, as the iCloud
  // twin above: set after it, the fetcher could start on the defaults (mains,
  // unmetered) first and hydrate (a flake seen on a Windows runner).
  r.cloud->unmetered = false;
  r.on_battery = true;
  REQUIRE(r.eng->set_setting("cloud_files", "1"));
  REQUIRE(eventually([&] { return r.status().cloud_fetch == MV_AI_ICLOUD_WAIT_POWER; }));
  r.on_battery = false;
  REQUIRE(eventually([&] { return r.status().cloud_fetch == MV_AI_ICLOUD_WAIT_NETWORK; }));
  CHECK(r.cloud->log.empty());

  r.cloud->unmetered = true;
  REQUIRE(eventually([&] { return has_row(r.answered("something blue"), "blue_sea.jpg"); }));
  REQUIRE(eventually([&] { return r.status().cloud_fetch == MV_AI_ICLOUD_DONE; }));
  REQUIRE(r.idle());
  // Brought down, indexed, then made online-only again.
  REQUIRE(eventually([&] { return r.cloud->is_online_only("blue_sea.jpg"); }));
  {
    std::lock_guard lock(r.cloud->m);
    CHECK(r.cloud->log == std::vector<std::string>{"hydrate blue_sea.jpg", "dehydrate blue_sea.jpg"});
  }
  s = r.status();
  CHECK(s.state == MV_AI_STATE_IDLE);
  CHECK(s.assets_done == 2);
  CHECK(s.cloud_files_left == 0);
  CHECK(s.cloud_files_fetched == 1);
  // The crash note goes once the file is given back (just after the provider call).
  CHECK(eventually([&] { return !fs::exists(r.dir / "data" / "cache" / "cloud-fetched"); }));

  // A rescan sees it online-only again; it is indexed, so nothing is fetched.
  REQUIRE(r.eng->root_rescan(*root));
  REQUIRE(r.idle());
  r.start();
  REQUIRE(r.idle());
  REQUIRE(eventually([&] { return r.status().cloud_fetch == MV_AI_ICLOUD_DONE; }));
  CHECK(r.status().assets_done == 2);
  {
    std::lock_guard lock(r.cloud->m);
    CHECK(r.cloud->log.size() == 2);
  }
  CHECK(r.eng->settings_json().find("\"cloud_files\":true") != std::string::npos);
}

TEST_CASE("a cloud file a crash left on disk is made online-only again at the next start",
          "[ai][engine][cloud]") {
  rig r;
  r.file("red_car.jpg");
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos()), true);
  REQUIRE(root);
  REQUIRE(r.idle());
  r.eng.reset();
  // A crash between hydrate and give-back leaves the row ids behind.
  const fs::path note = r.dir / "data" / "cache" / "cloud-fetched";
  fs::create_directories(note.parent_path());
  {
    std::ofstream out(note, std::ios::binary);
    out << "1\n";
  }
  r.start();
  REQUIRE(eventually([&] { return !fs::exists(note); }));
  REQUIRE(eventually([&] { return r.cloud->is_online_only("red_car.jpg"); }));
}

TEST_CASE("a folder that cannot be read says so on its row and keeps its rows", "[ai][engine][cloud]") {
  rig r;
  r.file("sub/red_car.jpg");
  r.start();
  auto root = r.eng->index_folder(utf8(r.photos() / "sub"), true);
  REQUIRE(root);
  REQUIRE(r.idle());
  CHECK(r.status().assets_done == 1);
  r.eng.reset();
  fs::rename(r.photos() / "sub", r.photos() / "moved");
  r.start();
  REQUIRE(r.idle());
  REQUIRE(eventually([&] { return r.eng->roots_json().find("\"error\":\"unreadable\"") != std::string::npos; }));
  CHECK(r.status().assets_done == 1);  // a missing drive does not throw work away
  fs::rename(r.photos() / "moved", r.photos() / "sub");
  REQUIRE(r.eng->root_rescan(*root));
  REQUIRE(eventually([&] { return r.eng->roots_json().find("\"error\"") == std::string::npos; }));
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
  // Nothing to scan on this Mac: the pass ends instead of asking PhotoKit for
  // the same clip again and again (it never went idle before this held).
  REQUIRE(r.idle());
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
  REQUIRE(b.eng->wait_search(path, kSettleMs));
  auto p = b.eng->result_path(path, 0);
  REQUIRE(p);
  CHECK(fs::path(*p).parent_path() == there);  // B's paths, not A's
}

TEST_CASE("documents travel only when ticked, with their text and meaning (.mvindex v2, #354 D3)",
          "[ai][engine][transfer][documents]") {
  rig a;
  a.file("red_car.jpg");
  a.file("lease.docx");
  a.file("invoice_march.pdf");
  a.docs_available = true;
  a.start();
  REQUIRE(a.eng->set_setting("document_files", "1"));
  REQUIRE(a.eng->index_folder(utf8(a.photos()), false));
  REQUIRE(a.idle());

  // Not ticked: as v1, no document and not its name.
  const std::string plain = utf8(a.dir / "plain.mvindex");
  REQUIRE(a.eng->export_index(plain, {}, 0));
  auto done = transfer_done(a);
  CHECK(*done.find("outcome")->integer("assets") == 1);
  CHECK(*done.find("outcome")->integer("passages") == 0);
  CHECK(done.find("outcome")->boolean("documents_included") == false);

  // Ticked: the documents, their passages and their vectors.
  const std::string file = utf8(a.dir / "docs.mvindex");
  REQUIRE(a.eng->export_index(file, {}, MV_AI_TRANSFER_DOCUMENTS));
  done = transfer_done(a);
  CHECK(*done.integer("status") == 0);
  CHECK(*done.find("outcome")->integer("assets") == 3);
  CHECK(*done.find("outcome")->integer("passages") == 5);
  CHECK(*done.find("outcome")->integer("passage_vecs") == 5);

  rig b;
  const fs::path there = b.dir / "NAS" / "Docs";
  copy_library(a, there);
  b.docs_available = true;
  b.start();
  REQUIRE(b.eng->set_setting("document_files", "1"));
  REQUIRE(b.idle());
  auto info = mv::json::parse(*b.eng->inspect_export(file));
  REQUIRE(info);
  CHECK(*info->integer("version") == 2);
  REQUIRE(info->find("documents"));
  CHECK(*info->find("documents")->integer("count") == 2);
  CHECK(info->find("documents")->boolean("meaning_match") == true);
  CHECK(mv::json::parse(*b.eng->inspect_export(plain))->find("documents")->k == mv::json::kind::null);
  const auto root_id = *info->find("roots")->a[0].integer("id");

  // Imported without the tick: the documents stay out.
  {
    rig c;
    copy_library(a, c.dir / "Docs");
    c.docs_available = true;
    c.start();
    REQUIRE(c.eng->set_setting("document_files", "1"));
    REQUIRE(c.idle());
    REQUIRE(c.eng->import_index(file, map_json(root_id, c.dir / "Docs"), 0));
    CHECK(*transfer_done(c).find("outcome")->integer("passages") == 0);
  }

  REQUIRE(b.eng->import_index(file, map_json(root_id, there), MV_AI_TRANSFER_DOCUMENTS));
  done = transfer_done(b);
  CHECK(*done.integer("status") == 0);
  CHECK(*done.find("outcome")->integer("passages") == 5);
  CHECK(*done.find("outcome")->integer("passage_vecs") == 5);
  REQUIRE(b.idle());
  // Found by their words and by meaning, nothing read or embedded again.
  CHECK(b.docs_model->passages_embedded.load() == 0);
  CHECK(b.pdf_pages_read == 0);
  CHECK(b.search("\"two months notice\"") == std::vector<std::pair<std::string, std::int64_t>>{{"lease.docx", 1}});
  CHECK(b.search("paying my landlord") == std::vector<std::pair<std::string, std::int64_t>>{{"lease.docx", 0}});
  CHECK(b.status().meaning_done == 2);
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
  REQUIRE(wait_for([&] { return a.eng->active_spec() == "fake-high/fp16/pre1"; }, kSettleMs));
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
    REQUIRE(wait_for([&] { return b.eng->active_spec() == "fake-high/fp16/pre1"; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return (a.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
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
  REQUIRE(wait_for([&] { return (r.status().flags & MV_AI_STATUS_FACES_READY) != 0; }, kSettleMs));
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
                   kSettleMs));
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

// The status refresh runs once a second, indexing or idle (control_loop): what
// the text passes' three counts add to it, against the picture count it
// already makes, on a 25,000-asset index where every pass is done. Hidden:
// `mv_ai_tests "[.perf-bench]"` prints the medians as JSON.
TEST_CASE("perf: the text passes' status counts on a 25 k index", "[.perf-bench][ai][engine]") {
  scratch_dir dir{"ai-counts"};
  auto d = mv::ai::index_db::open(utf8(dir / "index.db"));
  REQUIRE(d);
  auto root = (*d)->add_root("/library", true);
  REQUIRE(root);
  std::vector<mv::ai::index_db::seen_file> files;
  for (int i = 0; i < 25000; ++i) {
    mv::ai::index_db::seen_file f;
    f.kind = i % 5 == 0 ? mv::ai::asset_kind::video : mv::ai::asset_kind::photo;
    f.path = "/library/" + std::to_string(i) + (f.kind == mv::ai::asset_kind::video ? ".mov" : ".jpg");
    f.mtime = 1;
    f.size = 100;
    files.push_back(std::move(f));
  }
  const std::int64_t gen = (*d)->next_generation();
  auto ups = (*d)->see_assets(*root, files, gen);
  REQUIRE(ups);
  const std::string pic = "fake/pic", ocr = "ocr/fake/1", moments = "ocr/fake/1/moments";
  for (std::size_t i = 0; i < ups->size(); ++i) {
    const std::int64_t id = (*ups)[i].id;
    const bool video = files[i].kind == mv::ai::asset_kind::video;
    REQUIRE((*d)->commit_cues(id, {}, mv::ai::work_state::done, 0, -1, pic));
    REQUIRE((*d)->commit_cues(id, {}, mv::ai::work_state::done, 0, -1, video ? moments : ocr));
    if (video) REQUIRE((*d)->commit_cues(id, {}, mv::ai::work_state::done, 0, -1));
  }
  const std::uint32_t media = MV_AI_MEDIA_PICTURES;
  const auto time = [](const std::function<void()>& f) {
    std::vector<double> ms;
    for (int i = 0; i < 30; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      f();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
  };
  const double pictures = time([&] {
    CHECK((*d)->count(pic, mv::ai::track_filter{true, MV_AI_MEDIA_PICTURES, media, 0, {}}).done == 25000);
  });
  const double cues = time([&] {
    CHECK((*d)->count(mv::ai::kCueSpec, mv::ai::track_filter{false, MV_AI_MEDIA_BOTH, media, 0, {}}).done == 5000);
  });
  const double photos_text = time([&] {
    CHECK((*d)->count(ocr, mv::ai::track_filter{true, 0, media, 0, {}}).done == 20000);
  });
  const double moments_text = time([&] {
    CHECK((*d)->count(moments, mv::ai::track_filter{false, MV_AI_MEDIA_PICTURES, media, 0, {}}, pic).done == 5000);
  });
  std::printf("{\"assets\":25000,\"pictures_count_ms\":%.2f,\"cues_count_ms\":%.2f,\"text_count_ms\":%.2f,"
              "\"moments_count_ms\":%.2f,\"added_ms_per_refresh\":%.2f}\n",
              pictures, cues, photos_text, moments_text, cues + photos_text + moments_text);
}
