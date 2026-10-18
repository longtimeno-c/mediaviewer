// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 D3: the Documents piece's sentence-embedding model (docs/plans/
// document-search.md §1, docs/design/17 "Documents"): a document's passages
// and a typed query in one vector space, so "tax return from my accountant"
// finds the page that is about one without holding those words.
//
// A model is a folder with a model.json (tools/package/ai-models.json writes
// it) naming the ONNX file, the tokenizer, how token vectors pool into one
// (mean or the first token's) and the prefixes the model was trained with
// ("query: " / "passage: "). Vectors come back L2-normalised. Pure C++ over
// infer/ort.h: no OS dependency.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/result.h"
#include "infer/ort.h"

namespace mv::infer {

// BERT's uncased WordPiece (all-MiniLM, bge-small-en, snowflake-arctic-embed):
// a vocab.txt in (one token a line, the id its line), token ids out. The
// normalisation is BertNormalizer's: control characters dropped, whitespace
// made a space, CJK ideographs split apart, then lower-cased with accents
// stripped (core/fold.h: decompose, drop the marks, lower-case; Latin, Greek
// and Cyrillic, the scripts that have them). Punctuation splits a word.
class wordpiece_tokenizer {
 public:
  [[nodiscard]] static result<wordpiece_tokenizer> load(std::string_view vocab_txt);
  // [CLS] + word pieces (truncated to fit) + [SEP], at most `max_len`.
  [[nodiscard]] std::vector<std::int64_t> encode(std::string_view utf8, std::size_t max_len) const;
  [[nodiscard]] std::size_t vocab_size() const noexcept { return vocab_.size(); }
  // Exposed for tests: the normalised text's words and punctuation, in order.
  [[nodiscard]] static std::vector<std::string> pieces(std::string_view utf8);

 private:
  void word(const std::string& w, std::vector<std::int64_t>& out) const;
  std::unordered_map<std::string, std::int64_t> vocab_;
  std::int64_t cls_ = 101, sep_ = 102, unk_ = 100;
};

// model.json of a text-embedding folder.
struct text_spec {
  std::string id;          // "bge-small-en-v1.5"
  std::string name;        // "BGE small (English)"
  std::string precision;   // "int8"
  std::uint32_t dim = 384;
  std::string model_file;  // absolute
  std::string vocab_file;  // vocab.txt, beside it
  bool mean_pool = true;   // else the first token's vector ([CLS])
  std::string query_prefix;
  std::string passage_prefix;
  std::uint32_t max_tokens = 256;
  // "Nothing found" and a row's floor, as CLAP's: margins over the generic
  // prompts (embedded as queries), calibrated by the D3 spike.
  float query_margin = 0.05f;
  float result_margin = 0.02f;
  // The mean cosine of a passage against the generic prompts (the spike's
  // index): what a meaning row's rank counts from, so rows rank by cosine.
  float rank_offset = 0.0f;
  std::vector<std::string> generic_prompts;
  // The index key: a change of model, precision or preprocessing re-queues.
  [[nodiscard]] std::string spec_key() const { return id + "/" + precision + "/pre1"; }
};

[[nodiscard]] result<text_spec> read_text_spec(const std::string& folder_utf8, const std::string& fetched_utf8 = {});

class text_model {
 public:
  // On `options` (CPU: every query and passage gets the same vector on every
  // compute choice; the model is small).
  [[nodiscard]] static result<std::unique_ptr<text_model>> open(const runtime& rt, const text_spec& spec,
                                                                const session_options& options);
  // One vector per text (passage_prefix + text), `out` texts.size() * dim.
  [[nodiscard]] expected embed_passages(std::span<const std::string> texts, std::vector<float>& out) const;
  [[nodiscard]] result<std::vector<float>> embed_query(std::string_view utf8) const;
  [[nodiscard]] const text_spec& spec() const noexcept { return spec_; }
  // Exposed for tests: token ids for a text as the model sees it.
  [[nodiscard]] std::vector<std::int64_t> tokens(std::string_view utf8) const {
    return tok_.encode(utf8, spec_.max_tokens);
  }

 private:
  [[nodiscard]] expected embed(std::span<const std::string> texts, std::string_view prefix,
                               std::vector<float>& out) const;
  text_spec spec_;
  std::unique_ptr<session> s_;
  wordpiece_tokenizer tok_;
  bool type_ids_ = false;  // the graph takes token_type_ids
};

}  // namespace mv::infer
