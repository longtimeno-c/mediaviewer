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
#include <numeric>
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
