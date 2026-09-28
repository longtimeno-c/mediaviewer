// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// src/infer and the pack's models: plan/17 PR 20's verify lines that need
// weights, plus the pure parts that do not.
//
//   always         tokenizer rules, half floats, int8 dot, YuNet decode, the
//                  store's int8 matrix
//   MV_AI_PACK_DIR a staged Core pack (tools/package/ai-models.py + the ORT
//                  runtime beside it): the tokenizer against the checkpoint's
//                  own ids, and embeddings within tolerance of the ORT-Python
//                  reference on CPU (tests/data/ai/reference.json)
//   MV_AI_CUDA_DIR the ai-cuda piece: the same tolerance on CUDA
//   MV_AI_EVAL_DIR a labelled folder (images + labels.json, kept out of git
//                  like the RAW corpus): a description ranks the right photos
//                  first, including "guy on a skateboard"; nonsense finds nothing
//   MV_AI_FACES_DIR the ai-faces piece: faces are found in the labelled photos
#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "addons/ai/index_db.h"
#include "addons/ai/query.h"
#if defined(MV_AI_TEST_DECODE)
#include "abi/addon_media.h"
#endif
#include "addons/ai/vectors.h"
#include "addons/ai/vocabulary.h"
#include "core/json.h"
#include "infer/clip_tokenizer.h"
#include "infer/models.h"
#include "infer/ort.h"
#include "infer/preprocess.h"

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

// The three test cards tools/ai-reference/make_reference.py draws.
std::vector<std::uint8_t> card(int kind, std::uint32_t& w, std::uint32_t& h) {
  std::vector<std::uint8_t> px;
  if (kind == 0) {
    w = 320;
    h = 240;
  } else if (kind == 1) {
    w = 200;
    h = 300;
  } else {
    w = 640;
    h = 480;
  }
  px.resize(static_cast<std::size_t>(w) * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
      if (kind == 0) {
        p[0] = static_cast<std::uint8_t>((x * 7 + y * 3) & 255);
        p[1] = static_cast<std::uint8_t>((x * y) & 255);
        p[2] = static_cast<std::uint8_t>((x ^ y) & 255);
      } else if (kind == 1) {
        const int dx = static_cast<int>(x) - 100, dy = static_cast<int>(y) - 150;
        p[0] = static_cast<std::uint8_t>((x * 255) / (w - 1));
        p[1] = static_cast<std::uint8_t>((y * 255) / (h - 1));
        p[2] = dx * dx + dy * dy < 60 * 60 ? 255 : 30;
      } else {
        const std::uint8_t grey = static_cast<std::uint8_t>((((x / 40) + (y / 40)) % 2) * 200 + 20);
        const bool stripe = y >= 200 && y < 260;
        p[0] = stripe ? 220 : grey;
        p[1] = stripe ? 30 : grey;
        p[2] = stripe ? 30 : grey;
      }
    }
  }
  return px;
}

std::vector<float> floats(const mv::json::value& a) {
  std::vector<float> v;
  for (const auto& x : a.a) v.push_back(static_cast<float>(x.is_integer ? static_cast<double>(x.i) : x.d));
  return v;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
  return mv::infer::dot(a, b);
}

}  // namespace

TEST_CASE("the tokenizer normalises and splits like CLIP's", "[ai][infer]") {
  using mv::infer::clip_tokenizer;
  CHECK(clip_tokenizer::normalise("  Guy   On\tA  Skateboard! ") == "guy on a skateboard!");
  CHECK(clip_tokenizer::normalise("CAFÉ") == "café");
  const auto p = clip_tokenizer::pieces("a dog's frisbee, 42!!");
  const std::vector<std::string> want{"a", "dog", "'s", "frisbee", ",", "4", "2", "!!"};
  CHECK(p == want);
  CHECK(clip_tokenizer::pieces("they're") == std::vector<std::string>{"they", "'re"});
}

TEST_CASE("half floats and int8 vectors round-trip within their precision", "[ai][infer]") {
  for (float f : {0.0f, 1.0f, -2.5f, 0.1f, 65504.0f, 1e-5f, -0.333f}) {
    const float back = mv::infer::from_half(mv::infer::to_half(f));
    CHECK(std::fabs(back - f) <= std::fabs(f) * 1e-3f + 1e-7f);
  }
  std::vector<float> v{0.1f, -0.7f, 0.3f, 0.6f};
  mv::infer::l2_normalise(v);
  std::vector<std::int8_t> q;
  float scale = 0;
  mv::ai::quantise(v, q, scale);
  const std::int32_t self = mv::ai::dot_i8(q.data(), q.data(), q.size());
  CHECK(std::fabs(self * scale * scale - 1.0f) < 0.02f);

  mv::ai::vector_store store(4);
  store.add(7, -1, 0.1f, scale, q);
  auto hits = store.scan(v, {}, 10, true, 0.5f, -1);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].asset == 7);
  CHECK(std::fabs(hits[0].score - 1.0f) < 0.02f);
  store.remove_asset(7);
  CHECK(store.scan(v, {}, 10, false, 0, -1).empty());
}

