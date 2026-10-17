// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/passages.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "core/fold.h"
#include "infer/audio_models.h"

namespace mv::ai {
namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// A token that ends a sentence: "end." "it?" "(see p. 3)." "said:" does not.
bool sentence_end(std::string_view token) {
  while (!token.empty() && (token.back() == ')' || token.back() == '"' || token.back() == '\'' ||
                            token.back() == ']')) {
    token.remove_suffix(1);
  }
  return !token.empty() && (token.back() == '.' || token.back() == '!' || token.back() == '?');
}

// One code point at `i` (UTF-8), its length in `n`; a bad byte is itself.
std::uint32_t decode(std::string_view s, std::size_t i, std::size_t& n) {
  const auto b = static_cast<unsigned char>(s[i]);
  if (b < 0x80) {
    n = 1;
    return b;
  }
  const std::size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
  if (len == 1 || i + len > s.size()) {
    n = 1;
    return b;
  }
  std::uint32_t c = b & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
  for (std::size_t k = 1; k < len; ++k) {
    const auto cb = static_cast<unsigned char>(s[i + k]);
    if ((cb & 0xC0) != 0x80) {
      n = 1;
      return b;
    }
    c = (c << 6) | (cb & 0x3F);
  }
  n = len;
  return c;
}

}  // namespace

std::string plain_text(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    const std::uint32_t c = decode(s, i, n);
    const char* plain = nullptr;
    switch (c) {
      case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: plain = "'"; break;
      case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: plain = "\""; break;
      case 0x2010: case 0x2011: case 0x2012: plain = "-"; break;
      case 0x2013: case 0x2014: case 0x2015: case 0x2212: plain = " - "; break;
      case 0x2026: plain = "..."; break;
      case 0x2022: case 0x25CF: case 0x25E6: case 0x2023: plain = " "; break;  // bullets
      case 0x00A0: case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006: case 0x2007:
      case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F: case 0x3000: case 0x2028:
      case 0x2029: plain = " "; break;
      case 0xFB00: plain = "ff"; break;
      case 0xFB01: plain = "fi"; break;
      case 0xFB02: plain = "fl"; break;
      case 0xFB03: plain = "ffi"; break;
      case 0xFB04: plain = "ffl"; break;
      case 0xFB05: case 0xFB06: plain = "st"; break;
      case 0x00AD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: plain = ""; break;  // invisible
      default: break;
    }
    if (plain) {
      out += plain;
    } else if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
      out.push_back(' ');  // a control character: a gap, never part of a word
    } else {
      out.append(s.substr(i, n));
    }
    i += n;
  }
  return out;
}

std::vector<std::string> match_words(std::string_view utf8) {
  return infer::speech_words(text::fold(plain_text(utf8)));
}

std::vector<std::string> pdf_paragraphs(std::string_view page) {
  std::vector<std::string> out;
  std::string cur;
  std::size_t i = 0;
  while (i < page.size()) {
    // One line, without its "\r\n" / "\n" / "\r".
    std::size_t end = i;
    while (end < page.size() && page[end] != '\r' && page[end] != '\n') ++end;
    std::string_view line = page.substr(i, end - i);
    i = end;
    if (i < page.size() && page[i] == '\r') ++i;
    if (i < page.size() && page[i] == '\n') ++i;
    while (!line.empty() && space(line.front())) line.remove_prefix(1);
    while (!line.empty() && space(line.back())) line.remove_suffix(1);
    if (line.empty()) {
      // A blank line: the paragraph ends.
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
      continue;
    }
    if (!cur.empty()) {
      // "infor-" / "mation": one word again, when the break split a word (a
      // letter before the hyphen, a lower-case letter after it). "e-" / "mail"
      // becomes "email", as most readers would search it.
      const bool hyphen = cur.size() >= 2 && cur.back() == '-' &&
                          std::isalpha(static_cast<unsigned char>(cur[cur.size() - 2])) &&
                          line.front() >= 'a' && line.front() <= 'z';
      if (hyphen) {
        cur.pop_back();
      } else {
        cur.push_back(' ');
      }
    }
    cur.append(line);
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

void passage_builder::add(std::uint32_t page, std::string_view text, bool block_end) {
  if (full()) return;
  if (page != page_ && words_ > 0) close();
  page_ = page;
  const std::string plain = plain_text(text);
  std::size_t i = 0;
  while (i < plain.size() && !full()) {
    while (i < plain.size() && space(plain[i])) ++i;
    if (i >= plain.size()) break;
    std::size_t end = i;
    while (end < plain.size() && !space(plain[end])) ++end;
    const std::string_view token(plain.data() + i, end - i);
    i = end;
    if (!text_.empty()) text_.push_back(' ');
    text_.append(token);
    ++words_;
    if ((words_ >= kPassageWords && sentence_end(token)) || words_ >= kPassageMaxWords) close();
  }
  // A paragraph's end closes a passage that is long enough to stand alone; a
  // heading or a label joins what follows it.
  if (block_end && words_ >= kPassageWords / 2) close();
}

void passage_builder::finish() {
  if (words_ > 0) close();
}

void passage_builder::close() {
  if (words_ == 0 || full()) {
    text_.clear();
    words_ = 0;
    return;
  }
  passage p;
  p.page = page_;
  std::vector<std::string> own = match_words(text_);
  for (const std::string& w : lead_) {
    if (!p.words.empty()) p.words.push_back(' ');
    p.words += w;
  }
  for (const std::string& w : own) {
    if (!p.words.empty()) p.words.push_back(' ');
    p.words += w;
  }
  p.text = std::move(text_);
  // The lead-in of the next passage: this one's last words.
  const std::size_t keep = std::min(own.size(), kPassageLeadWords);
  lead_.assign(std::make_move_iterator(own.end() - static_cast<std::ptrdiff_t>(keep)),
               std::make_move_iterator(own.end()));
  if (!p.words.empty()) {
    done_.push_back(std::move(p));
    ++made_;
  }
  text_.clear();
  words_ = 0;
}

std::vector<passage> passage_builder::take() {
  std::vector<passage> out;
  out.swap(done_);
  return out;
}

}  // namespace mv::ai
