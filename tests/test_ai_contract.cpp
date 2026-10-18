// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #482 T2: the model contract (docs/plans/model-tiers.md). A model that fits it
// is a manifest entry; a kind this build does not implement is refused as
// unsupported, never guessed. Also T3's fetched folder: a file model.json
// names comes from the model's own folder when there, else the downloads.
// Pack-free: each case writes a model.json and reads it back.
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "infer/audio_models.h"
#include "infer/models.h"
#include "infer/text_models.h"

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

struct scratch {
  fs::path root;
  explicit scratch(const char* name) : root(fs::temp_directory_path() / name) {
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path folder(const std::string& rel, const std::string& model_json) const {
    const fs::path d = root / rel;
    fs::create_directories(d);
    if (!model_json.empty()) std::ofstream(d / "model.json", std::ios::binary) << model_json;
    return d;
  }
  void touch(const fs::path& p) const {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
  }
};

std::string clip_json(const std::string& extra) {
  return R"({"id": "t", "name": "T", "precision": "fp16", "dim": 512, "image_size": 224,
             "image": "image.onnx", "text": "text.onnx", "tokenizer": "tok")" +
         extra + "}";
}

}  // namespace

TEST_CASE("a CLIP tower states its tokenizer and score kinds", "[ai][contract]") {
  scratch s("mv_ai_contract_clip");
  auto ok = mv::infer::read_clip_spec(utf8(s.folder("pack/models/a", clip_json(""))));
  REQUIRE(ok);
  CHECK(ok->tokenizer_kind == "clip-bpe");
  CHECK(ok->score == "cosine");
  CHECK(ok->coreml_dims[0] == "batch_size");
  CHECK(ok->image_output.empty());

  auto named = mv::infer::read_clip_spec(utf8(s.folder("pack/models/b", clip_json(
      R"(, "score": "sigmoid", "image_output": "image_embeds", "text_output": "text_embeds",
         "coreml_dims": ["n", "c", "h", "w"])"))));
  REQUIRE(named);
  CHECK(named->score == "sigmoid");
  CHECK(named->image_output == "image_embeds");
  CHECK(named->coreml_dims[3] == "w");
  // "softmax" is the CLIP training objective; it ranks by cosine.
  auto softmax = mv::infer::read_clip_spec(utf8(s.folder("pack/models/c", clip_json(R"(, "score": "softmax")"))));
  REQUIRE(softmax);
  CHECK(softmax->score == "cosine");

  // Kinds this build does not have: refused as unsupported, not corrupt.
  auto sp = mv::infer::read_clip_spec(utf8(s.folder("pack/models/d", clip_json(R"(, "tokenizer_kind": "sentencepiece")"))));
  REQUIRE_FALSE(sp);
  CHECK(sp.error() == mv::status::unsupported_format);
  auto odd = mv::infer::read_clip_spec(utf8(s.folder("pack/models/e", clip_json(R"(, "score": "hinge")"))));
  REQUIRE_FALSE(odd);
  CHECK(odd.error() == mv::status::unsupported_format);
  CHECK_FALSE(mv::infer::read_clip_spec(utf8(s.folder("pack/models/f", clip_json(R"(, "coreml_dims": ["n"])")))));
}

TEST_CASE("a model's files come from its folder, else the fetched one", "[ai][contract]") {
  scratch s("mv_ai_contract_fetch");
  const fs::path own = s.folder("pack/models/a", clip_json(""));
  const fs::path got = s.root / "data" / "models" / "a";
  s.touch(own / "text.onnx");                     // carried: the pack's copy wins
  s.touch(got / "image.onnx");                    // fetched only
  s.touch(s.root / "data/models/tok/vocab.json");  // the tokenizer folder too
  auto spec = mv::infer::read_clip_spec(utf8(own), utf8(got));
  REQUIRE(spec);
  CHECK(fs::path(std::u8string(spec->text_file.begin(), spec->text_file.end())) == own / "text.onnx");
  CHECK(fs::equivalent(fs::path(std::u8string(spec->image_file.begin(), spec->image_file.end())), got / "image.onnx"));
  CHECK(fs::equivalent(fs::path(std::u8string(spec->vocab_file.begin(), spec->vocab_file.end())),
                       s.root / "data/models/tok/vocab.json"));
}

TEST_CASE("a Whisper model states its mel bins and special tokens", "[ai][contract]") {
  scratch s("mv_ai_contract_whisper");
  const std::string base = R"({"id": "w", "name": "W", "precision": "fp16", "encoder": "e.onnx",
                               "decoder": "d.onnx", "tokenizer": "tok")";
  auto small = mv::infer::read_whisper_spec(utf8(s.folder("a/models/w1", base + "}")));
  REQUIRE(small);
  CHECK(small->n_mels == 80);
  CHECK(small->transcribe == 50359);

  // large-v3 / turbo: 128 mels, and the extra language token moves the rest.
  auto turbo = mv::infer::read_whisper_spec(utf8(s.folder("a/models/w2", base + R"(, "n_mels": 128,
      "tokens": {"transcribe": 50360, "no_speech": 50363, "no_timestamps": 50364, "ts_begin": 50365}})")));
  REQUIRE(turbo);
  CHECK(turbo->n_mels == 128);
  CHECK(turbo->transcribe == 50360);
  CHECK(turbo->ts_begin == 50365);
  CHECK(turbo->sot == 50258);  // unnamed tokens keep their defaults

  auto mels = mv::infer::read_whisper_spec(utf8(s.folder("a/models/w3", base + R"(, "n_mels": 96})")));
  REQUIRE_FALSE(mels);
  CHECK(mels.error() == mv::status::unsupported_format);
  CHECK_FALSE(mv::infer::read_whisper_spec(utf8(s.folder("a/models/w4", base + R"(, "tokens": {"eot": "x"}})"))));
  // 128-bin features are 128 x 3000.
  mv::infer::whisper_features f(128);
  std::vector<float> out;
  f.compute(std::vector<float>(16000, 0.1f), out);
  CHECK(out.size() == 128u * mv::infer::whisper_features::kFrames);
}

TEST_CASE("SFace and e5-style prefixes are data", "[ai][contract]") {
  scratch s("mv_ai_contract_data");
  auto face = mv::infer::read_face_spec(utf8(s.folder("f/models/faces", R"({"detector": "yunet.onnx",
      "embedder": "sface.onnx", "spec": "yunet-2023mar+sface-2021dec/pre1"})")));
  REQUIRE(face);
  CHECK(face->dim == 128);
  CHECK(face->embed_scale == 1.0f);
  CHECK(face->spec_key == "yunet-2023mar+sface-2021dec/pre1");

  auto text = mv::infer::read_text_spec(utf8(s.folder("d/models/doc", R"({"id": "e5", "name": "E5", "precision": "fp32",
      "model": "model.onnx", "vocab": "vocab.txt", "tokenizer": "wordpiece", "pooling": "mean",
      "query_prefix": "query: ", "passage_prefix": "passage: "})")));
  REQUIRE(text);
  CHECK(text->query_prefix == "query: ");
}