TEST_CASE("a query scans 100 k ViT-L frames in under 100 ms", "[ai][infer][vectors][perf]") {
  // plan/17 PR 22: "< 100 ms over 100 k frames". 2,000 clips of 50 frames,
  // pseudo-random unit vectors; the scan as search_text runs it (margin
  // filter, top 5000, the per-asset stats).
  constexpr std::uint32_t dim = 768;
  mv::ai::vector_store store(dim);
  std::uint32_t seed = 12345;
  const auto next = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(seed >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
  };
  std::vector<float> v(dim);
  std::vector<std::int8_t> q;
  for (std::int64_t asset = 1; asset <= 2000; ++asset) {
    for (std::int64_t f = 0; f < 50; ++f) {
      for (float& x : v) x = next();
      mv::infer::l2_normalise(v);
      float scale = 0;
      mv::ai::quantise(v, q, scale);
      store.add(asset, f * 2000, 0.1f, scale, q);
    }
  }
  REQUIRE(store.rows() == 100000);
  for (float& x : v) x = next();
  mv::infer::l2_normalise(v);
  mv::ai::vector_store::scan_stats st;
  double best_ms = 1e9;
  for (int run = 0; run < 3; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto hits = store.scan(v, {}, 5000, true, 0.015f, -1.0f, &st);
    best_ms = std::min(best_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    CHECK(st.assets == 2000);
    (void)hits;
  }
  WARN("100 k x 768 int8 scan: " << best_ms << " ms");
#if defined(NDEBUG)
  CHECK(best_ms < 100.0);  // optimised builds only: a Debug scan proves nothing
  // The label bars (issue #85): filling them for every row is the one-off
  // cost at load, and the filter is one compare a row in the scan. Optimised
  // builds only: the fill is 100 k x 78 x 768 int8 dots, 218 s under ASan in
  // Debug (it timed out CI's 15-minute test step), and the bar's semantics
  // are the next test's. The timings are what this one is for.
  std::vector<float> vocab;
  for (std::size_t l = 0; l < mv::ai::labels().size(); ++l) {
    for (float& x : v) x = next();
    mv::infer::l2_normalise(v);
    vocab.insert(vocab.end(), v.begin(), v.end());
  }
  const auto t_fill = std::chrono::steady_clock::now();
  store.set_labels(vocab, mv::ai::labels().size());
  const double fill_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_fill).count();
  double labelled_ms = 1e9;
  for (int run = 0; run < 3; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto hits = store.scan(v, {}, 5000, true, 0.015f, -1.0f, &st, true);
    labelled_ms = std::min(labelled_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    (void)hits;
  }
  WARN("label bars for 100 k rows: " << fill_ms << " ms; the scan with them: " << labelled_ms << " ms");
  CHECK(labelled_ms < 100.0);
#endif
}

TEST_CASE("the label bar: a row is a result only where the query beats its labels", "[ai][infer][vectors]") {
  // Ten labels on the first ten axes, two more beside them: a row on the
  // diagonal of the ten scores 0.32 against each, so its bar (the ninth best)
  // is 0.32. The row itself beats that; a query on an axis no label or row
  // shares does not.
  constexpr std::uint32_t dim = 16;
  const auto axis = [](std::uint32_t i) {
    std::vector<float> v(dim, 0.0f);
    v[i] = 1.0f;
    return v;
  };
  std::vector<float> row(dim, 0.0f);
  for (std::uint32_t i = 0; i < 10; ++i) row[i] = 1.0f;
  mv::infer::l2_normalise(row);
  std::vector<float> vocab;
  for (std::uint32_t i = 0; i < 12; ++i) {
    const auto a = axis(i);
    vocab.insert(vocab.end(), a.begin(), a.end());
  }
  std::vector<float> off = axis(14);
  off[0] = 0.3f;
  mv::infer::l2_normalise(off);
  const auto add_row = [&](mv::ai::vector_store& s) {
    std::vector<std::int8_t> q;
    float scale = 0;
    mv::ai::quantise(row, q, scale);
    s.add(1, -1, 0.0f, scale, q);
  };
  const auto found = [&](const mv::ai::vector_store& s, const std::vector<float>& query) {
    return s.scan(query, {}, 10, false, 0, -1.0f, nullptr, true).size();
  };
  mv::ai::vector_store store(dim);
  add_row(store);
  CHECK(found(store, off) == 1);  // no labels yet: every row passes
  store.set_labels(vocab, 12);
  CHECK(store.has_labels());
  CHECK(found(store, row) == 1);
  CHECK(found(store, off) == 0);
  CHECK(store.scan(off, {}, 10, false, 0, -1.0f).size() == 1);  // only with use_labels
  // A row added later gets its bar as it comes; clearing keeps the labels.
  store.clear();
  add_row(store);
  CHECK(found(store, off) == 0);
  // A new tower (reset) drops them until it has its own.
  store.reset(dim);
  add_row(store);
  CHECK_FALSE(store.has_labels());
  CHECK(found(store, off) == 1);
}

TEST_CASE("scan stats: the top ten stand out per asset, not per frame", "[ai][infer][vectors]") {
  const std::vector<float> query{1.0f, 0.0f};
  const auto add = [](mv::ai::vector_store& s, std::int64_t asset, std::int64_t pts, float x, float y) {
    std::vector<float> v{x, y};
    mv::infer::l2_normalise(v);
    std::vector<std::int8_t> q;
    float scale = 0;
    mv::ai::quantise(v, q, scale);
    s.add(asset, pts, 0.0f, scale, q);
  };
  mv::ai::vector_store::scan_stats st;
  {
    // Ten matching assets among two hundred stand out (z = sqrt(190 / 10));
    // a quarter of the index matching could not (z <= sqrt(3)): the margin
    // test is what passes a query that describes much of the library.
    mv::ai::vector_store s(2);
    for (std::int64_t a = 1; a <= 200; ++a) add(s, a, -1, a <= 10 ? 0.9f : 0.1f, a <= 10 ? 0.1f : 0.9f);
    (void)s.scan(query, {}, 100, false, 0, -1, &st);
    CHECK(st.assets == 200);
    CHECK(st.top10_z > 4.0f);
  }
  {
    // One long clip of matching frames among flat stills counts once.
    mv::ai::vector_store s(2);
    for (std::int64_t f = 0; f < 500; ++f) add(s, 100, f * 1000, 0.9f, 0.1f);
    for (std::int64_t a = 1; a <= 39; ++a) add(s, a, -1, 0.1f, 0.9f);
    (void)s.scan(query, {}, 100, false, 0, -1, &st);
    CHECK(st.assets == 40);
    CHECK(st.top10_z < 1.0f);
  }
  {
    // Too few assets to say anything.
    mv::ai::vector_store s(2);
    for (std::int64_t a = 1; a <= 12; ++a) add(s, a, -1, a <= 3 ? 0.9f : 0.1f, 0.5f);
    (void)s.scan(query, {}, 100, false, 0, -1, &st);
    CHECK(st.assets == 12);
    CHECK(st.top10_z == 0.0f);
  }
}

namespace {

// The picture search as engine::search_text ran it before the Precision
// setting (18f7998), kept to prove level 2 unchanged: the scan, the z rows,
// then group()'s "nothing found" gate. `person` is a name in the query (no
// gate). One deliberate change since (2026-09-28): a query whose rows clear
// the margin on several assets is believable at the calibrated margin even
// when its z is below noise's (scan_stats::over_margin, the owner's
// "mountain"). Another (issue #85): standing out needs the stand-out margin
// as well, and both margins grow with the library.
std::vector<mv::ai::vector_store::hit> search_before_precision(const mv::ai::vector_store& store,
                                                               std::span<const float> v, float result_margin,
                                                               float query_margin, float query_z, float result_z,
                                                               bool person) {
  mv::ai::vector_store::scan_stats stats;
  auto hits = store.scan(v, {}, 5000, true, result_margin, -1.0f, &stats);
  {
    std::set<std::int64_t> over;
    for (const auto& h : hits) {
      if (h.score - h.generic >= query_margin) over.insert(h.asset);
    }
    stats.over_margin = std::min(over.size(), stats.broad_assets());
  }
  const bool stands_out = stats.stands_out(query_z);
  const float margin_needed = stats.margin_needed(query_margin);
  if (stands_out && stats.sd > 0) {
    std::set<std::pair<std::int64_t, std::int64_t>> have;
    for (const auto& h : hits) have.insert({h.asset, h.pts_ms});
    for (const auto& h : store.scan(v, {}, 5000, false, 0, stats.mean + result_z * stats.sd)) {
      if (have.insert({h.asset, h.pts_ms}).second) hits.push_back(h);
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  }
  if (hits.empty()) return hits;
  float best = -1;
  for (std::size_t i = 0; i < hits.size() && i < 10; ++i) best = std::max(best, hits[i].score - hits[i].generic);
  const bool answered = stands_out && best >= stats.stand_out_margin(query_margin);
  if (best < (person ? -1.0f : margin_needed) && !answered) return {};
  return hits;
}

// CLAP's search before the setting: the margin rows and group()'s gate.
std::vector<mv::ai::vector_store::hit> sounds_before_precision(const mv::ai::vector_store& store,
                                                               std::span<const float> v, float result_margin,
                                                               float query_margin) {
  auto hits = store.scan(v, {}, 2000, true, result_margin, -1.0f);
  if (hits.empty()) return hits;
  float best = -1;
  for (std::size_t i = 0; i < hits.size() && i < 10; ++i) best = std::max(best, hits[i].score - hits[i].generic);
  if (best < query_margin) return {};
  return hits;
}

bool same_hits(const std::vector<mv::ai::vector_store::hit>& a, const std::vector<mv::ai::vector_store::hit>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].asset != b[i].asset || a[i].pts_ms != b[i].pts_ms || a[i].score != b[i].score ||
        a[i].generic != b[i].generic) {
      return false;
    }
  }
  return true;
}

}  // namespace

// A clustered fixture (stills and multi-row clips, generic prompts near the
// middle of everything as CLIP's are): Precision 2 returns exactly what the
// search returned before the setting existed, row for row, score for score,
// and each stricter level a subset of the looser one.
TEST_CASE("precision 2 is the calibrated rule, bit for bit; the levels nest", "[ai][infer][vectors]") {
  constexpr std::uint32_t dim = 32;
  std::mt19937 rng(20260927);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  const auto unit = [&](std::vector<float> v) {
    mv::infer::l2_normalise(v);
    return v;
  };
  const auto random_unit = [&] {
    std::vector<float> v(dim);
    for (float& x : v) x = normal(rng);
    return unit(v);
  };
  // What every image shares (CLIP's cone): the generic prompts sit near it.
  const std::vector<float> common = random_unit();
  std::vector<std::vector<float>> centres;
  for (int c = 0; c < 12; ++c) centres.push_back(random_unit());
  const auto around = [&](const std::vector<float>& centre, float spread) {
    std::vector<float> v(dim);
    for (std::uint32_t i = 0; i < dim; ++i) v[i] = 0.9f * common[i] + centre[i] * 0.5f + spread * normal(rng) / 6.0f;
    return unit(v);
  };
  std::vector<std::vector<float>> generic;
  for (int g = 0; g < 3; ++g) generic.push_back(around(random_unit(), 0.3f));
  for (const std::size_t assets : {std::size_t{25}, std::size_t{400}}) {
    mv::ai::vector_store store(dim);
    std::int64_t id = 0;
    for (std::size_t a = 0; a < assets; ++a) {
      ++id;
      // One in ten is a clip of four moments; a quarter of the assets hold
      // one of the first three subjects, the rest spread over all twelve.
      const int rows = a % 10 == 0 ? 4 : 1;
      const auto& centre = centres[a % 4 == 0 ? a % 3 : a % centres.size()];
      for (int r = 0; r < rows; ++r) {
        const auto v = around(centre, 1.0f + static_cast<float>(r));
        float g = -1;
        for (const auto& p : generic) g = std::max(g, mv::infer::dot(v, p));
        std::vector<std::int8_t> q;
        float scale = 0;
        mv::ai::quantise(v, q, scale);
        store.add(id, rows == 1 ? -1 : r * 2000, g, scale, q);
      }
    }
    std::vector<std::vector<float>> queries;
    for (const auto& c : centres) {
      queries.push_back(around(c, 0.2f));
      queries.push_back(around(c, 1.5f));
      queries.push_back(unit(c));  // a subject far from the common cone
    }
    for (int i = 0; i < 12; ++i) queries.push_back(random_unit());  // off in another direction
    // Nonsense as CLIP embeds it: near the generic prompts, favouring nothing.
    for (const auto& g : generic) queries.push_back(g);
    for (int i = 0; i < 6; ++i) {
      std::vector<float> v(dim);
      for (std::uint32_t d = 0; d < dim; ++d) v[d] = common[d] + normal(rng) / 20.0f;
      queries.push_back(unit(v));
    }
    int answered = 0, empty = 0;
    for (const auto& q : queries) {
      for (const bool person : {false, true}) {
        const mv::ai::text_thresholds t;  // model.json's defaults: 0.04, 0.015, 2.5, 2.0
        const auto before = search_before_precision(store, q, t.result_margin, t.query_margin, t.query_z, t.result_z, person);
        const auto now = mv::ai::find_text(store, q, {}, t, mv::ai::precision_scale::at(mv::ai::kPrecisionDefault), !person);
        CHECK(same_hits(before, now));
        (before.empty() ? empty : answered) += 1;
      }
      mv::ai::text_thresholds sound;
      sound.query_z = 0;
      sound.result_z = 0;
      CHECK(same_hits(sounds_before_precision(store, q, sound.result_margin, sound.query_margin),
                      mv::ai::find_text(store, q, {}, sound, mv::ai::precision_scale::at(2), true, 2000)));
      // The levels nest: every row of a stricter level is a row of the looser.
      std::vector<std::pair<std::int64_t, std::int64_t>> looser;
      for (std::uint32_t level = 0; level < mv::ai::kPrecisionLevels; ++level) {
        std::vector<std::pair<std::int64_t, std::int64_t>> rows;
        for (const auto& h : mv::ai::find_text(store, q, {}, {}, mv::ai::precision_scale::at(level), true)) {
          rows.push_back({h.asset, h.pts_ms});
        }
        std::sort(rows.begin(), rows.end());
        if (level > 0) CHECK(std::includes(looser.begin(), looser.end(), rows.begin(), rows.end()));
        looser = std::move(rows);
      }
    }
    // The fixture reaches both answers.
    INFO(assets << " assets: " << answered << " answered, " << empty << " empty");
    CHECK(answered > 0);
    CHECK(empty > 0);
  }
}

