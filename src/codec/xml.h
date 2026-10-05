// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A pull reader for the XML in an Office Open XML package (DOCX,
// docs/plans/audio-and-documents.md §2.5). Elements, attributes and text, with
// the five predefined entities and numeric character references decoded.
// Comments and processing instructions are skipped; CDATA is text. A DOCTYPE
// is refused outright, so no entity is ever expanded (no "billion laughs").
// Not a validating parser: it reads well-formed documents and stops with
// `error` at the first thing it does not understand.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::codec::xml {

enum class token { start, end, text, eof, error };

class reader {
 public:
  explicit reader(std::string_view doc) noexcept : doc_(doc) {}

  // Advances to the next token. A self-closing element is a start followed by
  // its end. Whitespace-only text between elements is still reported.
  token next();

  // The element's qualified name ("w:p") for start and end.
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  // The part after the prefix ("p").
  [[nodiscard]] std::string_view local() const noexcept;
  // An attribute of the current start element by qualified name; empty when
  // absent. `has` tells absent from empty.
  [[nodiscard]] std::string_view attr(std::string_view qname) const noexcept;
  [[nodiscard]] bool has(std::string_view qname) const noexcept;
  // Decoded text for a text token.
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  // Element depth after this token (a start counts itself).
  [[nodiscard]] int depth() const noexcept { return depth_; }

  // Skips the rest of the element whose start was just read.
  void skip_element();

 private:
  bool decode(std::string_view raw, std::string& out) const;
  // After '<'. `eof` means a comment or processing instruction was skipped.
  token read_tag();

  std::string_view doc_;
  std::size_t at_ = 0;
  std::string_view name_;
  std::string text_;
  struct attribute {
    std::string_view name;
    std::string value;
  };
  std::vector<attribute> attrs_;
  bool pending_end_ = false;
  int depth_ = 0;
};

// The part of a qualified name after its prefix.
[[nodiscard]] std::string_view local_name(std::string_view qname) noexcept;

}  // namespace mv::codec::xml
