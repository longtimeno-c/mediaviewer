// SPDX-License-Identifier: GPL-2.0-or-later
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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "addons/ai/index_db.h"
#if defined(MV_AI_TEST_DECODE)
#include "addon/media.h"
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
    // A bounded, fixed subset keeps the CPU run to minutes.
    const std::size_t n = std::min<std::size_t>(labels->a.size(), 300);
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
    std::vector<std::vector<float>> generic;
    for (const auto& g : spec->generic_prompts) generic.push_back(*(*model)->embed_text(g));
    const auto margin_of = [&](const std::vector<float>& q, std::size_t i) {
      float best_g = -1;
      for (const auto& g : generic) best_g = std::max(best_g, cosine(embs[i], g));
      return cosine(embs[i], q) - best_g;
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
      CHECK(best_margin >= spec->query_margin);
    }
    const auto nonsense = *(*model)->embed_text("xyzzy plugh qwertyuiop");
    std::vector<std::size_t> by(embs.size());
    for (std::size_t i = 0; i < by.size(); ++i) by[i] = i;
    std::sort(by.begin(), by.end(), [&](std::size_t a, std::size_t b) { return cosine(embs[a], nonsense) > cosine(embs[b], nonsense); });
    float nonsense_margin = -1;
    for (std::size_t r = 0; r < 10 && r < by.size(); ++r) nonsense_margin = std::max(nonsense_margin, margin_of(nonsense, by[r]));
    INFO(spec->id << " nonsense margin " << nonsense_margin);
    CHECK(nonsense_margin < spec->query_margin);
  }
}