TEST_CASE("YuNet's outputs decode to boxes and landmarks, then NMS", "[ai][infer][faces]") {
  const std::uint32_t side = 640;
  std::vector<mv::infer::tensor_f32> outs(12);
  const int strides[3] = {8, 16, 32};
  for (int i = 0; i < 3; ++i) {
    const std::size_t n = static_cast<std::size_t>(side / strides[i]) * (side / strides[i]);
    outs[i].data.assign(n, 0.0f);
    outs[i + 3].data.assign(n, 0.0f);
    outs[i + 6].data.assign(n * 4, 0.0f);
    outs[i + 9].data.assign(n * 10, 0.0f);
  }
  // One face at stride 16, cell (row 10, col 20); a near duplicate beside it.
  const std::size_t cols = side / 16;
  for (std::size_t idx : {10 * cols + 20, 10 * cols + 21}) {
    outs[1].data[idx] = 0.95f;
    outs[4].data[idx] = 0.95f;
    outs[7].data[idx * 4 + 2] = std::log(4.0f);  // 64 px wide
    outs[7].data[idx * 4 + 3] = std::log(5.0f);  // 80 px tall
  }
  outs[7].data[(10 * cols + 21) * 4] = -1.0f;  // same centre as the first
  const auto faces = mv::infer::decode_yunet(outs, side, 0.8f, 0.3f);
  REQUIRE(faces.size() == 1);
  CHECK(std::fabs(faces[0].w - 64.0f) < 0.01f);
  CHECK(std::fabs(faces[0].h - 80.0f) < 0.01f);
  CHECK(std::fabs(faces[0].x + faces[0].w / 2 - 20 * 16) < 0.01f);
  CHECK(std::fabs(faces[0].score - 0.95f) < 1e-4f);
}

TEST_CASE("the tokenizer and both towers match the reference", "[ai][infer][pack]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  if (pack.empty()) SKIP("MV_AI_PACK_DIR not set (a staged Core pack)");
  const fs::path ref_path = fs::path(MV_AI_TEST_DATA) / "reference.json";
  const auto ref = mv::json::parse(read_text(ref_path), 8);
  REQUIRE(ref);

  const std::string vocab = read_text(fs::path(pack) / "models/clip-tokenizer/vocab.json");
  const std::string merges = read_text(fs::path(pack) / "models/clip-tokenizer/merges.txt");
  auto tok = mv::infer::clip_tokenizer::load(vocab, merges);
  REQUIRE(tok);
  const mv::json::value* tokens = ref->find("tokens");
  REQUIRE(tokens);
  for (const auto& [text, ids] : tokens->o) {
    std::vector<std::int64_t> want;
    for (const auto& x : ids.a) want.push_back(x.i);
    INFO(text);
    CHECK(tok->encode(text) == want);
  }

  auto rt = mv::infer::runtime::load(pack);
  REQUIRE(rt);
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    auto model = mv::infer::clip_model::open(**rt, *spec, mv::infer::session_options{});
    REQUIRE(model);
    const mv::json::value* m = ref->find("models")->find(spec->id);
    REQUIRE(m);
    const mv::json::value* images = m->find("images");
    for (int k = 0; k < 3; ++k) {
      std::uint32_t w = 0, h = 0;
      const auto px = card(k, w, h);
      const mv::infer::rgb_view view{px.data(), w, h};
      std::vector<float> e;
      REQUIRE((*model)->embed_images(std::span<const mv::infer::rgb_view>(&view, 1), e));
      const float c = cosine(e, floats(images->a[static_cast<std::size_t>(k)]));
      INFO(spec->id << " card " << k << " cosine " << c);
      CHECK(c > 0.999f);
    }
    const mv::json::value* texts = m->find("texts");
    const mv::json::value* queries = ref->find("queries");
    for (std::size_t q = 0; q < queries->a.size(); ++q) {
      auto e = (*model)->embed_text(queries->a[q].s);
      REQUIRE(e);
      const float c = cosine(*e, floats(texts->a[q]));
      INFO(spec->id << " query " << queries->a[q].s << " cosine " << c);
      CHECK(c > 0.999f);
    }
  }
}

TEST_CASE("the CUDA provider agrees with the reference", "[ai][infer][pack][cuda]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  const std::string cuda = env("MV_AI_CUDA_DIR");
  if (pack.empty() || cuda.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_CUDA_DIR not set");
  const auto ref = mv::json::parse(read_text(fs::path(MV_AI_TEST_DATA) / "reference.json"), 8);
  REQUIRE(ref);
  auto rt = mv::infer::runtime::load(cuda);
  REQUIRE(rt);
  REQUIRE((*rt)->has_provider(mv::infer::backend::cuda));
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.on = mv::infer::backend::cuda;
    mv::infer::provider_fault fault = mv::infer::provider_fault::none;
    auto model = mv::infer::clip_model::open(**rt, *spec, o, &fault);
    if (!model) SKIP("CUDA did not open: " << mv::infer::fault_name(fault) << " (runtime user-supplied)");
    const mv::json::value* images = ref->find("models")->find(spec->id)->find("images");
    for (int k = 0; k < 3; ++k) {
      std::uint32_t w = 0, h = 0;
      const auto px = card(k, w, h);
      const mv::infer::rgb_view view{px.data(), w, h};
      std::vector<float> e;
      REQUIRE((*model)->embed_images(std::span<const mv::infer::rgb_view>(&view, 1), e));
      const float c = cosine(e, floats(images->a[static_cast<std::size_t>(k)]));
      INFO(spec->id << " CUDA card " << k << " cosine " << c);
      CHECK(c > 0.995f);
    }
  }
}

