// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The audio index's front end (docs/design/17 "Audio", 2026-09-27): log-mel features
// against transformers' numpy reference, and, with the ai-audio piece staged,
// the CLAP tokenizer and towers against ORT-Python and Whisper on a clip whose
// words are known.
//
//   always              FFT and both feature extractors (tests/data/ai/audio_reference.json)
//   MV_AI_PACK_DIR +    the Core pack's runtime and
//   MV_AI_AUDIO_DIR     the staged ai-audio piece: tokenizer golden, CLAP tolerance
//   MV_AI_SPEECH_CLIP   a clip with known speech (tools/ai-reference/make_speech_clip.ps1):
//                       Whisper finds the words at the right times
#include "catch_compat.h"

#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "core/json.h"
#include "infer/audio_features.h"
#include "infer/audio_models.h"
#include "infer/models.h"
#if defined(MV_AI_TEST_DECODE)
#include "abi/addon_media.h"
#endif

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

// tools/ai-reference/make_audio_reference.py's signals.
std::vector<float> signal(int kind, std::uint32_t rate) {
  const std::size_t n = static_cast<std::size_t>(rate) * (kind == 0 ? 3 : 12);
  std::vector<float> x(n);
  constexpr double pi = 3.14159265358979323846;
  std::uint64_t state = 12345;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    if (kind == 0) {
      x[i] = static_cast<float>(0.5 * std::sin(2 * pi * 440.0 * t));
    } else {
      state = (state * 1103515245ull + 12345ull) & 0x7FFFFFFFull;
      const double noise = static_cast<double>(state) / 0x7FFFFFFF - 0.5;
      const double v = 0.3 * std::sin(2 * pi * 220.0 * t) +
                       0.2 * std::sin(2 * pi * 1760.0 * t) * (0.5 + 0.5 * std::sin(2 * pi * 0.5 * t));
      x[i] = static_cast<float>(v + 0.05 * noise);
    }
  }
  return x;
}

double num(const mv::json::value& v) { return v.is_integer ? static_cast<double>(v.i) : v.d; }

mv::json::value reference() {
  auto doc = mv::json::parse(read_text(fs::path(MV_AI_TEST_DATA) / "audio_reference.json"), 8);
  REQUIRE(doc);
  return *doc;
}

}  // namespace

TEST_CASE("the mixed-radix FFT matches a direct DFT", "[ai][audio]") {
  for (std::size_t n : {8u, 12u, 20u, 400u, 1024u}) {
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = std::sin(0.37 * i) + 0.25 * std::cos(1.3 * i * i);
    mv::infer::real_fft fft(n);
    std::vector<std::complex<double>> got;
    fft.run(x, got);
    REQUIRE(got.size() == n / 2 + 1);
    for (std::size_t k = 0; k <= n / 2; ++k) {
      std::complex<double> want = 0;
      for (std::size_t j = 0; j < n; ++j) want += x[j] * std::polar(1.0, -2 * 3.14159265358979323846 * j * k / n);
      INFO("n " << n << " k " << k);
      CHECK(std::abs(got[k] - want) < 1e-6 * n);
    }
  }
}

