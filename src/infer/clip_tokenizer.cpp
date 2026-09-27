// SPDX-License-Identifier: GPL-2.0-or-later
#include "infer/clip_tokenizer.h"

#include <algorithm>
#include <limits>

#include "core/json.h"

namespace mv::infer {
namespace {

// ---- UTF-8 ------------------------------------------------------------------

// Decodes one code point; malformed bytes decode as U+FFFD, one byte each.
char32_t next_cp(std::string_view s, std::size_t& i) noexcept {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  int len = 0;
  char32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + static_cast<std::size_t>(len) > s.size()) {
    ++i;
    return 0xFFFD;
  }
  for (int k = 1; k < len; ++k) {
    const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
    if ((b & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  i += static_cast<std::size_t>(len);
  return cp;
}

void put_cp(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// ---- classes (\s, \p{L}, \p{N}) for the scripts a first pack meets ----------

bool is_space(char32_t c) noexcept {
  return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
         c == 0x205F || c == 0x3000;
}

bool is_number(char32_t c) noexcept {
  return (c >= '0' && c <= '9') || c == 0xB2 || c == 0xB3 || c == 0xB9 ||
         (c >= 0xBC && c <= 0xBE) || (c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9) ||
         (c >= 0x966 && c <= 0x96F) || (c >= 0xFF10 && c <= 0xFF19) ||
         (c >= 0x2150 && c <= 0x2189) || (c >= 0x2460 && c <= 0x249B);
}

bool is_letter(char32_t c) noexcept {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
  if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;
  if (c >= 0x250 && c <= 0x2C1) return true;
  if (c >= 0x370 && c <= 0x3FF) return c != 0x37E && c != 0x375 && c != 0x384 && c != 0x385 &&
                                       c != 0x387;
  if (c >= 0x400 && c <= 0x52F) return c < 0x482 || c > 0x489;  // Cyrillic minus its marks
  if (c >= 0x531 && c <= 0x587) return true;
  if (c >= 0x5D0 && c <= 0x5EA) return true;
  if (c >= 0x620 && c <= 0x64A) return true;
  if (c >= 0x671 && c <= 0x6D3) return true;
  if (c >= 0x904 && c <= 0x939) return true;  // Devanagari letters
  if (c >= 0xE01 && c <= 0xE30) return true;  // Thai
  if (c >= 0x10A0 && c <= 0x10FF) return true;
  if (c >= 0x1100 && c <= 0x11FF) return true;
  if (c >= 0x1E00 && c <= 0x1FFF) return true;
  if (c >= 0x3041 && c <= 0x3096) return true;
  if (c >= 0x30A1 && c <= 0x30FA) return true;
  if (c >= 0x3400 && c <= 0x4DBF) return true;
  if (c >= 0x4E00 && c <= 0x9FFF) return true;
  if (c >= 0xAC00 && c <= 0xD7A3) return true;
  if (c >= 0xF900 && c <= 0xFAFF) return true;
  if (c >= 0xFF21 && c <= 0xFF3A) return true;
  if (c >= 0xFF41 && c <= 0xFF5A) return true;
  return false;
}

char32_t lower(char32_t c) noexcept {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0x80) return c;
  if ((c >= 0xC0 && c <= 0xDE) && c != 0xD7) return c + 32;
  if (c >= 0x100 && c <= 0x17F) {
    // Latin Extended-A alternates upper/lower, with a shifted stretch.
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 1) ? c + 1 : c;
    if (c == 0x130) return 'i';
    if (c == 0x178) return 0xFF;
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;  // Greek
  if (c >= 0x386 && c <= 0x38F) {
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
  }
  if (c >= 0x410 && c <= 0x42F) return c + 32;  // Cyrillic
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x460 && c <= 0x4FF && c % 2 == 0 && (c < 0x482 || c > 0x489)) return c + 1;
  if (c >= 0x531 && c <= 0x556) return c + 48;  // Armenian
  if (c >= 0x1E00 && c <= 0x1EFF && c % 2 == 0) return c + 1;
  if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
  return c;
}

enum class cls : std::uint8_t { space, letter, number, other };

cls class_of(char32_t c) noexcept {
  if (is_space(c)) return cls::space;
  if (is_letter(c)) return cls::letter;
  if (is_number(c)) return cls::number;
  return cls::other;
}

// The OpenAI/HF pre-tokenizer: 's 't 're 've 'm 'll 'd | letters+ | one digit
// | [^\s\p{L}\p{N}]+. Case is already folded, so the contractions match as-is.
std::size_t contraction_at(std::string_view s, std::size_t i) noexcept {
  if (s[i] != '\'') return 0;
  const std::string_view rest = s.substr(i + 1);
  for (std::string_view c : {"ll", "re", "ve", "s", "t", "m", "d"}) {
    if (rest.substr(0, c.size()) == c) return c.size() + 1;
  }
  return 0;
}

}  // namespace

std::string clip_tokenizer::normalise(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  bool pending_space = false;
  for (std::size_t i = 0; i < utf8.size();) {
    const char32_t c = next_cp(utf8, i);
    if (is_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    put_cp(out, lower(c));
  }
  return out;
}

std::vector<std::string> clip_tokenizer::pieces(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (const std::size_t n = contraction_at(s, i); n) {
      out.emplace_back(s.substr(i, n));
      i += n;
      continue;
    }
    std::size_t j = i;
    const char32_t c = next_cp(s, j);
    const cls k = class_of(c);
    if (k == cls::space) {
      i = j;
      continue;
    }
    if (k == cls::number) {
      out.emplace_back(s.substr(i, j - i));
      i = j;
      continue;
    }
    // A run of letters, or a run of "other" (a contraction starts a new piece).
    std::size_t end = j;
    while (end < s.size()) {
      if (k == cls::other && contraction_at(s, end)) break;
      std::size_t probe = end;
      const char32_t d = next_cp(s, probe);
      if (class_of(d) != k) break;
      end = probe;
    }
    out.emplace_back(s.substr(i, end - i));
    i = end;
  }
  return out;
}

result<clip_tokenizer> clip_tokenizer::load(std::string_view vocab_json, std::string_view merges_txt,
                                            std::uint32_t context) {
  clip_tokenizer t;
  t.context_ = std::max<std::uint32_t>(context, 3);
  // bytes_to_unicode: printable Latin-1 bytes map to themselves, the rest to
  // U+0100 onwards, in byte order.
  {
    int n = 0;
    for (int b = 0; b < 256; ++b) {
      const bool keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
      std::string s;
      put_cp(s, keep ? static_cast<char32_t>(b) : static_cast<char32_t>(256 + n++));
      t.byte_to_unicode_[b] = std::move(s);
    }
  }
  const auto doc = json::parse(vocab_json, 4);
  if (!doc || doc->k != json::kind::object || doc->o.empty()) return err(status::corrupt);
  t.vocab_.reserve(doc->o.size());
  for (const auto& [token, id] : doc->o) {
    if (id.k != json::kind::number || !id.is_integer || id.i < 0) return err(status::corrupt);
    t.vocab_.emplace(token, id.i);
  }
  if (auto it = t.vocab_.find("<|startoftext|>"); it != t.vocab_.end()) t.sot_ = it->second;
  if (auto it = t.vocab_.find("<|endoftext|>"); it != t.vocab_.end()) t.eot_ = it->second;

  std::uint32_t rank = 0;
  std::size_t pos = 0;
  while (pos < merges_txt.size()) {
    std::size_t eol = merges_txt.find('\n', pos);
    if (eol == std::string_view::npos) eol = merges_txt.size();
    std::string_view line = merges_txt.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.rfind("#version", 0) == 0) continue;
    const std::size_t sp = line.find(' ');
    if (sp == std::string_view::npos || sp == 0 || sp + 1 >= line.size()) return err(status::corrupt);
    t.ranks_.emplace(std::string(line), rank++);
  }
  if (t.ranks_.empty()) return err(status::corrupt);
  return t;
}

std::vector<std::string> clip_tokenizer::bpe(const std::string& word) const {
  // Symbols: one per code point, the last carrying the end-of-word mark.
  std::vector<std::string> sym;
  for (std::size_t i = 0; i < word.size();) {
    const std::size_t start = i;
    (void)next_cp(word, i);
    sym.emplace_back(word.substr(start, i - start));
  }
  if (sym.empty()) return sym;
  sym.back() += "</w>";
  while (sym.size() > 1) {
    std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
    std::size_t at = 0;
    for (std::size_t k = 0; k + 1 < sym.size(); ++k) {
      auto it = ranks_.find(sym[k] + ' ' + sym[k + 1]);
      if (it != ranks_.end() && it->second < best) {
        best = it->second;
        at = k;
      }
    }
    if (best == std::numeric_limits<std::uint32_t>::max()) break;
    // Merge every occurrence of the best pair, left to right.
    const std::string a = sym[at], b = sym[at + 1];
    std::vector<std::string> next;
    next.reserve(sym.size());
    for (std::size_t k = 0; k < sym.size();) {
      if (k + 1 < sym.size() && sym[k] == a && sym[k + 1] == b) {
        next.push_back(a + b);
        k += 2;
      } else {
        next.push_back(sym[k]);
        ++k;
      }
    }
    sym.swap(next);
  }
  return sym;
}

std::vector<std::int64_t> clip_tokenizer::encode(std::string_view utf8) const {
  std::vector<std::int64_t> ids{sot_};
  const std::size_t room = context_ - 2;
  std::vector<std::int64_t> body;
  for (const std::string& piece : pieces(normalise(utf8))) {
    std::string mapped;
    for (unsigned char b : piece) mapped += byte_to_unicode_[b];
    for (const std::string& tok : bpe(mapped)) {
      if (auto it = vocab_.find(tok); it != vocab_.end()) body.push_back(it->second);
    }
    if (body.size() >= room) break;
  }
  if (body.size() > room) body.resize(room);
  ids.insert(ids.end(), body.begin(), body.end());
  ids.push_back(eot_);
  return ids;
}

}  // namespace mv::infer
