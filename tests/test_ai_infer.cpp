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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "addons/ai/index_db.h"
#if defined(MV_AI_TEST_DECODE)
#include "abi/addon_media.h"
#endif
#include "addons/ai/vectors.h"
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
#endif
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
    std::vector<std::vector<float>> embs;
    std::vector<std::string> captions;
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
      embs.push_back(e);
      std::string all;
      for (const auto& s : item.find("sentences")->a) all += " " + s.s;
      std::transform(all.begin(), all.end(), all.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
      captions.push_back(all);
    }
    if (embs.size() < 50) SKIP("fewer than 50 labelled images decoded (a build without the host decoders)");
    const auto all_embs = std::move(embs);
    const auto all_captions = std::move(captions);
    for (const std::size_t count : {std::min<std::size_t>(300, all_embs.size()), all_embs.size()}) {
      const std::vector<std::vector<float>> embs(all_embs.begin(), all_embs.begin() + static_cast<std::ptrdiff_t>(count));
      const std::vector<std::string> captions(all_captions.begin(), all_captions.begin() + static_cast<std::ptrdiff_t>(count));
      INFO(count << " photos");
      // The engine's rule (vector_store::scan_stats) on these photos.
      const auto accepted = [&](float best_margin, float z) {
        mv::ai::vector_store::scan_stats st;
        st.assets = count;
        st.top10_z = z;
        return st.stands_out(spec->query_z) || best_margin >= st.margin_needed(spec->query_margin);
      };
      std::vector<std::vector<float>> generic;
      for (const auto& g : spec->generic_prompts) generic.push_back(*(*model)->embed_text(g));
      const auto margin_of = [&](const std::vector<float>& q, std::size_t i) {
        float best_g = -1;
        for (const auto& g : generic) best_g = std::max(best_g, cosine(embs[i], g));
        return cosine(embs[i], q) - best_g;
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
        CHECK(accepted(best_margin, z));
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
        CHECK_FALSE(accepted(nonsense_margin, z));
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
  // 3.35, so standing out takes 3.85.
  st.assets = 10000;
  st.top10_z = 3.5f;
  CHECK_FALSE(st.stands_out(2.5f));
  st.top10_z = 3.9f;
  CHECK(st.stands_out(2.5f));
  st.assets = 300;
  st.top10_z = 2.6f;
  CHECK(st.stands_out(2.5f));
  // A margin-only pass needs 1.5 x the margin when the best score like noise.
  st.top10_z = 2.0f;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.06f) < 1e-6f);
  st.top10_z = 2.3f;
  CHECK(std::fabs(st.margin_needed(0.04f) - 0.04f) < 1e-6f);
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

// Held-out check of the "nothing found" rule (hidden: `mv_ai_tests
// "[.calibration]"`): real captions of photos in the index should be
// accepted, strings that describe nothing should not. Prints the rates.
TEST_CASE("calibration: captions are found, nonsense is not", "[.calibration][ai][infer]") {
  const std::string pack = env("MV_AI_PACK_DIR");
  const std::string eval = env("MV_AI_EVAL_DIR");
  if (pack.empty() || eval.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_EVAL_DIR not set");
#if defined(MV_AI_TEST_DECODE)
  const auto labels = mv::json::parse(read_text(fs::path(eval) / "labels.json"), 8);
  REQUIRE(labels);
  auto rt = mv::infer::runtime::load(pack);
  REQUIRE(rt);
  const std::vector<const char*> nonsense{
      "xyzzy plugh qwertyuiop", "asdf", "blorf zxqv", "qqqq", "lorem ipsum dolor", "zzzzzz",
      "hjkl hjkl", "fnord", "kwyjibo", "wibble wobble", "12345", "!!!", "the the the",
      "grbl", "snorfle", "aaaaa bbbbb", "mxyzptlk", "quux", "flibbertigibbet", "thx1138",
      "ooga booga", "blah", "nothing", "asdfghjkl", "vbnm"};
  for (const char* folder : {"clip-b32", "clip-l14"}) {
    auto spec = mv::infer::read_clip_spec(utf8(fs::path(pack) / "models" / folder));
    REQUIRE(spec);
    mv::infer::session_options o;
    o.threads = 4;
    auto model = mv::infer::clip_model::open(**rt, *spec, o);
    REQUIRE(model);
    const std::size_t n = std::min<std::size_t>(labels->a.size(), 1000);
    std::vector<std::vector<float>> embs;
    std::vector<std::string> first;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& item = labels->a[i];
      auto still = mv::addon::media::decode_still(utf8(fs::path(eval) / "img" / *item.str("file")), 448);
      if (!still) continue;
      const mv::infer::rgb_view view{still->rgb.data(), still->width, still->height};
      std::vector<float> e;
      REQUIRE((*model)->embed_images(std::span<const mv::infer::rgb_view>(&view, 1), e));
      embs.push_back(e);
      first.push_back(item.find("sentences")->a[0].s);
    }
    std::vector<std::vector<float>> generic;
    for (const auto& g : spec->generic_prompts) generic.push_back(*(*model)->embed_text(g));
    std::vector<float> gen(embs.size(), -1.0f);
    for (std::size_t i = 0; i < embs.size(); ++i) {
      for (const auto& g : generic) gen[i] = std::max(gen[i], cosine(embs[i], g));
    }
    const auto accepted = [&](const std::string& text, std::size_t count) {
      const auto q = *(*model)->embed_text(text);
      std::vector<float> s(count);
      for (std::size_t i = 0; i < count; ++i) s[i] = cosine(embs[i], q);
      std::vector<std::size_t> by(count);
      for (std::size_t i = 0; i < count; ++i) by[i] = i;
      std::partial_sort(by.begin(), by.begin() + 10, by.end(), [&](std::size_t a, std::size_t b) { return s[a] > s[b]; });
      float best_margin = -1;
      for (std::size_t r = 0; r < 10; ++r) best_margin = std::max(best_margin, s[by[r]] - gen[by[r]]);
      double mean = 0, var = 0;
      for (float v : s) mean += v;
      mean /= static_cast<double>(count);
      for (float v : s) var += (v - mean) * (v - mean);
      double top = 0;
      for (std::size_t r = 0; r < 10; ++r) top += s[by[r]];
      mv::ai::vector_store::scan_stats st;
      st.assets = count;
      st.top10_z = static_cast<float>((top / 10 - mean) / std::sqrt(var / static_cast<double>(count)));
      // MV_AI_CALIBRATION_OLD=1: the fixed rule before 2026-09-27, for comparison.
      if (const char* o = std::getenv("MV_AI_CALIBRATION_OLD"); o && *o == '1') {
        return st.top10_z >= spec->query_z || best_margin >= spec->query_margin;
      }
      return st.stands_out(spec->query_z) || best_margin >= st.margin_needed(spec->query_margin);
    };
    for (const std::size_t count : {std::min<std::size_t>(300, embs.size()), embs.size()}) {
      // Held out: captions of photos 150..299 (in both subsets).
      int real_ok = 0, real_n = 0, junk_ok = 0;
      for (std::size_t i = 150; i < 300 && i < count; ++i, ++real_n) real_ok += accepted(first[i], count) ? 1 : 0;
      std::string passed;
      for (const char* t : nonsense) {
        if (accepted(t, count)) {
          ++junk_ok;
          passed += std::string(" \"") + t + "\"";
        }
      }
      WARN(spec->id << " at " << count << " photos: captions found " << real_ok << "/" << real_n
                    << ", nonsense found " << junk_ok << "/" << nonsense.size() << ":" << passed);
    }
  }
#else
  SKIP("a build without the host decoders");
#endif
}