TEST_CASE("CLAP and Whisper features match transformers' extractors", "[ai][audio]") {
  const auto ref = reference();
  mv::infer::clap_features clap;
  mv::infer::whisper_features whisper;
  for (int k = 0; k < 2; ++k) {
    const std::string key = std::to_string(k);
    std::vector<float> c;
    clap.compute(signal(k, 48000), c);
    REQUIRE(c.size() == 1001u * 64u);
    const mv::json::value* cr = ref.find("clap")->find(key);
    for (const auto& [row, vals] : cr->find("rows")->o) {
      const std::size_t r = std::stoul(row);
      for (std::size_t j = 0; j < 64; ++j) {
        INFO("clap signal " << k << " row " << r << " mel " << j);
        CHECK(std::fabs(c[r * 64 + j] - num(vals.a[j])) < 0.02);  // dB
      }
    }
    const double cmean = std::accumulate(c.begin(), c.end(), 0.0) / static_cast<double>(c.size());
    CHECK(std::fabs(cmean - num(*cr->find("mean"))) < 0.01);

    std::vector<float> w;
    whisper.compute(signal(k, 16000), w);
    REQUIRE(w.size() == 80u * 3000u);
    const mv::json::value* wr = ref.find("whisper")->find(key);
    for (const auto& [col, vals] : wr->find("cols")->o) {
      const std::size_t f = std::stoul(col);
      for (std::size_t j = 0; j < 80; ++j) {
        INFO("whisper signal " << k << " frame " << f << " mel " << j);
        CHECK(std::fabs(w[j * 3000 + f] - num(vals.a[j])) < 1e-3);
      }
    }
    const double wmean = std::accumulate(w.begin(), w.end(), 0.0) / static_cast<double>(w.size());
    CHECK(std::fabs(wmean - num(*wr->find("mean"))) < 1e-4);
  }
}

TEST_CASE("speech words ignore case and punctuation", "[ai][audio]") {
  const auto w = mv::infer::speech_words("Happy Birthday, Anna! We're landing in Lisbon.");
  const std::vector<std::string> want{"happy", "birthday", "anna", "were", "landing", "in", "lisbon"};
  CHECK(w == want);
}

