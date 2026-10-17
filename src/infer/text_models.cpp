// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "infer/text_models.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "core/fold.h"
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
         rel.find('/') == std::string::npos && rel.find('\\') == std::string::npos;
}

// One code point from UTF-8 at `i` (advanced); an invalid byte is U+FFFD.
char32_t next_cp(std::string_view s, std::size_t& i) {
  const auto b = static_cast<unsigned char>(s[i]);
  std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
  if (n == 0 || i + n > s.size()) {
    ++i;
    return 0xFFFD;
  }
  char32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
  for (std::size_t k = 1; k < n; ++k) {
    const auto t = static_cast<unsigned char>(s[i + k]);
    if ((t & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    c = (c << 6) | (t & 0x3F);
  }
  i += n;
  return c;
}

void put(std::string& out, char32_t c) { text::fold_detail::put_utf8(out, c); }

// BertNormalizer's classes (the Unicode categories it asks for, by range).
bool is_space(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool is_control(char32_t c) {
  if (c == '\t' || c == '\n' || c == '\r') return false;
  return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xAD || (c >= 0x200B && c <= 0x200F) ||
         (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
}
bool is_cjk(char32_t c) {
  return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) ||
         (c >= 0x2A700 && c <= 0x2B73F) || (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF) ||
         (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}
bool is_punct(char32_t c) {
  if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) return true;
  // Unicode P*: Latin-1, General Punctuation, CJK and fullwidth forms.
  return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF ||
         c == 0x37E || c == 0x387 || (c >= 0x55A && c <= 0x55F) || c == 0x589 || c == 0x5BE ||
         (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) ||
         (c >= 0x3008 && c <= 0x3011) || (c >= 0x3014 && c <= 0x301F) || (c >= 0xFF01 && c <= 0xFF0F) ||
         (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);
}

// BERT's "strip accents" past core/fold.h's scripts: NFD's base characters
// (kana with a dakuten, CJK compatibility ideographs) and no nonspacing mark.
struct decompose_entry {
  char32_t from, a, b;
};
constexpr decompose_entry k_decompose[] = {
#define MV_WORDPIECE_DECOMPOSE
#include "infer/wordpiece_strip.inc"
#undef MV_WORDPIECE_DECOMPOSE
};
struct mark_range {
  char32_t first, last;
};
constexpr mark_range k_marks[] = {
#define MV_WORDPIECE_MARKS
#include "infer/wordpiece_strip.inc"
#undef MV_WORDPIECE_MARKS
};

void strip_rest(std::string_view in, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < in.size();) {
    const char32_t c = next_cp(in, i);
    if (c >= 0xAC00 && c <= 0xD7A3) {  // Hangul: NFD by formula, into conjoining jamo
      const char32_t s = c - 0xAC00;
      put(out, 0x1100 + s / 588);
      put(out, 0x1161 + (s % 588) / 28);
      if (s % 28) put(out, 0x11A7 + s % 28);
      continue;
    }
    if (c < 0x0530) {
      put(out, c);
      continue;
    }
    const auto d = std::lower_bound(std::begin(k_decompose), std::end(k_decompose), c,
                                    [](const decompose_entry& e, char32_t v) { return e.from < v; });
    if (d != std::end(k_decompose) && d->from == c) {
      put(out, d->a);
      if (d->b) put(out, d->b);
      continue;
    }
    const auto m = std::upper_bound(std::begin(k_marks), std::end(k_marks), c,
                                    [](char32_t v, const mark_range& r) { return v < r.first; });
    if (m != std::begin(k_marks) && c <= std::prev(m)->last) continue;  // a nonspacing mark
    put(out, c);
  }
}

}  // namespace

// ---- WordPiece ------------------------------------------------------------------------------

result<wordpiece_tokenizer> wordpiece_tokenizer::load(std::string_view vocab_txt) {
  wordpiece_tokenizer t;
  std::int64_t id = 0;
  std::size_t i = 0;
  while (i < vocab_txt.size()) {
    std::size_t e = vocab_txt.find('\n', i);
    if (e == std::string_view::npos) e = vocab_txt.size();
    std::string_view line = vocab_txt.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    t.vocab_.emplace(std::string(line), id++);
    i = e + 1;
  }
  const auto find = [&](const char* s) -> std::int64_t {
    auto it = t.vocab_.find(s);
    return it == t.vocab_.end() ? -1 : it->second;
  };
  t.cls_ = find("[CLS]");
  t.sep_ = find("[SEP]");
  t.unk_ = find("[UNK]");
  if (t.cls_ < 0 || t.sep_ < 0 || t.unk_ < 0 || t.vocab_.size() < 1000) return err(status::corrupt);
  return t;
}

std::vector<std::string> wordpiece_tokenizer::pieces(std::string_view utf8) {
  // BertNormalizer: clean, CJK apart, then lower-case and strip accents.
  std::string clean;
  clean.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const char32_t c = next_cp(utf8, i);
    if (c == 0 || c == 0xFFFD || is_control(c)) continue;
    if (is_space(c)) {
      clean.push_back(' ');
    } else if (is_cjk(c)) {
      clean.push_back(' ');
      put(clean, c);
      clean.push_back(' ');
    } else {
      put(clean, c);
    }
  }
  std::string norm;
  strip_rest(text::fold(clean), norm);
  // BertPreTokenizer: whitespace splits, and every punctuation mark is a word.
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < norm.size();) {
    const std::size_t at = i;
    const char32_t c = next_cp(norm, i);
    if (is_space(c)) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else if (is_punct(c)) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
      out.emplace_back(norm.substr(at, i - at));
    } else {
      cur.append(norm, at, i - at);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

void wordpiece_tokenizer::word(const std::string& w, std::vector<std::int64_t>& out) const {
  // Greedy longest match first, "##" on every piece after the first; a word
  // with a part no piece covers, or over 100 characters, is [UNK] whole.
  std::vector<std::size_t> cps;  // byte offset of each code point, and the end
  for (std::size_t i = 0; i < w.size();) {
    cps.push_back(i);
    (void)next_cp(w, i);
  }
  cps.push_back(w.size());
  const std::size_t n = cps.size() - 1;
  if (n > 100) {
    out.push_back(unk_);
    return;
  }
  std::vector<std::int64_t> ids;
  std::size_t start = 0;
  std::string sub;
  while (start < n) {
    std::size_t end = n;
    std::int64_t found = -1;
    while (end > start) {
      sub = start > 0 ? "##" : "";
      sub.append(w, cps[start], cps[end] - cps[start]);
      if (auto it = vocab_.find(sub); it != vocab_.end()) {
        found = it->second;
        break;
      }
      --end;
    }
    if (found < 0) {
      out.push_back(unk_);
      return;
    }
    ids.push_back(found);
    start = end;
  }
  out.insert(out.end(), ids.begin(), ids.end());
}

std::vector<std::int64_t> wordpiece_tokenizer::encode(std::string_view utf8, std::size_t max_len) const {
  std::vector<std::int64_t> ids{cls_};
  const std::size_t room = max_len > 2 ? max_len - 1 : 1;  // [SEP] still fits
  for (const std::string& w : pieces(utf8)) {
    word(w, ids);
    if (ids.size() >= room) break;
  }
  if (ids.size() > room) ids.resize(room);
  ids.push_back(sep_);
  return ids;
}

// ---- the model --------------------------------------------------------------------------------

result<text_spec> read_text_spec(const std::string& folder) {
  MV_TRY(std::string text, read_text(join(folder, "model.json")));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* precision = doc->str("precision");
  const std::string* model = doc->str("model");
  const std::string* vocab = doc->str("vocab");
  const std::string* tokenizer = doc->str("tokenizer");
  const std::string* pooling = doc->str("pooling");
  if (!id || !name || !precision || !model || !vocab || !plain_name(*model) || !plain_name(*vocab) ||
      (tokenizer && *tokenizer != "wordpiece") || (pooling && *pooling != "mean" && *pooling != "cls")) {
    return err(status::corrupt);
  }
  text_spec s;
  s.id = *id;
  s.name = *name;
  s.precision = *precision;
  s.dim = static_cast<std::uint32_t>(doc->integer("dim").value_or(384));
  s.model_file = join(folder, *model);
  s.vocab_file = join(folder, *vocab);
  s.mean_pool = !pooling || *pooling == "mean";
  if (const std::string* q = doc->str("query_prefix")) s.query_prefix = *q;
  if (const std::string* p = doc->str("passage_prefix")) s.passage_prefix = *p;
  s.max_tokens = static_cast<std::uint32_t>(doc->integer("max_tokens").value_or(256));
  s.query_margin = number_or(*doc, "query_margin", s.query_margin);
  s.result_margin = number_or(*doc, "result_margin", s.result_margin);
  s.rank_offset = number_or(*doc, "rank_offset", s.rank_offset);
  if (const json::value* g = doc->find("generic_prompts"); g && g->k == json::kind::array) {
    for (const auto& p : g->a) {
      if (p.k == json::kind::string && !p.s.empty()) s.generic_prompts.push_back(p.s);
    }
  }
  if (s.generic_prompts.empty()) s.generic_prompts = {"a document.", "some text.", "information."};
  if (s.dim == 0 || s.dim > 4096 || s.max_tokens < 8 || s.max_tokens > 512) return err(status::corrupt);
  return s;
}

result<std::unique_ptr<text_model>> text_model::open(const runtime& rt, const text_spec& spec,
                                                     const session_options& options) {
  std::unique_ptr<text_model> m(new text_model());
  m->spec_ = spec;
  MV_TRY(std::string vocab, read_text(spec.vocab_file));
  MV_TRY(wordpiece_tokenizer tok, wordpiece_tokenizer::load(vocab));
  m->tok_ = std::move(tok);
  MV_TRY(auto s, session::open(rt, spec.model_file, options, nullptr));
  m->s_ = std::move(s);
  const auto& names = m->s_->input_names();
  if (std::find(names.begin(), names.end(), "input_ids") == names.end() ||
      std::find(names.begin(), names.end(), "attention_mask") == names.end()) {
    return err(status::corrupt);
  }
  m->type_ids_ = std::find(names.begin(), names.end(), "token_type_ids") != names.end();
  return m;
}

expected text_model::embed(std::span<const std::string> texts, std::string_view prefix,
                           std::vector<float>& out) const {
  out.clear();
  if (texts.empty()) return {};
  std::vector<std::vector<std::int64_t>> enc;
  enc.reserve(texts.size());
  std::size_t width = 0;
  std::string with;
  for (const std::string& t : texts) {
    with.assign(prefix);
    with += t;
    enc.push_back(tok_.encode(with, spec_.max_tokens));
    width = std::max(width, enc.back().size());
  }
  const std::size_t n = texts.size();
  std::vector<std::int64_t> ids(n * width, 0), mask(n * width, 0), types(n * width, 0);
  for (std::size_t i = 0; i < n; ++i) {
    std::copy(enc[i].begin(), enc[i].end(), ids.begin() + static_cast<std::ptrdiff_t>(i * width));
    std::fill_n(mask.begin() + static_cast<std::ptrdiff_t>(i * width), enc[i].size(), 1);
  }
  const std::vector<std::int64_t> shape{static_cast<std::int64_t>(n), static_cast<std::int64_t>(width)};
  std::vector<session::named_input> in;
  in.push_back({"input_ids", shape, session::element::i64, nullptr, ids.data(), nullptr, ids.size()});
  in.push_back({"attention_mask", shape, session::element::i64, nullptr, mask.data(), nullptr, mask.size()});
  if (type_ids_) {
    in.push_back({"token_type_ids", shape, session::element::i64, nullptr, types.data(), nullptr, types.size()});
  }
  MV_TRY(auto outs, s_->run_named(in));
  // last_hidden_state: [n, width, dim].
  const tensor_f32* h = nullptr;
  for (const tensor_f32& t : outs) {
    if (t.shape.size() == 3 && t.shape[0] == static_cast<std::int64_t>(n) &&
        t.shape[1] == static_cast<std::int64_t>(width) && t.shape[2] == spec_.dim) {
      h = &t;
      break;
    }
  }
  if (!h) return err(status::corrupt);
  const std::size_t dim = spec_.dim;
  out.assign(n * dim, 0.0f);
  for (std::size_t i = 0; i < n; ++i) {
    float* v = out.data() + i * dim;
    const float* rows = h->data.data() + i * width * dim;
    if (spec_.mean_pool) {
      const std::size_t len = enc[i].size();
      for (std::size_t t = 0; t < len; ++t) {
        for (std::size_t d = 0; d < dim; ++d) v[d] += rows[t * dim + d];
      }
      for (std::size_t d = 0; d < dim; ++d) v[d] /= static_cast<float>(len);
    } else {
      std::copy(rows, rows + dim, v);
    }
    l2_normalise(std::span<float>(v, dim));
  }
  return {};
}

expected text_model::embed_passages(std::span<const std::string> texts, std::vector<float>& out) const {
  return embed(texts, spec_.passage_prefix, out);
}

result<std::vector<float>> text_model::embed_query(std::string_view utf8) const {
  const std::string q(utf8);
  std::vector<float> out;
  MV_TRY_VOID(embed(std::span<const std::string>(&q, 1), spec_.query_prefix, out));
  return out;
}

}  // namespace mv::infer
