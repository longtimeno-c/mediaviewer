// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 D3: the Documents piece's text model (docs/design/17 "Documents"). The
// WordPiece tokenizer's rules always; with the ai-docs piece staged, its ids
// against the Hugging Face tokenizer and its vectors against ORT-Python on
// CPU (tools/ai-reference/make_docs_reference.py), cosine >= 0.999: the D3
// verify line, on each platform.
//
//   always              WordPiece normalisation, pre-tokenisation and pieces on a toy vocab
//   MV_AI_PACK_DIR +    the Core pack's runtime and
//   MV_AI_DOCS_DIR      the staged ai-docs piece: tests/data/ai/docs_reference.json
#include "catch_compat.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/json.h"
#include "infer/models.h"
#include "infer/text_models.h"

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

double num(const mv::json::value& v) { return v.is_integer ? static_cast<double>(v.i) : v.d; }

// A vocab.txt: the specials at BERT's ids, then a few pieces.
std::string toy_vocab() {
  std::string v;
  for (int i = 0; i < 100; ++i) v += "[unused" + std::to_string(i) + "]\n";
  v += "[UNK]\n[CLS]\n[SEP]\n[MASK]\n";  // 100 .. 103
  for (const char* w : {"un", "##aff", "##able", "the", "cafe", ",", "!", "resume", "a", "##b", "東", "京"}) {
    v += w;
    v += '\n';
  }
  for (int i = 0; i < 1000; ++i) v += "filler" + std::to_string(i) + "\n";
  return v;
}

}  // namespace

TEST_CASE("WordPiece normalises and splits as BERT's uncased tokenizer", "[ai][docs]") {
  using mv::infer::wordpiece_tokenizer;
  CHECK(wordpiece_tokenizer::pieces("Hello,  World!") == std::vector<std::string>{"hello", ",", "world", "!"});
  // Accents stripped, case folded, control characters dropped, CJK apart.
  CHECK(wordpiece_tokenizer::pieces("Café RÉSUMÉ\x01") == std::vector<std::string>{"cafe", "resume"});
  CHECK(wordpiece_tokenizer::pieces("東京x") == std::vector<std::string>{"東", "京", "x"});
  CHECK(wordpiece_tokenizer::pieces("a b\tc") == std::vector<std::string>{"a", "b", "c"});
  // Beyond Latin, Greek and Cyrillic: NFD's base (で -> て), marks dropped (Hebrew
  // points), Hangul into jamo.
  CHECK(wordpiece_tokenizer::pieces("です") == std::vector<std::string>{"てす"});
  CHECK(wordpiece_tokenizer::pieces("שָׁלוֹם") == std::vector<std::string>{"שלום"});
  CHECK(wordpiece_tokenizer::pieces("한") == std::vector<std::string>{"\u1112\u1161\u11ab"});

  auto tok = wordpiece_tokenizer::load(toy_vocab());
  REQUIRE(tok);
  // [CLS] un ##aff ##able , the [SEP]: longest match first.
  CHECK(tok->encode("Unaffable, the", 64) == std::vector<std::int64_t>{101, 104, 105, 106, 109, 107, 102});
  // A word with a part no piece covers is [UNK] whole; so is one over 100 characters.
  CHECK(tok->encode("unzzz", 64) == std::vector<std::int64_t>{101, 100, 102});
  CHECK(tok->encode(std::string(101, 'a'), 64) == std::vector<std::int64_t>{101, 100, 102});
  CHECK(tok->encode("ab", 64) == std::vector<std::int64_t>{101, 112, 113, 102});
  // Truncated to fit, [SEP] kept.
  CHECK(tok->encode("the the the the the", 4) == std::vector<std::int64_t>{101, 107, 107, 102});
  CHECK(!wordpiece_tokenizer::load("a\nb\n"));
}

TEST_CASE("the Documents model matches the ORT-Python reference", "[ai][docs][pack]") {
  const std::string core = env("MV_AI_PACK_DIR");
  const std::string docs = env("MV_AI_DOCS_DIR");
  if (core.empty() || docs.empty()) SKIP("MV_AI_PACK_DIR and MV_AI_DOCS_DIR not set");
  auto doc = mv::json::parse(read_text(fs::path(MV_AI_TEST_DATA) / "docs_reference.json"), 8);
  REQUIRE(doc);
  auto spec = mv::infer::read_text_spec(utf8(fs::path(docs) / "models/doc-text"));
  REQUIRE(spec);
  REQUIRE(*doc->str("model") == spec->id);
  auto rt = mv::infer::runtime::load(core);
  REQUIRE(rt);
  mv::infer::session_options o;
  o.threads = 2;
  auto model = mv::infer::text_model::open(**rt, *spec, o);
  REQUIRE(model);
  float worst = 1.0f;
  const auto check = [&](const mv::json::value& row, const std::string& prefix, bool query) {
    const std::string text = row.str("text") ? *row.str("text") : std::string();
    std::vector<std::int64_t> want_ids;
    for (const auto& x : row.find("ids")->a) want_ids.push_back(x.i);
    INFO(text);
    CHECK((*model)->tokens(prefix + text) == want_ids);
    std::vector<float> got;
    if (query) {
      auto v = (*model)->embed_query(text);
      REQUIRE(v);
      got = *v;
    } else {
      REQUIRE((*model)->embed_passages(std::vector<std::string>{text}, got));
    }
    std::vector<float> want;
    for (const auto& x : row.find("vec")->a) want.push_back(static_cast<float>(num(x)));
    REQUIRE(got.size() == want.size());
    const float c = mv::infer::dot(got, want);
    worst = std::min(worst, c);
    INFO("cosine " << c);
    CHECK(c >= 0.999f);
  };
  for (const auto& p : doc->find("passages")->a) check(p, spec->passage_prefix, false);
  for (const auto& q : doc->find("queries")->a) check(q, spec->query_prefix, true);
  // A batch pads its shorter rows: the vector must not move with the batch.
  std::vector<std::string> texts;
  for (const auto& p : doc->find("passages")->a) texts.push_back(*p.str("text"));
  std::vector<float> batch;
  REQUIRE((*model)->embed_passages(texts, batch));
  for (std::size_t i = 0; i < texts.size(); ++i) {
    std::vector<float> want;
    for (const auto& x : doc->find("passages")->a[i].find("vec")->a) want.push_back(static_cast<float>(num(x)));
    const float c = mv::infer::dot(std::span<const float>(batch.data() + i * spec->dim, spec->dim), want);
    worst = std::min(worst, c);
    INFO("batched passage " << i << " cosine " << c);
    CHECK(c >= 0.999f);
  }
  WARN("worst cosine to ORT-Python: " << worst);
}