TEST_CASE("a description ranks the labelled photos, and nonsense finds nothing", "[ai][infer][eval]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  const std::string eval = env("MV_AI_EVAL_DIR");
  if (pack.empty() || eval.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_EVAL_DIR not set");
  // labels.json: [{"file","sentences":[...]}] (COCO-style captions).
  const auto labels = mv::json::parse(read_text(fs::path(eval) / "labels.json"), 8);
  REQUIRE(labels);
  const std::string cuda = env("MV_AI_CUDA_DIR");
  auto rt = mv::infer::runtime::load(cuda.empty() ? pack : cuda);
  REQUIRE(rt);
  struct query_case {
    const char* text;
    std::vector<const char*> any;   // a caption containing one of these...
    std::vector<const char*> also;  // ...and (if given) one of these is relevant
  };
  const std::vector<query_case> cases{
      {"guy on a skateboard", {"skateboard"}, {"man", "guy", "boy", "person", "skateboarder", "male"}},
      {"a plate of pizza", {"pizza"}, {}},
      {"giraffes", {"giraffe"}, {}},
      {"a train at the station", {"train"}, {}},
      {"a dog", {"dog"}, {}},
      {"a cat", {"cat"}, {}},
      {"dog", {"dog"}, {}},
  };
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.threads = 4;
    if (!cuda.empty()) o.on = mv::infer::backend::cuda;
    auto model = mv::infer::clip_model::open(**rt, *spec, o);
    if (!model) {
      o.on = mv::infer::backend::cpu;
      model = mv::infer::clip_model::open(**rt, *spec, o);
    }
    REQUIRE(model);
    // A bounded, fixed subset keeps the CPU run to minutes: 300 photos (where
    // query_z was calibrated) and, given the labels, 1,000 (where a fixed z
    // let every nonsense query through).
    const std::size_t n = std::min<std::size_t>(labels->a.size(), 1000);
    std::vector<std::vector<float>> all_embs;
    std::vector<std::string> all_captions;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& item = labels->a[i];
      const fs::path img = fs::path(eval) / "img" / *item.str("file");
#if defined(MV_AI_TEST_DECODE)
      // The host's own first-pixel decode (media.h), as the indexer gets it.
      auto still = mv::addon::media::decode_still(utf8(img), 448);
      if (!still) continue;
      const std::uint32_t w = still->width, h = still->height;
      const std::vector<std::uint8_t>& px = still->rgb;
#else
      (void)img;
      continue;
      const std::uint32_t w = 0, h = 0;
      const std::vector<std::uint8_t> px;
#endif
      const mv::infer::rgb_view view{px.data(), w, h};
      std::vector<float> e;
      REQUIRE((*model)->embed_images(std::span<const mv::infer::rgb_view>(&view, 1), e));
      all_embs.push_back(e);
      std::string all;
      for (const auto& s : item.find("sentences")->a) all += " " + s.s;
      std::transform(all.begin(), all.end(), all.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
      all_captions.push_back(all);
    }
    if (all_embs.size() < 50) SKIP("fewer than 50 labelled images decoded (a build without the host decoders)");
    for (const std::size_t count : {std::min<std::size_t>(300, all_embs.size()), all_embs.size()}) {
      const std::vector<std::vector<float>> embs(all_embs.begin(), all_embs.begin() + static_cast<std::ptrdiff_t>(count));
      const std::vector<std::string> captions(all_captions.begin(), all_captions.begin() + static_cast<std::ptrdiff_t>(count));
      INFO(count << " photos");
      // The engine's rule (vector_store::scan_stats) on these photos.
      std::vector<std::vector<float>> generic;
      for (const auto& g : spec->generic_prompts) generic.push_back(*(*model)->embed_text(g));
      const auto margin_of = [&](const std::vector<float>& q, std::size_t i) {
        float best_g = -1;
        for (const auto& g : generic) best_g = std::max(best_g, cosine(embs[i], g));
        return cosine(embs[i], q) - best_g;
      };
      const auto accepted = [&](const std::vector<float>& q, float best_margin, float z) {
        mv::ai::vector_store::scan_stats st;
        st.assets = count;
        st.top10_z = z;
        // The images over the query margin (find_text's over_margin, 2026-09-28).
        for (std::size_t i = 0; i < embs.size() && st.over_margin < st.broad_assets(); ++i) {
          if (margin_of(q, i) >= spec->query_margin) ++st.over_margin;
        }
        return (st.stands_out(spec->query_z) && best_margin >= st.stand_out_margin(spec->query_margin)) ||
               best_margin >= st.margin_needed(spec->query_margin);
      };
      // The engine's second test (vector_store::scan_stats): the ten best
      // images' mean score, in standard deviations of all the images' scores.
      const auto top10_z = [&](const std::vector<float>& q) {
        std::vector<float> s(embs.size());
        for (std::size_t i = 0; i < s.size(); ++i) s[i] = cosine(embs[i], q);
        double mean = 0, var = 0;
        for (float v : s) mean += v;
        mean /= static_cast<double>(s.size());
        for (float v : s) var += (v - mean) * (v - mean);
        std::sort(s.begin(), s.end(), std::greater<float>());
        double top = 0;
        for (std::size_t i = 0; i < 10; ++i) top += s[i];
        return static_cast<float>((top / 10 - mean) / std::sqrt(var / static_cast<double>(s.size())));
      };
      for (const query_case& qc : cases) {
        const auto q = *(*model)->embed_text(qc.text);
        std::vector<std::size_t> order(embs.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return cosine(embs[a], q) > cosine(embs[b], q); });
        int relevant = 0;
        std::size_t pool = 0;
        for (std::size_t i = 0; i < captions.size(); ++i) {
          const bool any = std::any_of(qc.any.begin(), qc.any.end(), [&](const char* w) { return captions[i].find(w) != std::string::npos; });
          const bool also = qc.also.empty() || std::any_of(qc.also.begin(), qc.also.end(), [&](const char* w) { return captions[i].find(w) != std::string::npos; });
          pool += any && also ? 1 : 0;
        }
        const std::size_t k = std::min<std::size_t>(5, pool);
        for (std::size_t r = 0; r < k; ++r) {
          const std::string& c = captions[order[r]];
          const bool any = std::any_of(qc.any.begin(), qc.any.end(), [&](const char* w) { return c.find(w) != std::string::npos; });
          const bool also = qc.also.empty() || std::any_of(qc.also.begin(), qc.also.end(), [&](const char* w) { return c.find(w) != std::string::npos; });
          relevant += any && also ? 1 : 0;
        }
        INFO(spec->id << " \"" << qc.text << "\": " << relevant << " of top " << k << " relevant (pool " << pool << ")");
        REQUIRE(k > 0);
        CHECK(relevant >= static_cast<int>(k) - 1);  // plan/17 PR 20 target: P@5 >= 0.8
        float best_margin = -1;  // the engine's rule: the best of the top ten
        for (std::size_t r = 0; r < 10 && r < order.size(); ++r) best_margin = std::max(best_margin, margin_of(q, order[r]));
        const float z = top10_z(q);
        INFO("best margin " << best_margin << ", top-ten z " << z);
        CHECK(accepted(q, best_margin, z));
      }
      for (const char* text : {"xyzzy plugh qwertyuiop", "asdf", "blorf zxqv"}) {
        const auto nonsense = *(*model)->embed_text(text);
        std::vector<std::size_t> by(embs.size());
        for (std::size_t i = 0; i < by.size(); ++i) by[i] = i;
        std::sort(by.begin(), by.end(), [&](std::size_t a, std::size_t b) { return cosine(embs[a], nonsense) > cosine(embs[b], nonsense); });
        float nonsense_margin = -1;
        for (std::size_t r = 0; r < 10 && r < by.size(); ++r) nonsense_margin = std::max(nonsense_margin, margin_of(nonsense, by[r]));
        const float z = top10_z(nonsense);
        INFO(spec->id << " \"" << text << "\" margin " << nonsense_margin << ", top-ten z " << z);
        CHECK_FALSE(accepted(nonsense, nonsense_margin, z));
      }
    }
  }
}

TEST_CASE("what noise scores grows with the index", "[ai][infer][vectors]") {
  mv::ai::vector_store::scan_stats st;
  const auto at = [&](std::size_t n) {
    st.assets = n;
    return st.null_top10_z();
  };
  // The mean of the ten largest of n standard normals.
  CHECK(std::fabs(at(300) - 2.200f) < 0.01f);
  CHECK(std::fabs(at(1000) - 2.641f) < 0.01f);
  CHECK(std::fabs(at(100000) - 3.940f) < 0.01f);
  CHECK(at(5) == 0.0f);
  // Near 300 assets the calibrated 2.5 decides; at 10,000 noise alone scores
  // 3.35, so standing out takes 3.95 (1.18 x, issue #85).
  st.assets = 10000;
  st.top10_z = 3.9f;
  CHECK_FALSE(st.stands_out(2.5f));
  st.top10_z = 4.0f;
  CHECK(st.stands_out(2.5f));
  st.assets = 300;
  st.top10_z = 2.6f;
  CHECK(st.stands_out(2.5f));
  // At 1,000 assets a margin-only pass needs 1.375 x the margin, and a query
  // that stands out 0.375 x; both move with log10 of the assets over 1,000.
  st.assets = 1000;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.055f) < 1e-6f);
  CHECK(std::fabs(st.stand_out_margin(0.04f) - 0.015f) < 1e-6f);
  st.assets = 500;
  st.top10_z = 2.0f;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.0535f) < 1e-4f);
  CHECK(std::fabs(st.stand_out_margin(0.04f) - 0.0105f) < 1e-4f);
  // ... unless the margin is cleared on several assets: a subject that fills
  // much of the library scores like noise because it is the mean (the owner's
  // "mountain", 2026-09-28). One lucky asset is not several.
  CHECK(st.broad_assets() == 5);
  st.over_margin = 1;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.0535f) < 1e-4f);
  st.over_margin = 5;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.04f) < 1e-6f);
  st.over_margin = 0;
  st.assets = 10000;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.06f) < 1e-6f);
  CHECK(std::fabs(st.stand_out_margin(0.04f) - 0.03f) < 1e-6f);
  CHECK(st.broad_assets() == 100);  // 1 %: nonsense reached 0.6 % of 503 assets
}

TEST_CASE("a greyscale JPEG decodes to RGB for the index", "[ai][infer][decode]") {
#if defined(MV_AI_TEST_DECODE)
  const std::string file = env("MV_AI_GREY_JPEG");
  if (file.empty()) SKIP("MV_AI_GREY_JPEG not set");
  auto img = mv::addon::media::decode_still(file, 448);
  if (!img) FAIL("decode_still: " << mv::status_name(img.error()));
  CHECK(img->width > 0);
  CHECK(img->rgb.size() == static_cast<std::size_t>(img->width) * img->height * 3);
#else
  SKIP("a build without the host decoders");
#endif
}

// Mac PR 20 (MAC-VALIDATION §4-5): Core ML must agree with the reference as
// the pack's own self-test demands (cosine 0.99, pack.cpp), on both towers.
TEST_CASE("the Core ML provider agrees with the reference", "[ai][infer][pack][coreml]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  if (pack.empty()) SKIP("MV_AI_PACK_DIR not set");
  const auto ref = mv::json::parse(read_text(fs::path(MV_AI_TEST_DATA) / "reference.json"), 8);
  REQUIRE(ref);
  auto rt = mv::infer::runtime::load(pack);
  REQUIRE(rt);
  if (!(*rt)->has_provider(mv::infer::backend::coreml)) SKIP("no Core ML provider in this runtime");
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.on = mv::infer::backend::coreml;
    mv::infer::provider_fault fault = mv::infer::provider_fault::none;
    auto model = mv::infer::clip_model::open(**rt, *spec, o, &fault);
    INFO(spec->id << " Core ML open: " << mv::infer::fault_name(fault));
    REQUIRE(model);
    const mv::json::value* m = ref->find("models")->find(spec->id);
    float worst = 1.0f;
    for (int k = 0; k < 3; ++k) {
      std::uint32_t w = 0, h = 0;
      const auto px = card(k, w, h);
      const mv::infer::rgb_view view{px.data(), w, h};
      std::vector<float> e;
      REQUIRE((*model)->embed_images(std::span<const mv::infer::rgb_view>(&view, 1), e));
      const float c = cosine(e, floats(m->find("images")->a[static_cast<std::size_t>(k)]));
      worst = std::min(worst, c);
      INFO(spec->id << " Core ML card " << k << " cosine " << c);
      CHECK(c > 0.99f);
    }
    const mv::json::value* queries = ref->find("queries");
    for (std::size_t q = 0; q < queries->a.size(); ++q) {
      auto e = (*model)->embed_text(queries->a[q].s);
      REQUIRE(e);
      const float c = cosine(*e, floats(m->find("texts")->a[q]));
      worst = std::min(worst, c);
      INFO(spec->id << " Core ML query " << queries->a[q].s << " cosine " << c);
      CHECK(c > 0.99f);
    }
    WARN(spec->id << " Core ML min cosine to the reference: " << worst);
  }
}

