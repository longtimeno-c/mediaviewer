// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 — a document's text as passages (docs/plans/document-search.md §3,
// docs/design/17 "Documents"): a few sentences each, never across a page, so a
// match names the page that shows it and its snippet reads as a sentence.
//
// Each passage carries two texts: `text` as written (whitespace tidied,
// typographic punctuation and ligatures made plain), the snippet; and `words`,
// the folded words search matches (core/fold.h, then the speech rule), led by
// the last few words of the passage before it, so a quoted phrase that runs
// over a passage break is still one phrase. Pure C++: no SQLite, no PDFium.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::ai {

// Budgets (docs/design/17 "Documents"): passages per document, and their size
// in words (a passage closes at a sentence end past kPassageWords, and at
// kPassageMaxWords wherever it is).
inline constexpr std::size_t kMaxPassages = 20000;
inline constexpr std::size_t kPassageWords = 40;
inline constexpr std::size_t kPassageMaxWords = 80;
// Words of the passage before that lead a passage's `words`.
inline constexpr std::size_t kPassageLeadWords = 8;

struct passage {
  std::uint32_t page = 0;
  std::string text;
  std::string words;
};

class passage_builder {
 public:
  // Text that starts on `page`. `block_end`: a paragraph ends with it (a DOCX
  // paragraph); a short one (a heading) still joins the next on its page.
  void add(std::uint32_t page, std::string_view text, bool block_end = true);
  // Closes the passage in hand (the document's end).
  void finish();
  // The passages closed since the last take, in order.
  [[nodiscard]] std::vector<passage> take();
  // kMaxPassages made: what follows is dropped.
  [[nodiscard]] bool full() const noexcept { return made_ >= kMaxPassages; }
  [[nodiscard]] std::size_t made() const noexcept { return made_; }

 private:
  void close();
  std::uint32_t page_ = 0;
  std::string text_;
  std::size_t words_ = 0;
  std::vector<std::string> lead_;  // the last folded words of the passage before
  std::vector<passage> done_;
  std::size_t made_ = 0;
};

// A PDF page's text (PDFium's lines end in "\r\n") as running prose: a line
// break is a space, a word hyphenated over one is joined again, and a blank
// line ends a paragraph.
[[nodiscard]] std::vector<std::string> pdf_paragraphs(std::string_view page_text);

// Typographic punctuation (curly quotes, dashes, ellipsis, no-break and other
// spaces) and the Latin ligatures as their plain forms; soft hyphens and
// control characters dropped.
[[nodiscard]] std::string plain_text(std::string_view utf8);

// The words search matches in `utf8`: folded (core/fold.h), then split by the
// speech rule (infer::speech_words). The query's words go through it too.
[[nodiscard]] std::vector<std::string> match_words(std::string_view utf8);

}  // namespace mv::ai
