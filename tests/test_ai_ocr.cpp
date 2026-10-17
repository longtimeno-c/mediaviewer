// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #403: the operating system's text reader (src/addons/ai/text_reader.h) over a
// labelled set of photos with words in them, with the engine's matching rule:
// how many phrases a quoted search would find, and what reading costs a photo
// beside the picture pass's own decode.
//
//   python3 tools/ai/ocr-fixture.py <dir>
//   MV_OCR_FIXTURE_DIR=<dir> mv_ai_tests "[.ocr-fixture]"
//
// Hidden: it needs the fixture and the OS recogniser (Vision on the Mac,
// Windows.Media.Ocr with an OCR language installed on Windows).
#include "catch_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#if defined(MV_AI_TEST_DECODE)
#include "abi/addon_media.h"
#endif
#include "addons/ai/query.h"
#include "addons/ai/text_reader.h"
#include "core/fold.h"
#include "infer/audio_models.h"

namespace {

namespace fs = std::filesystem;

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The engine's rule for a file's own text (engine::cue_words).
std::vector<std::string> words(const std::string& s) { return mv::infer::speech_words(mv::text::fold(s)); }

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

TEST_CASE("the OS text reader finds the labelled phrases (#403)", "[.ocr-fixture]") {
#if !defined(MV_AI_TEST_DECODE)
  SKIP("built without the host's decoders");
#else
  const char* dir_env = std::getenv("MV_OCR_FIXTURE_DIR");
  if (!dir_env) SKIP("MV_OCR_FIXTURE_DIR not set (python3 tools/ai/ocr-fixture.py <dir>)");
  const fs::path dir(dir_env);
  auto reader = mv::ai::make_text_reader();
  if (!reader) SKIP("no OS text reader here");
  std::ifstream labels(dir / "labels.tsv");
  REQUIRE(labels);
  std::string line;
  int total = 0, found = 0;
  std::vector<double> decode_small, decode_large, ocr;
  std::vector<std::string> missed;
  while (std::getline(labels, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const std::string file = (dir / line.substr(0, tab)).string();
    const std::string phrase = line.substr(tab + 1);
    ++total;
    // The picture pass's decode, for scale.
    auto t0 = std::chrono::steady_clock::now();
    auto small = mv::addon::media::decode_still(file, 448);
    decode_small.push_back(ms_since(t0));
    REQUIRE(small);
    // The text pass: a 2,048 px decode and the OS reader (engine::process_text).
    t0 = std::chrono::steady_clock::now();
    auto big = mv::addon::media::decode_still(file, 2048);
    decode_large.push_back(ms_since(t0));
    REQUIRE(big);
    mv::ai::rgb_frame f;
    f.width = big->width;
    f.height = big->height;
    f.rgb = std::move(big->rgb);
    t0 = std::chrono::steady_clock::now();
    auto lines = reader->read(f);
    ocr.push_back(ms_since(t0));
    REQUIRE(lines);
    std::string joined;
    for (const std::string& l : *lines) joined += (joined.empty() ? "" : " ") + l;
    // A quoted search: the phrase's words, in order, in the photo's words.
    if (mv::ai::query::contains_phrase(words(joined), words(phrase), false)) {
      ++found;
    } else {
      missed.push_back(line.substr(0, tab) + " \"" + phrase + "\" read as \"" + joined + "\"");
    }
  }
  REQUIRE(total > 0);
  const double recall = static_cast<double>(found) / total;
  for (const std::string& m : missed) std::printf("missed: %s\n", m.c_str());
  std::printf(
      "{\"reader\":\"%s\",\"photos\":%d,\"found\":%d,\"recall\":%.3f,"
      "\"decode_448_ms_median\":%.1f,\"decode_2048_ms_median\":%.1f,\"read_ms_median\":%.1f,\"read_ms_mean\":%.1f}\n",
      reader->spec_key().c_str(), total, found, recall, median(decode_small), median(decode_large), median(ocr),
      std::accumulate(ocr.begin(), ocr.end(), 0.0) / static_cast<double>(ocr.size()));
  CHECK(recall >= 0.9);
#endif
}

// The text on clip moments (engine::process_text_moments): what one moment
// costs on top of the picture pass. Every clip in the folder, twelve moments
// spread over it (the picture pass keeps a moment every 0.5-2 s on a change;
// the cost per moment is what matters), each decoded at the text edge (2,048,
// the clip's own size when smaller) by the host's seek-and-decode, then read.
// The 448 px seek of the same moment is printed for scale.
//
//   MV_OCR_CLIP_DIR=<folder of clips> mv_ai_tests "[.ocr-moments]"
TEST_CASE("the OS text reader's cost per clip moment (#403)", "[.ocr-moments]") {
#if !defined(MV_AI_TEST_DECODE)
  SKIP("built without the host's decoders");
#else
  const char* dir_env = std::getenv("MV_OCR_CLIP_DIR");
  if (!dir_env) SKIP("MV_OCR_CLIP_DIR not set");
  auto reader = mv::ai::make_text_reader();
  if (!reader) SKIP("no OS text reader here");
  constexpr int kMoments = 12;
  std::vector<double> seek_small, seek_large, ocr, total;
  int clips = 0, with_text = 0, moments = 0;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(fs::path(dir_env))) {
    if (e.is_regular_file()) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    const std::string file = p.string();
    mv::addon::video_facts facts;
    {
      auto sampler = mv::addon::media::open_sampler(file, mv::addon::sampler_options{});
      if (!sampler) continue;  // not a clip
      facts = (*sampler)->facts();
    }
    if (facts.duration_ms <= 0) continue;
    ++clips;
    for (int i = 0; i < kMoments; ++i) {
      const std::int64_t ms = facts.duration_ms * (2 * i + 1) / (2 * kMoments);
      auto t0 = std::chrono::steady_clock::now();
      auto small = mv::addon::media::video_frame(file, ms, 448);
      seek_small.push_back(ms_since(t0));
      if (!small) continue;
      t0 = std::chrono::steady_clock::now();
      auto big = mv::addon::media::video_frame(file, ms, 2048);
      const double decode = ms_since(t0);
      seek_large.push_back(decode);
      REQUIRE(big);
      mv::ai::rgb_frame f;
      f.width = big->width;
      f.height = big->height;
      f.rgb = std::move(big->rgb);
      t0 = std::chrono::steady_clock::now();
      auto lines = reader->read(f);
      const double read = ms_since(t0);
      ocr.push_back(read);
      total.push_back(decode + read);
      REQUIRE(lines);
      ++moments;
      if (!lines->empty()) ++with_text;
    }
    std::printf("clip %d: %ux%u, %.1f s\n", clips, facts.width, facts.height,
                static_cast<double>(facts.duration_ms) / 1000.0);
  }
  if (moments == 0) SKIP("no clips decoded in MV_OCR_CLIP_DIR");
  std::printf(
      "{\"reader\":\"%s\",\"clips\":%d,\"moments\":%d,\"with_text\":%d,\"seek_448_ms_median\":%.1f,"
      "\"seek_2048_ms_median\":%.1f,\"read_ms_median\":%.1f,\"moment_ms_median\":%.1f,\"moment_ms_mean\":%.1f}\n",
      reader->spec_key().c_str(), clips, moments, with_text, median(seek_small), median(seek_large), median(ocr),
      median(total), std::accumulate(total.begin(), total.end(), 0.0) / static_cast<double>(total.size()));
#endif
}

// #354 D4: a scanned PDF's page as the scan pass reads it: drawn by the host
// (video_frame with pts_ms the page, the viewer's own renderer) at the text
// edge, then read by the OS. Prints the draw and the read apart, per page, and
// with MV_OCR_SCAN_LINES=1 each page's lines (the verify set's text, never a
// user's: the folder is tools/ai-bench/doc-scans' output). MV_OCR_SCAN_EDGE
// draws at another edge (the pass's is 2,048); MV_OCR_SCAN_TRUTH=<out>/truth.tsv
// scores each page's words read against the words drawn (the engine's rule).
//
//   swift tools/ai-bench/doc-scans/make_doc_scans.swift <out>
//   MV_OCR_SCAN_DIR=<out>/scans MV_OCR_SCAN_TRUTH=<out>/truth.tsv mv_ai_tests "[.ocr-scans]"
TEST_CASE("the OS text reader's cost per scanned page (#354 D4)", "[.ocr-scans]") {
#if !defined(MV_AI_TEST_DECODE)
  SKIP("built without the host's decoders");
#else
  const char* dir_env = std::getenv("MV_OCR_SCAN_DIR");
  if (!dir_env) SKIP("MV_OCR_SCAN_DIR not set");
  const bool show = std::getenv("MV_OCR_SCAN_LINES") != nullptr;
  const std::uint32_t edge =
      std::getenv("MV_OCR_SCAN_EDGE") ? static_cast<std::uint32_t>(std::atoi(std::getenv("MV_OCR_SCAN_EDGE"))) : 2048u;
  // file#page -> the words drawn there.
  std::map<std::string, std::vector<std::string>> truth;
  if (const char* t = std::getenv("MV_OCR_SCAN_TRUTH")) {
    std::ifstream in(t);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
      const auto a = line.find('\t'), b = line.find('\t', a + 1);
      if (a == std::string::npos || b == std::string::npos) continue;
      truth[line.substr(0, a) + "#" + line.substr(a + 1, b - a - 1)] = words(line.substr(b + 1));
    }
  }
  std::vector<double> recall;
  auto reader = mv::ai::make_text_reader();
  if (!reader) SKIP("no OS text reader here");
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(fs::path(dir_env))) {
    if (e.is_regular_file() && e.path().extension() == ".pdf") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  std::vector<double> draw, read, lines_n;
  for (const fs::path& p : files) {
    for (std::int64_t page = 0;; ++page) {
      auto t0 = std::chrono::steady_clock::now();
      auto img = mv::addon::media::video_frame(p.string(), page, edge);
      const double d = ms_since(t0);
      if (!img) break;  // past the last page
      draw.push_back(d);
      mv::ai::rgb_frame f;
      f.width = img->width;
      f.height = img->height;
      f.rgb = std::move(img->rgb);
      t0 = std::chrono::steady_clock::now();
      auto lines = reader->read(f);
      read.push_back(ms_since(t0));
      REQUIRE(lines);
      lines_n.push_back(static_cast<double>(lines->size()));
      if (auto it = truth.find(p.filename().string() + "#" + std::to_string(page)); it != truth.end()) {
        std::string all;
        for (const std::string& l : *lines) all += l + " ";
        const std::vector<std::string> got = words(all);
        const std::set<std::string> have(got.begin(), got.end());
        const std::set<std::string> want(it->second.begin(), it->second.end());
        std::size_t hit = 0;
        for (const std::string& w : want) hit += have.count(w);
        recall.push_back(want.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(want.size()));
      }
      if (show) {
        std::printf("%s p%lld %ux%u: %zu lines\n", p.filename().string().c_str(), static_cast<long long>(page),
                    f.width, f.height, lines->size());
        for (const std::string& l : *lines) std::printf("  | %s\n", l.c_str());
      }
    }
  }
  if (draw.empty()) SKIP("no PDF page drew in MV_OCR_SCAN_DIR");
  const double recall_mean =
      recall.empty() ? 0 : std::accumulate(recall.begin(), recall.end(), 0.0) / static_cast<double>(recall.size());
  std::printf("{\"reader\":\"%s\",\"edge\":%u,\"pages\":%zu,\"draw_ms_median\":%.1f,\"read_ms_median\":%.1f,"
              "\"read_ms_mean\":%.1f,\"lines_median\":%.0f,\"recall_mean\":%.3f,\"recall_min\":%.3f,"
              "\"pages_at_90\":%zu}\n",
              reader->spec_key().c_str(), edge, draw.size(), median(draw), median(read),
              std::accumulate(read.begin(), read.end(), 0.0) / static_cast<double>(read.size()), median(lines_n),
              recall_mean, recall.empty() ? 0 : *std::min_element(recall.begin(), recall.end()),
              static_cast<std::size_t>(std::count_if(recall.begin(), recall.end(), [](double x) { return x >= 0.9; })));
#endif
}