// Timings for plan/17 (hidden: `mv_ai_tests "[.bench]"`). Batches as shipped:
// four photos (engine.cpp kPhotoBatch), two CPU threads (pack.cpp).
TEST_CASE("bench: CLIP towers, CPU against Core ML", "[.bench][ai][coreml]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  if (pack.empty()) SKIP("MV_AI_PACK_DIR not set");
  auto rt = mv::infer::runtime::load(pack);
  REQUIRE(rt);
  std::vector<std::vector<std::uint8_t>> px(4);
  std::vector<mv::infer::rgb_view> views;
  for (std::size_t i = 0; i < px.size(); ++i) {
    std::uint32_t w = 0, h = 0;
    px[i] = card(static_cast<int>(i % 3), w, h);
    views.push_back(mv::infer::rgb_view{px[i].data(), w, h});
  }
  using clk = std::chrono::steady_clock;
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    for (auto on : {mv::infer::backend::cpu, mv::infer::backend::coreml}) {
      if (!(*rt)->has_provider(on)) continue;
      mv::infer::session_options o;
      o.on = on;
      o.threads = 2;
      const auto t_open = clk::now();
      auto model = mv::infer::clip_model::open(**rt, *spec, o);
      REQUIRE(model);
      const double open_ms = std::chrono::duration<double, std::milli>(clk::now() - t_open).count();
      std::vector<float> e;
      for (int i = 0; i < 5; ++i) REQUIRE((*model)->embed_images(views, e));  // warm
      const int batches = on == mv::infer::backend::cpu && std::string(folder) == "clip-l14" ? 10 : 50;
      const auto t0 = clk::now();
      for (int i = 0; i < batches; ++i) REQUIRE((*model)->embed_images(views, e));
      const double s = std::chrono::duration<double>(clk::now() - t0).count();
      (void)(*model)->embed_text("warm");
      const auto q0 = clk::now();
      for (int i = 0; i < 20; ++i) REQUIRE((*model)->embed_text("guy on a skateboard"));
      const double q_ms = std::chrono::duration<double, std::milli>(clk::now() - q0).count() / 20;
      WARN(folder << " " << mv::infer::backend_name(on) << ": " << (batches * 4) / s << " img/s, "
                  << q_ms << " ms/query, open " << open_ms << " ms");
    }
  }
}

#if defined(MV_AI_TEST_DECODE)
namespace {

// ---- [.calibration] at library sizes (issue #85) ------------------------------

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s + ",") {
    if (c == ',') {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      cur += c;
    }
  }
  return out;
}

std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

std::string json_str(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// The image embeddings of one tower on one provider (MV_AI_CALIBRATION_CACHE),
// append-only so a long run that is stopped resumes where it was, and a cache
// made for 25,000 photos serves any smaller size:
//   <spec key>-<provider>.emb2, little-endian
//   header  16 bytes: "MVCE", u32 version (1), u32 dim, u32 0
//   record  u32 label (index in labels.json), u32 flags (0 embedded, 1 did not
//           decode), u64 FNV-1a 64 of the label's "file", float32[dim]
//           (L2-normalised; zeros when flags is 1)
// numpy: np.fromfile(path, np.dtype([("label", "<u4"), ("flags", "<u4"),
// ("file_hash", "<u8"), ("v", "<f4", dim)]), offset=16). A later record for a
// label replaces an earlier one; one whose hash is not its label's file
// (labels.json changed) is ignored and embedded again; a torn last record (a
// run killed mid-write) is cut off. No path: kept in memory only.
class embedding_cache {
 public:
  struct entry {
    std::uint32_t flags = 0;
    std::vector<float> v;
  };

  void open(const fs::path& path, std::uint32_t dim, const std::function<std::string(std::uint32_t)>& file_of,
            std::size_t labels) {
    path_ = path;
    dim_ = dim;
    if (path_.empty()) return;
    const std::size_t record = 16 + std::size_t{dim} * 4;
    std::size_t whole = 0;  // bytes of complete records after the header
    bool fresh = true;
    {
      std::ifstream in(path_, std::ios::binary);
      char magic[4] = {};
      std::uint32_t version = 0, d = 0, pad = 0;
      if (in.read(magic, 4) && in.read(reinterpret_cast<char*>(&version), 4) &&
          in.read(reinterpret_cast<char*>(&d), 4) && in.read(reinterpret_cast<char*>(&pad), 4) &&
          std::memcmp(magic, "MVCE", 4) == 0 && version == 1 && d == dim) {
        fresh = false;
        std::vector<char> buf(record);
        while (in.read(buf.data(), static_cast<std::streamsize>(record))) {
          whole += record;
          std::uint32_t label = 0, flags = 0;
          std::uint64_t hash = 0;
          std::memcpy(&label, buf.data(), 4);
          std::memcpy(&flags, buf.data() + 4, 4);
          std::memcpy(&hash, buf.data() + 8, 8);
          if (label >= labels || hash != fnv1a(file_of(label))) continue;
          entry e;
          e.flags = flags;
          if (flags == 0) {
            e.v.resize(dim);
            std::memcpy(e.v.data(), buf.data() + 16, std::size_t{dim} * 4);
          }
          entries_[label] = std::move(e);
        }
      }
    }
    if (fresh) {
      std::ofstream out(path_, std::ios::binary | std::ios::trunc);
      const std::uint32_t head[3] = {1, dim, 0};
      out.write("MVCE", 4);
      out.write(reinterpret_cast<const char*>(head), sizeof head);
    } else if (fs::file_size(path_) != 16 + whole) {
      fs::resize_file(path_, 16 + whole);
    }
    out_.open(path_, std::ios::binary | std::ios::app);
  }

  [[nodiscard]] const entry* find(std::uint32_t label) const {
    const auto it = entries_.find(label);
    return it == entries_.end() ? nullptr : &it->second;
  }

  void put(std::uint32_t label, const std::string& file, std::uint32_t flags, std::vector<float> v) {
    if (out_.is_open()) {
      const std::uint64_t hash = fnv1a(file);
      std::vector<float> body = flags == 0 ? v : std::vector<float>(dim_, 0.0f);
      out_.write(reinterpret_cast<const char*>(&label), 4);
      out_.write(reinterpret_cast<const char*>(&flags), 4);
      out_.write(reinterpret_cast<const char*>(&hash), 8);
      out_.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size() * 4));
    }
    entries_[label] = entry{flags, std::move(v)};
  }

  void flush() {
    if (out_.is_open()) out_.flush();
  }

 private:
  fs::path path_;
  std::uint32_t dim_ = 0;
  std::ofstream out_;
  std::map<std::uint32_t, entry> entries_;
};

// The host's own first-pixel decode (media.h), as the indexer gets it, on
// several threads: at ~500 img/s (B/32 on Core ML) one thread decoding COCO
// JPEGs is the bottleneck.
struct decoded {
  bool ok = false;
  mv::addon::rgb_image img;
};

std::vector<decoded> decode_all(const std::vector<std::string>& paths) {
  std::vector<decoded> out(paths.size());
  const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 4u, 10u) - 2;
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (std::size_t i = next++; i < paths.size(); i = next++) {
        auto still = mv::addon::media::decode_still(paths[i], 448);
        if (!still || still->width == 0 || still->height == 0) continue;
        out[i].ok = true;
        out[i].img = std::move(*still);
      }
    });
  }
  for (auto& t : pool) t.join();
  return out;
}

}  // namespace
#endif