TEST_CASE("the CLAP tokenizer and towers match the reference", "[ai][audio][pack]") {
  const std::string core = env("MV_AI_PACK_DIR");
  const std::string audio = env("MV_AI_AUDIO_DIR");
  if (core.empty() || audio.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_AUDIO_DIR not set");
  const auto ref = reference();
  auto spec = mv::infer::read_clap_spec(utf8(fs::path(audio) / "models/clap-general"));
  REQUIRE(spec);
  auto tok = mv::infer::gpt2_tokenizer::load(read_text(spec->vocab_file), read_text(spec->merges_file));
  REQUIRE(tok);
  for (const auto& [text, ids] : ref.find("tokens")->o) {
    std::vector<std::int64_t> want;
    for (const auto& x : ids.a) want.push_back(x.i);
    INFO(text);
    CHECK(tok->encode(text, 0, 2) == want);
  }
  auto rt = mv::infer::runtime::load(core);
  REQUIRE(rt);
  auto model = mv::infer::clap_model::open(**rt, *spec, mv::infer::session_options{});
  REQUIRE(model);
  for (int k = 0; k < 2; ++k) {
    const auto x = signal(k, 48000);
    const std::span<const float> win(x);
    std::vector<float> e;
    REQUIRE((*model)->embed_audio(std::span<const std::span<const float>>(&win, 1), e));
    std::vector<float> want;
    for (const auto& v : ref.find("clap_audio")->a[static_cast<std::size_t>(k)].a) want.push_back(static_cast<float>(num(v)));
    const float c = mv::infer::dot(e, want);
    INFO("CLAP audio signal " << k << " cosine " << c);
    CHECK(c > 0.995f);
  }
  const mv::json::value* queries = ref.find("queries");
  for (std::size_t q = 0; q < queries->a.size(); ++q) {
    auto e = (*model)->embed_text(queries->a[q].s);
    REQUIRE(e);
    std::vector<float> want;
    for (const auto& v : ref.find("clap_text")->a[q].a) want.push_back(static_cast<float>(num(v)));
    const float c = mv::infer::dot(*e, want);
    INFO("CLAP text " << queries->a[q].s << " cosine " << c);
    CHECK(c > 0.999f);
  }
}

TEST_CASE("Whisper finds the words said in a clip, at the right times", "[ai][audio][pack][speech]") {
  const std::string core = env("MV_AI_PACK_DIR");
  const std::string audio = env("MV_AI_AUDIO_DIR");
  const std::string clip = env("MV_AI_SPEECH_CLIP");
  if (core.empty() || audio.empty() || clip.empty()) SKIP("MV_AI_PACK_DIR, MV_AI_AUDIO_DIR, MV_AI_SPEECH_CLIP not set");
#if !defined(MV_AI_TEST_DECODE)
  SKIP("a build without the host decoders");
#else
  auto rt = mv::infer::runtime::load(env("MV_AI_CUDA_DIR").empty() ? core : env("MV_AI_CUDA_DIR"));
  REQUIRE(rt);
  for (const char* folder : {"whisper-base", "whisper-small"}) {
    auto spec = mv::infer::read_whisper_spec(utf8(fs::path(audio) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.threads = 4;
    if (!env("MV_AI_CUDA_DIR").empty()) o.on = mv::infer::backend::cuda;
    auto model = mv::infer::whisper_model::open(**rt, *spec, o);
    REQUIRE(model);
    // The host's own soundtrack reader, as the indexer uses it.
    auto pcm = mv::addon::media::open_audio(clip, 16000, 0);
    REQUIRE(pcm);
    std::vector<float> all;
    std::int64_t t = 0;
    while (true) {
      auto chunk = (*pcm)->read(16000 * 10, t);
      REQUIRE(chunk);
      if (chunk->empty()) break;
      all.insert(all.end(), chunk->begin(), chunk->end());
    }
    std::vector<mv::infer::speech_segment> segs;
    std::size_t at = 0;
    while (at < all.size()) {
      const std::size_t n = std::min<std::size_t>(480000, all.size() - at);
      auto w = (*model)->transcribe(std::span<const float>(all.data() + at, n), static_cast<std::int64_t>(at / 16));
      if (!w) FAIL(folder << " transcribe failed: " << mv::status_name(w.error()) << " " << mv::infer::session::last_error());
      segs.insert(segs.end(), w->segments.begin(), w->segments.end());
      at += static_cast<std::size_t>(std::max<std::int64_t>(w->consumed_ms, 1000)) * 16;
    }
    // The segment holding a word starts between the line's start and the
    // word itself (a segment may hold several sentences).
    const auto find = [&](const char* word, std::int64_t from_ms, std::int64_t to_ms) {
      for (const auto& s : segs) {
        for (const auto& w : mv::infer::speech_words(s.text)) {
          if (w == word) {
            INFO(folder << " \"" << word << "\" in a segment at " << s.start_ms << " ms (expected "
                        << from_ms << "-" << to_ms << ")");
            CHECK(s.start_ms >= from_ms);
            CHECK(s.start_ms <= to_ms);
            return true;
          }
        }
      }
      return false;
    };
    std::string all_text;
    for (const auto& s : segs) all_text += "[" + std::to_string(s.start_ms) + "] " + s.text + " ";
    INFO(folder << ": " << all_text);
    // The clip: "Happy birthday Anna..." at 5 s, "...landing in Lisbon..." at
    // 15 s, "Come here Max, good boy. Fetch the ball." at 28 s ("fetch" ~31 s).
    CHECK(find("birthday", 4000, 6500));
    CHECK(find("lisbon", 14000, 16500));
    CHECK(find("fetch", 27500, 31500));
  }
#endif
}

// Mac (MAC-VALIDATION §12b): CLAP does not compile on Core ML and Whisper
// aborts the process inside MPSGraph (ORT 1.30, M5), so the pack keeps audio
// on CPU (pack.cpp) and there is no Core ML agreement case to run here.

#if defined(MV_AI_TEST_DECODE)
namespace {
std::vector<float> clip_pcm(const std::string& clip) {
  auto pcm = mv::addon::media::open_audio(clip, 16000, 0);
  REQUIRE(pcm);
  std::vector<float> all;
  std::int64_t t = 0;
  while (true) {
    auto chunk = (*pcm)->read(16000 * 10, t);
    REQUIRE(chunk);
    if (chunk->empty()) break;
    all.insert(all.end(), chunk->begin(), chunk->end());
  }
  return all;
}

std::string transcribe_all(mv::infer::whisper_model& model, const std::vector<float>& all) {
  std::string text;
  std::size_t at = 0;
  while (at < all.size()) {
    const std::size_t n = std::min<std::size_t>(480000, all.size() - at);
    auto w = model.transcribe(std::span<const float>(all.data() + at, n), static_cast<std::int64_t>(at / 16));
    REQUIRE(w);
    for (const auto& s : w->segments) text += " " + s.text;
    at += static_cast<std::size_t>(std::max<std::int64_t>(w->consumed_ms, 1000)) * 16;
  }
  return text;
}
}  // namespace
#endif

// Timings for docs/design/17 (hidden): CLAP 10 s windows/s and Whisper x real time,
// on CPU (the only provider the Mac gives audio).
TEST_CASE("bench: CLAP and Whisper on CPU", "[.bench][ai][audio]") {
  const std::string core = env("MV_AI_PACK_DIR");
  const std::string audio = env("MV_AI_AUDIO_DIR");
  const std::string clip = env("MV_AI_SPEECH_CLIP");
  if (core.empty() || audio.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_AUDIO_DIR not set");
  auto rt = mv::infer::runtime::load(core);
  REQUIRE(rt);
  using clk = std::chrono::steady_clock;
  auto spec = mv::infer::read_clap_spec(utf8(fs::path(audio) / "models/clap-general"));
  REQUIRE(spec);
  const auto x = signal(1, 48000);  // 12 s: one 10 s window
  const std::span<const float> win(x.data(), 480000);
  std::vector<std::span<const float>> wins(4, win);
  for (auto on : {mv::infer::backend::cpu}) {
    mv::infer::session_options o;
    o.on = on;
    o.threads = 2;
    auto model = mv::infer::clap_model::open(**rt, *spec, o);
    REQUIRE(model);
    std::vector<float> e;
    REQUIRE((*model)->embed_audio(wins, e));
    const auto t0 = clk::now();
    for (int i = 0; i < 5; ++i) REQUIRE((*model)->embed_audio(wins, e));
    const double s = std::chrono::duration<double>(clk::now() - t0).count();
    (void)(*model)->embed_text("warm");
    const auto q0 = clk::now();
    for (int i = 0; i < 20; ++i) REQUIRE((*model)->embed_text("dog barking"));
    const double q_ms = std::chrono::duration<double, std::milli>(clk::now() - q0).count() / 20;
    WARN("CLAP " << mv::infer::backend_name(on) << ": " << 20 / s << " windows/s, " << q_ms << " ms/query");
  }
#if defined(MV_AI_TEST_DECODE)
  if (clip.empty()) return;
  const auto all = clip_pcm(clip);
  const double clip_s = static_cast<double>(all.size()) / 16000.0;
  for (const char* folder : {"whisper-base", "whisper-small"}) {
    auto ws = mv::infer::read_whisper_spec(utf8(fs::path(audio) / "models" / folder));
    REQUIRE(ws);
    for (auto on : {mv::infer::backend::cpu}) {
      mv::infer::session_options o;
      o.on = on;
      o.threads = 2;
      const auto t_open = clk::now();
      auto model = mv::infer::whisper_model::open(**rt, *ws, o);
      REQUIRE(model);
      const double open_ms = std::chrono::duration<double, std::milli>(clk::now() - t_open).count();
      (void)transcribe_all(**model, all);  // warm
      const auto t0 = clk::now();
      (void)transcribe_all(**model, all);
      const double s = std::chrono::duration<double>(clk::now() - t0).count();
      WARN(folder << " " << mv::infer::backend_name(on) << ": " << clip_s / s << "x real time, open " << open_ms << " ms");
    }
  }
#endif
}
