// SPDX-License-Identifier: GPL-2.0-or-later
// The CLIP text tokenizer (byte-level BPE, OpenAI's simple_tokenizer and the
// Hugging Face tokenizer.json of the same checkpoints): the pack's vocab.json
// and merges.txt in, token ids out. plan/17 "Model choice"; the query encode
// is ~10 ms and needs nothing but this and the text tower.
//
// Normalisation: whitespace runs collapse to one space and the text is
// lower-cased. The hosts pass NFC (C# string.Normalize, Swift
// precomposedStringWithCanonicalMapping); an English-first pack is plan/17's
// accepted floor, and letters outside the ranges below split as punctuation
// rather than failing. Pure C++; no OS or ORT dependency.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/result.h"

namespace mv::infer {

class clip_tokenizer {
 public:
  // vocab.json bytes (token -> id) and merges.txt bytes ("#version" line
  // optional). `context` is the text tower's sequence length (77).
  [[nodiscard]] static result<clip_tokenizer> load(std::string_view vocab_json,
                                                   std::string_view merges_txt,
                                                   std::uint32_t context = 77);

  // [start] + BPE ids (truncated to fit) + [end]. Never empty.
  [[nodiscard]] std::vector<std::int64_t> encode(std::string_view utf8) const;

  [[nodiscard]] std::int64_t start_id() const noexcept { return sot_; }
  [[nodiscard]] std::int64_t end_id() const noexcept { return eot_; }
  [[nodiscard]] std::size_t vocab_size() const noexcept { return vocab_.size(); }

  // Exposed for tests: the lower-cased, whitespace-collapsed text and its
  // pre-tokenizer pieces.
  [[nodiscard]] static std::string normalise(std::string_view utf8);
  [[nodiscard]] static std::vector<std::string> pieces(std::string_view normalised);

 private:
  [[nodiscard]] std::vector<std::string> bpe(const std::string& byte_word) const;

  std::unordered_map<std::string, std::int64_t> vocab_;
  std::unordered_map<std::string, std::uint32_t> ranks_;  // "a b" -> merge rank
  std::string byte_to_unicode_[256];
  std::int64_t sot_ = 49406;
  std::int64_t eot_ = 49407;
  std::uint32_t context_ = 77;
};

}  // namespace mv::infer