// Held-out check of the "nothing found" rule and the Precision scale (hidden:
// `mv_ai_tests "[.calibration]"`), on the engine's own find_text over an int8
// store of the labelled photos. Per tower, per index size, per Precision
// level: real captions of photos in the index should find something (and
// their own photo); strings that describe nothing should not, both the 25 the
// rule was tuned on and a held-out list, so a new rule can be checked for
// overfitting; "helicopter" (COCO has planes and no helicopters) should find
// nothing at the strict levels; for category queries where a near miss is
// plausible (a bus / a truck, a cat / a dog, a horse / a cow, a sandwich / a
// pizza, a plane) and the [eval] test's labelled descriptions it prints how
// many rows came back and how many a caption keyword says are relevant.
// Queries are embedded as the engine does (engine::query_vector: the last
// noun in both numbers, averaged).
//
// The index is the first N photos of labels.json that decode, in its order.
// Sizes: MV_AI_CALIBRATION_SIZES=1000,5000,... (default 300 and every photo up
// to 1,000, plan/17 "Precision scale"; a size past what decodes runs at what
// did). The held-out captions are photos 150-299 and, when every size is at
// least 1,000, every other photo of 300-999 as well: in the index at every size.
// MV_AI_CALIBRATION_TOWERS=clip-b32,clip-l14 (default both). The image tower
// runs on Core ML where the runtime has it, on CUDA with MV_AI_CUDA_DIR, else
// CPU (MV_AI_CALIBRATION_CPU=1 forces CPU, as the plan/17 table was measured).
// MV_AI_CALIBRATION_CACHE=<dir> keeps the image embeddings (embedding_cache
// above) and Core ML's compiled towers between runs.
//
// MV_AI_CALIBRATION_OUT=<file.jsonl> ("{tower}" in it becomes the model folder)
// gets one line per (tower, size, level, query): the rows find_text returned
// and what they hold, and the rule's inputs as find_text computed them
// (scan_stats: assets, top10_z, null_top10_z, mean, sd, over_margin; the best
// margin among the ten best calibrated rows; stands_out; margin_needed), plus
// over_margin uncapped and the raw ranking's P@5 / P@10. Beside it,
// queries-<tower>.f32 holds every query's embedding (float32 little-endian,
// [n, dim], the vector find_text got) and the generic prompts' after them, and
// queries-<tower>.json lists them in that order ({"q", "kind", "text",
// "photo", "label"}; photo: position in the index, label: labels.json index).
// With the cache (photo i of the index is the i-th smallest embedded label)
// that is enough to replay a candidate rule offline.
TEST_CASE("calibration: captions are found, nonsense is not", "[.calibration][ai][infer]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  const std::string eval = env("MV_AI_EVAL_DIR");
  if (pack.empty() || eval.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_EVAL_DIR not set");
#if defined(MV_AI_TEST_DECODE)
  using clk = std::chrono::steady_clock;
  const auto t_start = clk::now();
  // Progress on stderr (a 25,000-photo L/14 run embeds for ~20 minutes).
  const auto say = [&](const std::string& what) {
    std::fprintf(stderr, "[calibration %6.1f s] %s\n", std::chrono::duration<double>(clk::now() - t_start).count(),
                 what.c_str());
  };
  const auto labels = mv::json::parse(read_text(fs::path(eval) / "labels.json"), 8);
  REQUIRE(labels);
  say(std::to_string(labels->a.size()) + " labels");
  const std::string cuda = env("MV_AI_CUDA_DIR");
  auto rt = mv::infer::runtime::load(cuda.empty() ? pack : cuda);
  REQUIRE(rt);
  std::vector<std::size_t> sizes;
  for (const std::string& s : split_list(env("MV_AI_CALIBRATION_SIZES"))) sizes.push_back(std::stoul(s));
  if (sizes.empty()) sizes = {300, std::min<std::size_t>(labels->a.size(), 1000)};
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  REQUIRE(sizes.front() >= 300);  // the held-out captions are photos 150-299
  std::vector<std::string> towers = split_list(env("MV_AI_CALIBRATION_TOWERS"));
  if (towers.empty()) towers = {"clip-b32", "clip-l14"};
  const std::string cache_dir = env("MV_AI_CALIBRATION_CACHE");
  const std::string out_path = env("MV_AI_CALIBRATION_OUT");
  // MV_AI_CALIBRATION_VOCAB=<file>: only embed its lines (one label each) as
  // queries are embedded, to <out folder>/vocab-<tower>.f32 ([n, dim] float32
  // in file order), for a vocabulary check replayed offline; nothing else runs.
  const std::string vocab = env("MV_AI_CALIBRATION_VOCAB");
  const bool cpu_only = env("MV_AI_CALIBRATION_CPU") == "1" || !vocab.empty();
  // What the rule was tuned on (plan/17, 2026-09-27)...
  const std::vector<const char*> nonsense_tune{
      "xyzzy plugh qwertyuiop", "asdf", "blorf zxqv", "qqqq", "lorem ipsum dolor", "zzzzzz",
      "hjkl hjkl", "fnord", "kwyjibo", "wibble wobble", "12345", "!!!", "the the the",
      "grbl", "snorfle", "aaaaa bbbbb", "mxyzptlk", "quux", "flibbertigibbet", "thx1138",
      "ooga booga", "blah", "nothing", "asdfghjkl", "vbnm"};
  // ...and what it was not (issue #85): keyboard mashes, made-up words,
  // numbers, punctuation, stop words alone, long gibberish, lorem ipsum,
  // random syllables.
  const std::vector<const char*> nonsense_heldout{
      "zxcvbnm", "poiuytrewq", "qwerty uiop", "mnbvcxz lkjh", "jkjkjk", "wasd wasd wasd", "ghfjdksla",
      "sdfg hjkl", "florbish", "gleebnork", "trumbulent vex", "skrindle mop", "blivet", "zorptang",
      "plimbo", "quazzle frump", "yibbit", "crondle", "0000", "314159", "867 5309", "42", "99999999",
      "1 2 3 4 5 6", "0.001", "???", "...", "#$%&", "-- --", ":-)", "()[]{}", "of the", "and or but",
      "it is what it is", "a an the", "to be or not to be", "this that these",
      "the florp wended its snarvelous way past the glimmering zonk", "colorless green ideas sleep furiously",
      "mimsy were the borogoves and the mome raths outgrabe",
      "seventeen invisible thoughts argued quietly about tuesday", "an abstract concept of nothing in particular",
      "lorem ipsum", "dolor sit amet consectetur", "adipiscing elit sed do eiusmod", "ipsum lorem",
      "lorem ipsum dolor sit amet", "ka lo mi ta", "bu ra ze po ni", "tofa leki", "vemu dari solo",
      "nak tur bel", "asdfasdf", "foo bar baz", "zyx", "qwfp arst", "hmm", "undefined", "null", "lol"};
  struct category {
    const char* text;
    std::vector<const char*> words;  // a caption holding one of these is relevant
  };
  const std::vector<const char*> plane{"plane", "planes", "airplane", "airplanes", "jet", "jets",
                                       "aircraft", "airliner", "jetliner"};
  const std::vector<category> categories{
      {"helicopter", {"helicopter", "helicopters"}},
      {"a helicopter", {"helicopter", "helicopters"}},
      {"a plane", plane},
      {"a bus", {"bus", "buses", "busses"}},
      {"a truck", {"truck", "trucks"}},
      {"a cat", {"cat", "cats", "kitten", "kittens", "kitty"}},
      {"a dog", {"dog", "dogs", "puppy", "puppies"}},
      {"a horse", {"horse", "horses", "pony"}},
      {"a cow", {"cow", "cows", "cattle", "bull", "calf"}},
      {"a sandwich", {"sandwich", "sandwiches"}},
      {"a pizza", {"pizza", "pizzas"}},
  };
  // The [eval] test's labelled descriptions and its relevance rule (a caption
  // containing one of `any` and, if given, one of `also`).
  struct labelled {
    const char* text;
    std::vector<const char*> any;
    std::vector<const char*> also;
  };
  const std::vector<labelled> described{
      {"guy on a skateboard", {"skateboard"}, {"man", "guy", "boy", "person", "skateboarder", "male"}},
      {"a plate of pizza", {"pizza"}, {}},
      {"giraffes", {"giraffe"}, {}},
      {"a train at the station", {"train"}, {}},
      {"a dog", {"dog"}, {}},
      {"a cat", {"cat"}, {}},
      {"dog", {"dog"}, {}},
  };
  const auto words_of = [](const std::string& text) {
    std::vector<std::string> out;
    std::string w;
    for (char c : text + " ") {
      if (std::isalpha(static_cast<unsigned char>(c))) {
        w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else if (!w.empty()) {
        out.push_back(std::move(w));
        w.clear();
      }
    }
    return out;
  };
  const auto file_of = [&](std::uint32_t label) { return *labels->a[label].str("file"); };
  std::map<std::string, std::ofstream> outs;  // JSONL per path, truncated at first use
  for (const std::string& folder : towers) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.threads = 4;
    if (!cpu_only && (*rt)->has_provider(mv::infer::backend::coreml)) {
      o.on = mv::infer::backend::coreml;
      if (!cache_dir.empty()) {
        fs::create_directories(fs::path(cache_dir) / "coreml");
        o.cache_dir_utf8 = utf8(fs::path(cache_dir) / "coreml");
      }
    } else if (!cpu_only && !cuda.empty()) {
      o.on = mv::infer::backend::cuda;
    }
    mv::infer::provider_fault fault = mv::infer::provider_fault::none;
    auto model = mv::infer::clip_model::open(**rt, *spec, o, &fault);
    if (!model && o.on != mv::infer::backend::cpu) {
      WARN(folder << " " << mv::infer::backend_name(o.on) << " did not open (" << mv::infer::fault_name(fault)
                  << "), CPU instead");
      o.on = mv::infer::backend::cpu;
      model = mv::infer::clip_model::open(**rt, *spec, o);
    }
    REQUIRE(model);
    say(folder + " open on " + mv::infer::backend_name(o.on));
    if (!vocab.empty()) {
      REQUIRE_FALSE(out_path.empty());
      std::ifstream in(vocab);
      std::ofstream f32(fs::path(out_path).parent_path() / ("vocab-" + folder + ".f32"),
                        std::ios::binary | std::ios::trunc);
      std::size_t n = 0;
      for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> forms = mv::ai::query::number_forms(line);
        auto v = forms.size() == 1 ? (*model)->embed_text(forms.front()) : (*model)->embed_text_mean(forms);
        REQUIRE(v);
        f32.write(reinterpret_cast<const char*>(v->data()), static_cast<std::streamsize>(v->size() * 4));
        ++n;
      }
      WARN(folder << ": " << n << " vocabulary lines embedded");
      continue;
    }
    std::string key = spec->spec_key();
    std::replace(key.begin(), key.end(), '/', '_');
    embedding_cache cache;
    cache.open(cache_dir.empty() ? fs::path()
                                 : fs::path(cache_dir) / (key + "-" + mv::infer::backend_name(o.on) + ".emb2"),
               spec->dim, file_of, labels->a.size());

    say(folder + " cache read");
    // Embeddings for the first sizes.back() labels that decode, resuming from
    // the cache: decode the next batch on worker threads while this one embeds.
    const auto t_embed = clk::now();
    const std::size_t need = sizes.back();
    std::size_t planned = 0, next = 0, embedded_now = 0, missing = 0;
    const auto plan = [&] {
      std::vector<std::uint32_t> batch;
      while (batch.size() < 64 && planned < need && next < labels->a.size()) {
        const auto label = static_cast<std::uint32_t>(next++);
        const embedding_cache::entry* e = cache.find(label);
        if (e && e->flags != 0) continue;
        ++planned;
        if (!e) batch.push_back(label);
      }
      return batch;
    };
    const auto start_decode = [&](const std::vector<std::uint32_t>& batch) {
      std::vector<std::string> paths;
      for (std::uint32_t label : batch) paths.push_back(utf8(fs::path(eval) / "img" / file_of(label)));
      return std::async(std::launch::async, [files = std::move(paths)] { return decode_all(files); });
    };
    std::vector<std::uint32_t> cur = plan();
    auto pending = start_decode(cur);
    while (!cur.empty()) {
      std::vector<decoded> stills = pending.get();
      for (std::size_t i = 0; i < cur.size(); ++i) {
        if (stills[i].ok) continue;
        // A file not there (yet: train2017 still unzipping) is not a photo
        // that does not decode; it is tried again next run.
        std::error_code ec;
        if (fs::exists(fs::path(eval) / "img" / file_of(cur[i]), ec)) {
          cache.put(cur[i], file_of(cur[i]), 1, {});
        } else {
          ++missing;
        }
        --planned;
      }
      std::vector<std::uint32_t> after = plan();
      if (!after.empty()) pending = start_decode(after);
      std::vector<mv::infer::rgb_view> views;
      std::vector<std::uint32_t> ok;
      for (std::size_t i = 0; i < cur.size(); ++i) {
        if (!stills[i].ok) continue;
        views.push_back(mv::infer::rgb_view{stills[i].img.rgb.data(), stills[i].img.width, stills[i].img.height});
        ok.push_back(cur[i]);
      }
      std::vector<float> e;
      if (!views.empty()) REQUIRE((*model)->embed_images(views, e));
      for (std::size_t i = 0; i < ok.size(); ++i) {
        const auto* from = e.data() + i * spec->dim;
        cache.put(ok[i], file_of(ok[i]), 0, std::vector<float>(from, from + spec->dim));
      }
      cache.flush();
      if ((embedded_now + ok.size()) / 1000 != embedded_now / 1000) {
        const double s = std::chrono::duration<double>(clk::now() - t_embed).count();
        std::ostringstream m;
        m << folder << ": " << embedded_now + ok.size() << " embedded this run ("
          << static_cast<double>(embedded_now + ok.size()) / s << " img/s), " << planned << " of " << need
          << " planned";
        say(m.str());
      }
      embedded_now += ok.size();
      cur = std::move(after);
    }
    const double embed_s = std::chrono::duration<double>(clk::now() - t_embed).count();

    // The index order: embedded labels, ascending, up to the largest size.
    std::vector<std::uint32_t> which;
    std::vector<const std::vector<float>*> embs;
    for (std::size_t label = 0; label < labels->a.size() && which.size() < need; ++label) {
      const embedding_cache::entry* e = cache.find(static_cast<std::uint32_t>(label));
      if (!e || e->flags != 0) continue;
      which.push_back(static_cast<std::uint32_t>(label));
      embs.push_back(&e->v);
    }
    WARN(folder << " on " << mv::infer::backend_name(o.on) << ": " << which.size() << " photos, " << embedded_now
                << " embedded this run in " << embed_s << " s" << (missing ? ", files missing: " : "")
                << (missing ? std::to_string(missing) : std::string()));
    REQUIRE(embs.size() >= 300);
    std::vector<std::string> first, lower;
    std::vector<std::vector<std::string>> caption_words;
    for (std::uint32_t at : which) {
      const auto& sentences = labels->a[at].find("sentences")->a;
      first.push_back(sentences[0].s);
      std::string all;
      for (const auto& s : sentences) all += " " + s.s;
      caption_words.push_back(words_of(all));
      std::transform(all.begin(), all.end(), all.begin(),
                     [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
      lower.push_back(std::move(all));
    }
    std::vector<std::vector<float>> generic;
    for (const auto& g : spec->generic_prompts) generic.push_back(*(*model)->embed_text(g));
    std::vector<float> gen(embs.size(), -1.0f);
    for (std::size_t i = 0; i < embs.size(); ++i) {
      for (const auto& g : generic) gen[i] = std::max(gen[i], cosine(*embs[i], g));
    }
    mv::ai::text_thresholds t;
    t.query_margin = spec->query_margin;
    t.result_margin = spec->result_margin;
    t.query_z = spec->query_z;
    t.result_z = spec->result_z;

    say(folder + " index order and generic scores");
    // The queries, embedded as engine::query_vector does.
    enum class kind : std::uint8_t { caption, nonsense_tune, nonsense_heldout, category, labelled };
    const char* const kind_names[] = {"caption", "nonsense_tune", "nonsense_heldout", "category", "labelled"};
    struct query {
      kind k;
      std::string text;
      std::size_t photo = 0;  // caption: its photo's position in the index
      std::size_t item = 0;   // category / labelled: its entry
      std::vector<float> v;
    };
    const auto embed = [&](const std::string& text) {
      const std::vector<std::string> forms = mv::ai::query::number_forms(text);
      auto v = forms.size() == 1 ? (*model)->embed_text(forms.front()) : (*model)->embed_text_mean(forms);
      REQUIRE(v);
      return *v;
    };
    std::vector<std::size_t> held;
    for (std::size_t i = 150; i < 300; ++i) held.push_back(i);
    if (sizes.front() >= 1000) {
      for (std::size_t i = 300; i < 1000; i += 2) held.push_back(i);
    }
    std::vector<query> queries;
    for (std::size_t i : held) queries.push_back({kind::caption, first[i], i, 0, embed(first[i])});
    for (const char* s : nonsense_tune) queries.push_back({kind::nonsense_tune, s, 0, 0, embed(s)});
    for (const char* s : nonsense_heldout) queries.push_back({kind::nonsense_heldout, s, 0, 0, embed(s)});
    for (std::size_t c = 0; c < categories.size(); ++c) {
      queries.push_back({kind::category, categories[c].text, 0, c, embed(categories[c].text)});
    }
    for (std::size_t c = 0; c < described.size(); ++c) {
      queries.push_back({kind::labelled, described[c].text, 0, c, embed(described[c].text)});
    }
    // The label vocabulary as engine::load_labels embeds it (one text each);
    // MV_AI_CALIBRATION_NO_LABELS=1 leaves it out (the rule without it).
    std::vector<float> label_vecs;
    const bool use_labels = env("MV_AI_CALIBRATION_NO_LABELS") != "1";
    if (use_labels) {
      for (std::string_view w : mv::ai::labels()) {
        auto v = (*model)->embed_text(w);
        REQUIRE(v);
        label_vecs.insert(label_vecs.end(), v->begin(), v->end());
      }
    }
    const auto relevant = [&](const query& q, std::size_t i) {
      const auto has = [&](const std::vector<const char*>& words) {
        return std::any_of(words.begin(), words.end(),
                           [&](const char* w) { return lower[i].find(w) != std::string::npos; });
      };
      switch (q.k) {
        case kind::caption: return i == q.photo;
        case kind::category:
          for (const std::string& w : caption_words[i]) {
            for (const char* k : categories[q.item].words) {
              if (w == k) return true;
            }
          }
          return false;
        case kind::labelled: {
          const labelled& d = described[q.item];
          return has(d.any) && (d.also.empty() || has(d.also));
        }
        default: return false;
      }
    };
    std::string jsonl;
    if (!out_path.empty()) {
      jsonl = out_path;
      const auto at = jsonl.find("{tower}");
      if (at != std::string::npos) jsonl.replace(at, 7, folder);
      const fs::path dir = fs::path(jsonl).parent_path();
      if (!dir.empty()) fs::create_directories(dir);
      // The query vectors, then the generic prompts', for offline replay.
      std::ofstream f32(dir / ("queries-" + folder + ".f32"), std::ios::binary | std::ios::trunc);
      std::ofstream list(dir / ("queries-" + folder + ".json"), std::ios::trunc);
      list << "[";
      std::size_t n = 0;
      for (const query& q : queries) {
        f32.write(reinterpret_cast<const char*>(q.v.data()), static_cast<std::streamsize>(q.v.size() * 4));
        list << (n ? ",\n" : "\n") << "{\"q\":" << n << ",\"kind\":\"" << kind_names[static_cast<int>(q.k)]
             << "\",\"text\":" << json_str(q.text);
        if (q.k == kind::caption) list << ",\"photo\":" << q.photo << ",\"label\":" << which[q.photo];
        list << "}";
        ++n;
      }
      for (std::size_t g = 0; g < generic.size(); ++g, ++n) {
        f32.write(reinterpret_cast<const char*>(generic[g].data()),
                  static_cast<std::streamsize>(generic[g].size() * 4));
        list << ",\n{\"q\":" << n << ",\"kind\":\"generic\",\"text\":" << json_str(spec->generic_prompts[g]) << "}";
      }
      list << "\n]\n";
      if (!outs.count(jsonl)) outs[jsonl].open(jsonl, std::ios::trunc);
    }

    say(folder + " " + std::to_string(queries.size()) + " queries embedded");
    std::vector<std::size_t> done;
    for (const std::size_t want : sizes) {
      const std::size_t count = std::min(want, embs.size());
      if (count < want) WARN(folder << ": asked for " << want << " photos, " << count << " decode");
      if (std::find(done.begin(), done.end(), count) != done.end()) continue;
      done.push_back(count);
      const auto t_size = clk::now();
      // The engine's matrix: int8 rows, asset id = index + 1.
      mv::ai::vector_store store(spec->dim);
      for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::int8_t> qv;
        float scale = 0;
        mv::ai::quantise(*embs[i], qv, scale);
        store.add(static_cast<std::int64_t>(i + 1), -1, gen[i], scale, qv);
      }
      if (use_labels) store.set_labels(label_vecs, mv::ai::labels().size());
      // Per query: what find_text computed (mirrored: scan_stats, the best
      // margin, over_margin, the gate) and what it returned, per level.
      struct at_level {
        std::vector<std::int64_t> rows;  // find_text's, best first
        float best = -1;
        std::size_t over_margin = 0;
        float stand_out_z = 0;
        bool stands_out = false;
        bool answered = false;  // stands out with the stand-out margin
        float margin_needed = 0;
      };
      struct outcome {
        mv::ai::vector_store::scan_stats stats;
        std::size_t over_margin_all = 0;  // every asset over the query margin
        float top_score = 0;
        std::vector<std::int64_t> ranked;  // the ten best, no margin
        std::size_t pool = 0;              // relevant photos in the index
        at_level level[mv::ai::kPrecisionLevels];
      };
      std::vector<outcome> res(queries.size());
      std::atomic<std::size_t> next_q{0};
      std::vector<std::thread> pool;
      const unsigned threads = std::max(2u, std::thread::hardware_concurrency()) - 1;
      for (unsigned th = 0; th < threads; ++th) {
        pool.emplace_back([&] {
          for (std::size_t qi = next_q++; qi < queries.size(); qi = next_q++) {
            const query& q = queries[qi];
            outcome& r = res[qi];
            r.over_margin_all = store.scan(q.v, {}, store.rows(), true, t.query_margin, -1.0f).size();
            for (const auto& h : store.scan(q.v, {}, 10, false, 0, -1.0f)) {
              if (r.ranked.empty()) r.top_score = h.score;
              r.ranked.push_back(h.asset);
            }
            if (q.k == kind::category || q.k == kind::labelled) {
              for (std::size_t i = 0; i < count; ++i) r.pool += relevant(q, i) ? 1 : 0;
            }
            for (std::uint32_t level = 0; level < mv::ai::kPrecisionLevels; ++level) {
              const auto p = mv::ai::precision_scale::at(level);
              at_level& l = r.level[level];
              // find_text's first pass and its gate, step for step.
              mv::ai::vector_store::scan_stats st;
              const float row_margin = t.result_margin * p.result_margin;
              const auto hits =
                  store.scan(q.v, {}, 5000, true, std::min(t.result_margin, row_margin), -1.0f, &st, true);
              for (std::size_t i = 0, seen = 0; i < hits.size() && seen < 10; ++i) {
                const float margin = hits[i].score - hits[i].generic;
                if (margin < t.result_margin) continue;
                l.best = std::max(l.best, margin);
                ++seen;
              }
              std::set<std::int64_t> over;
              for (const auto& h : hits) {
                if (h.score - h.generic < t.query_margin) continue;
                if (over.insert(h.asset).second && over.size() >= st.broad_assets()) break;
              }
              st.over_margin = over.size();
              l.over_margin = st.over_margin;
              const float query_z = t.query_z * (p.stand_out / mv::ai::vector_store::scan_stats::kStandOutOverNoise);
              l.stand_out_z = std::max(query_z, p.stand_out * st.null_top10_z());
              l.stands_out = st.stands_out(query_z, p.stand_out);
              l.margin_needed = st.margin_needed(t.query_margin) * p.query_margin;
              l.answered = l.stands_out && l.best >= st.stand_out_margin(t.query_margin) * p.query_margin;
              if (level == 0) r.stats = st;
              for (const auto& h : mv::ai::find_text(store, q.v, {}, t, p, true)) l.rows.push_back(h.asset);
            }
          }
        });
      }
      for (auto& th : pool) th.join();
      const double eval_s = std::chrono::duration<double>(clk::now() - t_size).count();

      const auto ratio = [](std::size_t a, std::size_t b) {
        return b ? static_cast<double>(a) / static_cast<double>(b) : 0.0;
      };
      std::vector<std::vector<std::int64_t>> looser;  // the previous level's rows, per query
      for (std::uint32_t level = 0; level < mv::ai::kPrecisionLevels; ++level) {
        int found = 0, own = 0, found150 = 0, own150 = 0, tune = 0, heldout = 0, mirror_off = 0;
        std::string junk;
        std::vector<std::size_t> nonsense_rows, real_rows;
        double p5_sum = 0, p10_sum = 0, p5_min = 1;
        int described_found = 0;
        std::size_t cat_rows = 0, cat_good = 0, cat_pool = 0;
        std::ostringstream cats;
        std::vector<std::vector<std::int64_t>> sorted_rows;
        for (std::size_t qi = 0; qi < queries.size(); ++qi) {
          const query& q = queries[qi];
          const outcome& r = res[qi];
          const at_level& l = r.level[level];
          std::vector<std::int64_t> a = l.rows;
          std::sort(a.begin(), a.end());
          const std::size_t assets = static_cast<std::size_t>(std::unique(a.begin(), a.end()) - a.begin());
          a.resize(assets);
          sorted_rows.push_back(a);
          const bool passed = !l.rows.empty();
          // A query the gate rejects must come back empty (the mirror is true).
          mirror_off += passed && !l.answered && l.best < l.margin_needed ? 1 : 0;
          std::size_t good = 0, good5 = 0, good10 = 0, rank5 = 0, rank10 = 0;
          for (std::size_t i = 0; i < l.rows.size(); ++i) {
            const bool rel = relevant(q, static_cast<std::size_t>(l.rows[i] - 1));
            good += rel ? 1 : 0;
            good5 += rel && i < 5 ? 1 : 0;
            good10 += rel && i < 10 ? 1 : 0;
          }
          for (std::size_t i = 0; i < r.ranked.size(); ++i) {
            const bool rel = relevant(q, static_cast<std::size_t>(r.ranked[i] - 1));
            rank5 += rel && i < 5 ? 1 : 0;
            rank10 += rel ? 1 : 0;
          }
          const bool own_found = q.k == kind::caption && good > 0;
          switch (q.k) {
            case kind::caption:
              found += passed ? 1 : 0;
              own += own_found ? 1 : 0;
              if (q.photo < 300) {
                found150 += passed ? 1 : 0;
                own150 += own_found ? 1 : 0;
              }
              break;
            case kind::nonsense_tune:
            case kind::nonsense_heldout:
              if (passed) {
                (q.k == kind::nonsense_tune ? tune : heldout) += 1;
                junk += " \"" + q.text + "\"";
              }
              nonsense_rows.push_back(l.rows.size());
              break;
            case kind::category:
              cats << "\n    \"" << q.text << "\": " << l.rows.size() << " rows, " << good << " relevant (of "
                   << r.pool << ")";
              if (q.item >= 2) {  // not the helicopters
                real_rows.push_back(l.rows.size());
                cat_rows += l.rows.size();
                cat_good += good;
                cat_pool += r.pool;
              }
              break;
            case kind::labelled:
              real_rows.push_back(l.rows.size());
              described_found += passed ? 1 : 0;
              p5_sum += ratio(good5, 5);
              p10_sum += ratio(good10, 10);
              p5_min = std::min(p5_min, ratio(good5, 5));
              break;
          }
          if (!jsonl.empty()) {
            const auto& st = r.stats;
            const float null_z = st.null_top10_z();
            std::ostringstream line;
            line << "{\"tower\":\"" << folder << "\",\"size\":" << count << ",\"level\":" << level
                 << ",\"q\":" << qi << ",\"kind\":\"" << kind_names[static_cast<int>(q.k)]
                 << "\",\"text\":" << json_str(q.text);
            if (q.k == kind::caption) line << ",\"photo\":" << q.photo << ",\"label\":" << which[q.photo];
            line << ",\"rows\":" << l.rows.size() << ",\"assets\":" << assets
                 << ",\"capped\":" << (l.rows.size() >= 5000 ? "true" : "false") << ",\"passed\":"
                 << (passed ? "true" : "false");
            if (q.k == kind::caption) line << ",\"found_own\":" << (own_found ? "true" : "false");
            if (q.k == kind::category || q.k == kind::labelled) {
              line << ",\"relevant\":" << good << ",\"pool\":" << r.pool << ",\"p5\":" << ratio(good5, 5)
                   << ",\"p10\":" << ratio(good10, 10) << ",\"rank_p5\":" << ratio(rank5, 5) << ",\"rank_p10\":"
                   << ratio(rank10, 10);
            }
            line << ",\"stats_assets\":" << st.assets << ",\"top10_z\":" << st.top10_z << ",\"null_top10_z\":"
                 << null_z << ",\"z_over_null\":" << (null_z > 0 ? st.top10_z / null_z : 0.0f)
                 << ",\"mean\":" << st.mean << ",\"sd\":" << st.sd << ",\"top_score\":" << r.top_score
                 << ",\"over_margin\":" << l.over_margin << ",\"over_margin_all\":" << r.over_margin_all
                 << ",\"broad_assets\":" << st.broad_assets() << ",\"best_margin\":" << l.best
                 << ",\"stand_out_z\":" << l.stand_out_z << ",\"stands_out\":"
                 << (l.stands_out ? "true" : "false") << ",\"margin_needed\":" << l.margin_needed << "}\n";
            outs[jsonl] << line.str();
          }
        }
        CHECK(mirror_off == 0);
        const auto median_max = [](std::vector<std::size_t> v) {
          std::ostringstream s;
          if (v.empty()) return std::string("-");
          std::sort(v.begin(), v.end());
          s << v[v.size() / 2] << "/" << v.back();
          return s.str();
        };
        const std::size_t n_captions = held.size();
        WARN(spec->id << " at " << count << " photos, precision " << level << ": captions found " << found << "/"
                      << n_captions << " (own photo " << own << "; photos 150-299: " << found150 << "/150, own "
                      << own150 << "), nonsense found " << tune << "/" << nonsense_tune.size() << " tune, "
                      << heldout << "/" << nonsense_heldout.size() << " held out, rows median/max "
                      << median_max(nonsense_rows) << "; real rows median/max " << median_max(real_rows)
                      << "; categories P " << ratio(cat_good, cat_rows) << " R " << ratio(cat_good, cat_pool)
                      << "; labelled found "
                      << described_found << "/" << described.size() << ", P@5 mean "
                      << p5_sum / static_cast<double>(described.size()) << " min " << p5_min << ", P@10 mean "
                      << p10_sum / static_cast<double>(described.size()) << "; " << eval_s << " s:" << junk
                      << cats.str());
        // Each level answers a subset of the looser level's rows.
        if (!looser.empty()) {
          for (std::size_t q = 0; q < sorted_rows.size(); ++q) {
            CHECK(std::includes(looser[q].begin(), looser[q].end(), sorted_rows[q].begin(), sorted_rows[q].end()));
          }
        }
        looser = std::move(sorted_rows);
      }
      if (!jsonl.empty()) outs[jsonl].flush();
    }
  }
#else
  SKIP("a build without the host decoders");
#endif
}
