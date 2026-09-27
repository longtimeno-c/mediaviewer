// SPDX-License-Identifier: GPL-2.0-or-later
// The audio index's models (plan/17 "Audio", owner 2026-09-27): what a clip
// SOUNDS like (LAION CLAP: an audio tower and a text tower in one space, so a
// description finds "dog barking" the way CLIP finds a picture) and what is
// SAID in it (OpenAI Whisper: timestamped transcripts, searched as text).
// Both read a clip's soundtrack as mono float PCM (host table v2
// audio_open). Folders and model.json files as for CLIP (models.h).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "infer/audio_features.h"
#include "infer/clip_tokenizer.h"
#include "infer/ort.h"

namespace mv::infer {

// ---- sounds -------------------------------------------------------------------

struct clap_spec {
  std::string id;         // "clap-general"
  std::string name;       // "LAION CLAP (general)"
  std::string precision;  // "fp16"
  std::uint32_t dim = 512;
  std::string audio_file, text_file, vocab_file, merges_file;
  std::int64_t bos = 0, eos = 2;
  std::uint32_t window_ms = 10000;  // one embedding per window
  std::uint32_t hop_ms = 5000;
  float dedupe = 0.97f;
  float query_margin = 0.04f;
  float result_margin = 0.015f;
  std::vector<std::string> generic_prompts;
  [[nodiscard]] std::string spec_key() const { return id + "/" + precision + "/pre1"; }
};

[[nodiscard]] result<clap_spec> read_clap_spec(const std::string& folder_utf8);

class clap_model {
 public:
  [[nodiscard]] static result<std::unique_ptr<clap_model>> open(const runtime& rt, const clap_spec& spec,
                                                                const session_options& options,
                                                                provider_fault* fault = nullptr);
  // One L2-normalised vector per window (48 kHz mono PCM).
  [[nodiscard]] expected embed_audio(std::span<const std::span<const float>> windows, std::vector<float>& out);
  [[nodiscard]] result<std::vector<float>> embed_text(std::string_view utf8);
  [[nodiscard]] const clap_spec& spec() const noexcept { return spec_; }
  [[nodiscard]] backend on() const noexcept { return audio_->on(); }

 private:
  clap_spec spec_;
  std::unique_ptr<session> audio_, text_;
  gpt2_tokenizer tok_;
  clap_features features_;
};

// ---- speech -------------------------------------------------------------------

struct whisper_spec {
  std::string id;         // "whisper-base"
  std::string name;
  std::string precision;
  std::uint32_t quality = 1;  // mv_ai_quality: 1 base, 2 small
  std::string encoder_file, decoder_file, vocab_file, merges_file;
  std::int64_t sot = 50258, lang = 50259, transcribe = 50359, no_timestamps = 50363;
  std::int64_t no_speech = 50362, eot = 50257, first_special = 50257, ts_begin = 50364;
  float no_speech_threshold = 0.6f;   // skip a window the model thinks is not speech
  float logprob_threshold = -1.0f;    // ...or whose words it was unsure of (hallucinations)
  std::uint32_t max_tokens = 224;
  [[nodiscard]] std::string spec_key() const { return id + "/" + precision + "/pre1"; }
};

[[nodiscard]] result<whisper_spec> read_whisper_spec(const std::string& folder_utf8);

struct speech_segment {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
};

struct speech_window {
  std::vector<speech_segment> segments;  // times on the clip's timeline
  std::int64_t consumed_ms = 30000;      // where the next window starts, from this one's start
  bool speech = false;                   // false: silence / music / noise, nothing kept
};

class whisper_model {
 public:
  [[nodiscard]] static result<std::unique_ptr<whisper_model>> open(const runtime& rt, const whisper_spec& spec,
                                                                   const session_options& options,
                                                                   provider_fault* fault = nullptr);
  // Up to 30 s of 16 kHz mono PCM that starts at `start_ms` on the clip.
  [[nodiscard]] result<speech_window> transcribe(std::span<const float> pcm, std::int64_t start_ms);
  [[nodiscard]] const whisper_spec& spec() const noexcept { return spec_; }
  [[nodiscard]] backend on() const noexcept { return encoder_->on(); }

 private:
  whisper_spec spec_;
  std::unique_ptr<session> encoder_, decoder_;
  gpt2_tokenizer tok_;
  whisper_features features_;
};

// Words for the transcript match: lower-case letters and digits, split on
// everything else (exposed for tests and the index's search).
[[nodiscard]] std::vector<std::string> speech_words(std::string_view utf8);

}  // namespace mv::infer
