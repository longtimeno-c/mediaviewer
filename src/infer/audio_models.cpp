// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "infer/audio_models.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "core/json.h"
#include "infer/models.h"

namespace mv::infer {
namespace {

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string join(const std::string& dir, const std::string& rel) {
  const std::u8string u = (fs_path(dir) / fs_path(rel)).lexically_normal().generic_u8string();
  return std::string(u.begin(), u.end());
}

result<std::string> read_text(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return err(status::io);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

float number_or(const json::value& doc, std::string_view key, float fallback) {
  const json::value* v = doc.find(key);
  if (!v || v->k != json::kind::number) return fallback;
  return static_cast<float>(v->is_integer ? static_cast<double>(v->i) : v->d);
}

bool plain_name(const std::string& rel) {
  return !rel.empty() && rel.find("..") == std::string::npos && rel.find(':') == std::string::npos &&
         rel.front() != '/' && rel.front() != '\\';
}

// The tokenizer folder is a sibling of the model folder.
bool tokenizer_files(const std::string& folder, const std::string& name, std::string& vocab, std::string& merges) {
  if (!plain_name(name) || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
  const std::string dir = join(join(folder, ".."), name);
  vocab = join(dir, "vocab.json");
  merges = join(dir, "merges.txt");
  return true;
}

}  // namespace

std::vector<std::string> speech_words(std::string_view utf8) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : utf8) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 ||
                      c == '\'';
    if (word) {
      if (c == '\'') continue;  // "we're" -> "were": an apostrophe never splits a word
      cur.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    } else if (!cur.empty()) {
      out.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// ---- CLAP -------------------------------------------------------------------------------

result<clap_spec> read_clap_spec(const std::string& folder) {
  MV_TRY(std::string text, read_text(join(folder, "model.json")));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* precision = doc->str("precision");
  const std::string* audio = doc->str("audio");
  const std::string* text_file = doc->str("text");
  const std::string* tokenizer = doc->str("tokenizer");
  if (!id || !name || !precision || !audio || !text_file || !tokenizer || !plain_name(*audio) ||
      !plain_name(*text_file)) {
    return err(status::corrupt);
  }
  clap_spec s;
  s.id = *id;
  s.name = *name;
  s.precision = *precision;
  s.dim = static_cast<std::uint32_t>(doc->integer("dim").value_or(512));
  s.audio_file = join(folder, *audio);
  s.text_file = join(folder, *text_file);
  if (!tokenizer_files(folder, *tokenizer, s.vocab_file, s.merges_file)) return err(status::corrupt);
  s.bos = doc->integer("bos").value_or(0);
  s.eos = doc->integer("eos").value_or(2);
  s.window_ms = static_cast<std::uint32_t>(doc->integer("window_ms").value_or(10000));
  s.hop_ms = static_cast<std::uint32_t>(doc->integer("hop_ms").value_or(5000));
  s.dedupe = number_or(*doc, "dedupe", s.dedupe);
  s.query_margin = number_or(*doc, "query_margin", s.query_margin);
  s.result_margin = number_or(*doc, "result_margin", s.result_margin);
  if (const json::value* g = doc->find("generic_prompts"); g && g->k == json::kind::array) {
    for (const auto& p : g->a) {
      if (p.k == json::kind::string && !p.s.empty()) s.generic_prompts.push_back(p.s);
    }
  }
  if (s.generic_prompts.empty()) s.generic_prompts = {"a sound.", "noise.", "an audio recording."};
  if (s.dim == 0 || s.dim > 4096 || s.window_ms < 1000 || s.hop_ms == 0) return err(status::corrupt);
  return s;
}

result<std::unique_ptr<clap_model>> clap_model::open(const runtime& rt, const clap_spec& spec,
                                                     const session_options& options, provider_fault* fault) {
  std::unique_ptr<clap_model> m(new clap_model());
  m->spec_ = spec;
  MV_TRY(std::string vocab, read_text(spec.vocab_file));
  MV_TRY(std::string merges, read_text(spec.merges_file));
  MV_TRY(gpt2_tokenizer tok, gpt2_tokenizer::load(vocab, merges));
  m->tok_ = std::move(tok);
  MV_TRY(auto audio, session::open(rt, spec.audio_file, options, fault));
  m->audio_ = std::move(audio);
  session_options text = options;
  text.on = backend::cpu;  // a query's ~10 ms, and the same vector on every compute choice
  MV_TRY(auto t, session::open(rt, spec.text_file, text, nullptr));
  m->text_ = std::move(t);
  return m;
}

expected clap_model::embed_audio(std::span<const std::span<const float>> windows, std::vector<float>& out) {
  out.clear();
  if (windows.empty()) return {};
  tensor_f32 in;
  const auto n = static_cast<std::int64_t>(windows.size());
  in.shape = {n, 1, static_cast<std::int64_t>(clap_features::kFrames), static_cast<std::int64_t>(clap_features::kMels)};
  in.data.reserve(static_cast<std::size_t>(n) * clap_features::kFrames * clap_features::kMels);
  for (const auto& w : windows) features_.compute(w, in.data);
  MV_TRY(auto outs, audio_->run(std::span<const tensor_f32>(&in, 1)));
  for (const tensor_f32& t : outs) {
    if (t.shape.size() == 2 && t.shape[0] == n && t.shape[1] == spec_.dim) {
      out = t.data;
      for (std::int64_t i = 0; i < n; ++i) l2_normalise(std::span<float>(out.data() + i * spec_.dim, spec_.dim));
      return {};
    }
  }
  return err(status::corrupt);
}

result<std::vector<float>> clap_model::embed_text(std::string_view utf8) {
  tensor_i64 in;
  in.data = tok_.encode(utf8, spec_.bos, spec_.eos, 77);
  in.shape = {1, static_cast<std::int64_t>(in.data.size())};
  MV_TRY(auto outs, text_->run_ids(in));
  for (const tensor_f32& t : outs) {
    if (t.shape.size() == 2 && t.shape[0] == 1 && t.shape[1] == spec_.dim) {
      std::vector<float> v = t.data;
      l2_normalise(v);
      return v;
    }
  }
  return err(status::corrupt);
}

// ---- Whisper ------------------------------------------------------------------------------

result<whisper_spec> read_whisper_spec(const std::string& folder) {
  MV_TRY(std::string text, read_text(join(folder, "model.json")));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* precision = doc->str("precision");
  const std::string* encoder = doc->str("encoder");
  const std::string* decoder = doc->str("decoder");
  const std::string* tokenizer = doc->str("tokenizer");
  if (!id || !name || !precision || !encoder || !decoder || !tokenizer || !plain_name(*encoder) ||
      !plain_name(*decoder)) {
    return err(status::corrupt);
  }
  whisper_spec s;
  s.id = *id;
  s.name = *name;
  s.precision = *precision;
  s.quality = static_cast<std::uint32_t>(doc->integer("quality").value_or(1));
  s.encoder_file = join(folder, *encoder);
  s.decoder_file = join(folder, *decoder);
  if (!tokenizer_files(folder, *tokenizer, s.vocab_file, s.merges_file)) return err(status::corrupt);
  s.no_speech_threshold = number_or(*doc, "no_speech_threshold", s.no_speech_threshold);
  s.logprob_threshold = number_or(*doc, "logprob_threshold", s.logprob_threshold);
  return s;
}

result<std::unique_ptr<whisper_model>> whisper_model::open(const runtime& rt, const whisper_spec& spec,
                                                           const session_options& options, provider_fault* fault) {
  std::unique_ptr<whisper_model> m(new whisper_model());
  m->spec_ = spec;
  MV_TRY(std::string vocab, read_text(spec.vocab_file));
  MV_TRY(std::string merges, read_text(spec.merges_file));
  MV_TRY(gpt2_tokenizer tok, gpt2_tokenizer::load(vocab, merges));
  m->tok_ = std::move(tok);
  MV_TRY(auto enc, session::open(rt, spec.encoder_file, options, fault));
  MV_TRY(auto dec, session::open(rt, spec.decoder_file, options, fault));
  m->encoder_ = std::move(enc);
  m->decoder_ = std::move(dec);
  return m;
}

result<speech_window> whisper_model::transcribe(std::span<const float> pcm, std::int64_t start_ms) {
  speech_window out;
  const std::int64_t window_ms = static_cast<std::int64_t>(std::min<std::size_t>(pcm.size(), 480000)) * 1000 / 16000;
  out.consumed_ms = std::max<std::int64_t>(window_ms, 1);
  // Encoder.
  tensor_f32 feats;
  feats.shape = {1, static_cast<std::int64_t>(whisper_features::kMels), static_cast<std::int64_t>(whisper_features::kFrames)};
  features_.compute(pcm, feats.data);
  MV_TRY(auto enc, encoder_->run(std::span<const tensor_f32>(&feats, 1)));
  if (enc.empty() || enc[0].shape.size() != 3) return err(status::corrupt);
  const tensor_f32& hidden = enc[0];

  // The caches the merged decoder carries: past_key_values.<i>.<decoder|encoder>.<key|value>.
  const std::vector<std::string>& in_names = decoder_->input_names();
  const std::vector<std::string>& out_names = decoder_->output_names();
  struct cache {
    std::string name;
    std::vector<std::int64_t> shape;
    std::vector<float> data;
    bool encoder = false;
  };
  std::vector<cache> caches;
  const std::int64_t heads = hidden.shape[2] / 64;  // head_dim is 64 for every Whisper size
  for (const std::string& n : in_names) {
    if (n.rfind("past_key_values.", 0) != 0) continue;
    cache c;
    c.name = n;
    c.encoder = n.find(".encoder.") != std::string::npos;
    c.shape = {1, heads, 0, 64};
    caches.push_back(std::move(c));
  }
  const bool has_flag = std::find(in_names.begin(), in_names.end(), "use_cache_branch") != in_names.end();

  std::vector<std::int64_t> tokens{spec_.sot, spec_.lang, spec_.transcribe};
  std::vector<std::int64_t> sampled;
  double logprob_sum = 0;
  float no_speech_prob = 0;
  bool first = true;
  std::int64_t last_ts = -1;
  for (std::uint32_t step = 0; step < spec_.max_tokens; ++step) {
    std::vector<std::int64_t> ids = first ? tokens : std::vector<std::int64_t>{tokens.back()};
    std::vector<session::named_input> in;
    session::named_input a;
    a.name = "input_ids";
    a.shape = {1, static_cast<std::int64_t>(ids.size())};
    a.type = session::element::i64;
    a.i64 = ids.data();
    a.count = ids.size();
    in.push_back(a);
    session::named_input h;
    h.name = "encoder_hidden_states";
    h.shape = hidden.shape;
    h.f32 = hidden.data.data();
    h.count = hidden.data.size();
    in.push_back(h);
    for (const cache& c : caches) {
      session::named_input p;
      p.name = c.name;
      p.shape = c.shape;
      p.f32 = c.data.data();
      p.count = c.data.size();
      in.push_back(p);
    }
    bool flag = !first;
    if (has_flag) {
      session::named_input f;
      f.name = "use_cache_branch";
      f.shape = {1};
      f.type = session::element::flag;
      f.flag = &flag;
      f.count = 1;
      in.push_back(f);
    }
    MV_TRY(auto res, decoder_->run_named(in));
    if (res.empty() || res[0].shape.size() != 3) return err(status::corrupt);
    const std::int64_t vocab = res[0].shape[2];
    const std::int64_t seq = res[0].shape[1];
    // New caches: every one on the first step, the decoder's on the rest.
    for (std::size_t o = 1; o < res.size() && o < out_names.size(); ++o) {
      std::string past = out_names[o];
      if (past.rfind("present.", 0) != 0) continue;
      past = "past_key_values." + past.substr(8);
      for (cache& c : caches) {
        if (c.name != past || (c.encoder && !first)) continue;
        c.shape = res[o].shape;
        c.data = std::move(res[o].data);
      }
    }
    const float* row = res[0].data.data() + (seq - 1) * vocab;
    if (first) {
      // P(no speech) at the start-of-transcript position (OpenAI's rule).
      const float* sot_row = res[0].data.data();
      float mx = -std::numeric_limits<float>::infinity();
      for (std::int64_t k = 0; k < vocab; ++k) mx = std::max(mx, sot_row[k]);
      double z = 0;
      for (std::int64_t k = 0; k < vocab; ++k) z += std::exp(static_cast<double>(sot_row[k] - mx));
      if (spec_.no_speech < vocab) no_speech_prob = static_cast<float>(std::exp(sot_row[spec_.no_speech] - mx) / z);
    }
    // Logit rules: no specials but end-of-text and timestamps; timestamps come
    // in pairs, never go backwards, and the first token is one (<= 1 s).
    std::vector<float> l(row, row + vocab);
    const float neg = -std::numeric_limits<float>::infinity();
    for (std::int64_t k = spec_.first_special; k < std::min(spec_.ts_begin, vocab); ++k) {
      if (k != spec_.eot) l[static_cast<std::size_t>(k)] = neg;
    }
    const bool last_was_ts = !sampled.empty() && sampled.back() >= spec_.ts_begin;
    const bool penult_was_ts = sampled.size() < 2 || sampled[sampled.size() - 2] >= spec_.ts_begin;
    if (sampled.empty()) {
      for (std::int64_t k = 0; k < std::min(spec_.ts_begin, vocab); ++k) l[static_cast<std::size_t>(k)] = neg;
      for (std::int64_t k = spec_.ts_begin + 51; k < vocab; ++k) l[static_cast<std::size_t>(k)] = neg;
    } else if (last_was_ts) {
      if (penult_was_ts) {
        for (std::int64_t k = spec_.ts_begin; k < vocab; ++k) l[static_cast<std::size_t>(k)] = neg;
      } else {
        for (std::int64_t k = 0; k < spec_.eot; ++k) l[static_cast<std::size_t>(k)] = neg;
      }
    }
    if (last_ts >= 0) {
      const std::int64_t floor_ts = last_was_ts && !penult_was_ts ? last_ts + 1 : last_ts;
      for (std::int64_t k = spec_.ts_begin; k < std::min(floor_ts, vocab); ++k) l[static_cast<std::size_t>(k)] = neg;
    }
    // log-softmax
    float mx = neg;
    for (float v : l) mx = std::max(mx, v);
    double z = 0;
    for (float v : l) z += std::exp(static_cast<double>(v - mx));
    const double lz = std::log(z) + mx;
    // A timestamp wins when the timestamps together outweigh the best word.
    double ts_mass = 0;
    float best_text = neg;
    for (std::int64_t k = spec_.ts_begin; k < vocab; ++k) ts_mass += std::exp(static_cast<double>(l[static_cast<std::size_t>(k)] - mx));
    for (std::int64_t k = 0; k < std::min(spec_.ts_begin, vocab); ++k) best_text = std::max(best_text, l[static_cast<std::size_t>(k)]);
    std::int64_t pick = 0;
    if (std::log(ts_mass) + mx > best_text) {
      pick = spec_.ts_begin + static_cast<std::int64_t>(std::max_element(l.begin() + spec_.ts_begin, l.end()) - (l.begin() + spec_.ts_begin));
    } else {
      pick = static_cast<std::int64_t>(std::max_element(l.begin(), l.end()) - l.begin());
    }
    logprob_sum += static_cast<double>(l[static_cast<std::size_t>(pick)]) - lz;
    first = false;
    tokens.push_back(pick);
    if (pick == spec_.eot) break;
    sampled.push_back(pick);
    if (pick >= spec_.ts_begin) last_ts = pick;
  }
  const double avg_logprob = sampled.empty() ? -10.0 : logprob_sum / static_cast<double>(sampled.size() + 1);
  // Silence, music or wind: Whisper's own rule skips a window it calls "no
  // speech" and was unsure of; one it was unsure of anyway would be retried at
  // a higher temperature, and those retries are where it invents words, so
  // here it is dropped instead (the hallucination guard).
  const bool silent = no_speech_prob > spec_.no_speech_threshold && avg_logprob < spec_.logprob_threshold;
  if (silent || avg_logprob < spec_.logprob_threshold) {
    out.speech = false;
    return out;
  }
  // Segments between timestamp pairs; the seek follows OpenAI's long-form rule.
  std::vector<std::int64_t> text;
  std::int64_t seg_start = -1;
  std::int64_t last_complete_end = -1;
  for (std::int64_t t : sampled) {
    if (t >= spec_.ts_begin) {
      const std::int64_t ms = (t - spec_.ts_begin) * 20;
      if (seg_start < 0) {
        seg_start = ms;
      } else {
        const std::string words = tok_.decode(text, spec_.first_special);
        const auto b = words.find_first_not_of(' ');
        if (b != std::string::npos) {
          out.segments.push_back(speech_segment{start_ms + seg_start, start_ms + ms, words.substr(b)});
        }
        text.clear();
        seg_start = -1;
        last_complete_end = ms;
      }
    } else {
      text.push_back(t);
    }
  }
  const bool single_ts_end = sampled.size() >= 2 && sampled.back() >= spec_.ts_begin &&
                             sampled[sampled.size() - 2] < spec_.ts_begin;
  if (!text.empty() && seg_start >= 0 && !single_ts_end && last_complete_end > 0) {
    // An unfinished last segment: start the next window where it began.
    out.consumed_ms = std::max<std::int64_t>(last_complete_end, 1000);
  } else if (!text.empty() && seg_start >= 0) {
    const std::string words = tok_.decode(text, spec_.first_special);
    const auto b = words.find_first_not_of(' ');
    if (b != std::string::npos) out.segments.push_back(speech_segment{start_ms + seg_start, start_ms + window_ms, words.substr(b)});
  }
  // Whisper's timestamps are coarse (base often says 0.00): a segment starts
  // at its first audible 20 ms frame, so a moment lands where the words do.
  const auto rms_at = [&](std::int64_t ms) {
    const std::size_t a = static_cast<std::size_t>(std::max<std::int64_t>(0, ms) * 16);
    const std::size_t e = std::min(pcm.size(), a + 320);
    double sum = 0;
    for (std::size_t i = a; i < e; ++i) sum += static_cast<double>(pcm[i]) * pcm[i];
    return e > a ? std::sqrt(sum / static_cast<double>(e - a)) : 0.0;
  };
  std::vector<double> frames;
  for (std::int64_t ms = 0; ms < window_ms; ms += 20) frames.push_back(rms_at(ms));
  std::vector<double> sorted = frames;
  std::sort(sorted.begin(), sorted.end());
  const double floor = sorted.empty() ? 0.0 : sorted[sorted.size() / 10];
  const double threshold = std::max(floor * 3.2, 0.005);  // ~10 dB over the quiet tenth, above -46 dBFS
  // Words Whisper left out: a full window cut mid-sentence, or (base, greedy)
  // a decode that stopped early while there is still sound after it. Listen
  // again from where the transcript ended (never backwards: it only moves on).
  bool audible_after = false;
  const std::int64_t heard_to = last_complete_end > 0 ? last_complete_end : 0;
  for (std::int64_t ms = heard_to + 500; ms < window_ms; ms += 20) {
    if (frames[static_cast<std::size_t>(ms / 20)] >= threshold) {
      audible_after = true;
      break;
    }
  }
  if (window_ms >= 29000 && out.consumed_ms >= window_ms - 1000) {
    out.consumed_ms = std::max<std::int64_t>(std::max<std::int64_t>(heard_to, window_ms - 5000), 1000);
  } else if (audible_after && heard_to >= 1000 && heard_to < out.consumed_ms) {
    out.consumed_ms = heard_to;
  }
  for (speech_segment& s : out.segments) {
    const std::int64_t from = s.start_ms - start_ms, to = s.end_ms - start_ms;
    for (std::int64_t ms = std::max<std::int64_t>(0, from); ms < to && ms < window_ms; ms += 20) {
      if (frames[static_cast<std::size_t>(ms / 20)] >= threshold) {
        s.start_ms = start_ms + ms;
        break;
      }
    }
  }
  out.speech = !out.segments.empty();
  return out;
}

}  // namespace mv::infer
