// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A lossless XML tree for FCPXML round trips (docs/design/29 "Lossless").
//
// Final Cut Pro hands over a project as FCPXML and takes a new version back
// the same way. Everything MediaViewer does not edit must come back exactly as
// FCP wrote it: effects, colour, keyframes, metadata, element kinds a newer
// FCP invents. So the tree keeps the source text and every node remembers its
// span in it. Writing copies the original bytes of every node that was not
// changed (and holds no changed node); only a changed start tag, a changed
// text node or a changed child list is written afresh, and inside a changed
// start tag every attribute that was not changed keeps its original bytes
// (quotes, spacing, entity spelling). Parse then write with no change gives
// the input back byte for byte.
//
// Portable C++, no XML library, no exceptions (an allocation failure aside).
// Not a validating parser: it reads well-formed XML (elements, attributes,
// text, CDATA, comments, processing instructions, a DOCTYPE with or without
// an internal subset) and refuses anything else with a byte offset. Entities
// other than the five predefined ones and character references are refused:
// FCP writes none, and expanding a declared entity is how a small file
// becomes a large one.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::nle::xml {

enum class node_kind : std::uint8_t {
  document,  // node 0: the prolog, the root element and anything after it
  element,
  text,      // character data between tags (decoded in `text`)
  cdata,     // <![CDATA[...]]> (its content in `text`)
  other,     // comment, processing instruction, XML declaration, DOCTYPE: kept as bytes
};

struct attribute {
  std::string name;
  std::string value;  // decoded
  // The original bytes, from the whitespace before the name through the
  // closing quote. Empty for an attribute that was added or changed.
  std::string raw;
};

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

struct node {
  node_kind kind = node_kind::element;
  std::string name;  // element name
  std::vector<attribute> attrs;
  std::vector<std::uint32_t> children;
  std::uint32_t parent = kNone;
  std::string text;  // text / cdata content, decoded

  // Where the node came from in document::source. A node made by the program
  // (not parsed, not cloned) has from_source false and is always written afresh.
  bool from_source = false;
  bool self_closing = false;
  std::uint32_t begin = 0;        // first byte of the node
  std::uint32_t open_end = 0;     // element: one past the start tag's '>'
  std::uint32_t close_begin = 0;  // element: the end tag's '<' (== end when self-closing)
  std::uint32_t end = 0;          // one past the node's last byte
  std::string tag_tail;           // element: bytes after the last attribute up to and incl. '>' / "/>"

  // Changed by the program: the start tag or text (`dirty`), or something
  // below it (`below_dirty`: a child list, a descendant). Set by document's
  // editing calls; a node with neither flag writes its source bytes.
  bool dirty = false;
  bool below_dirty = false;
};

struct parse_error {
  std::uint32_t offset = 0;
  std::string message;
};

class document {
 public:
  // Parses `source` (UTF-8). On failure returns false and fills `error`; the
  // document is then empty.
  [[nodiscard]] bool parse(std::string source, parse_error* error);

  // The document as text. With no edit since parse(), exactly the source.
  [[nodiscard]] std::string write() const;
  // One node (and everything under it) as text.
  [[nodiscard]] std::string write(std::uint32_t node) const;

  [[nodiscard]] const std::string& source() const noexcept { return source_; }
  [[nodiscard]] std::uint32_t root() const noexcept { return root_; }  // the document element, or kNone
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] const node& at(std::uint32_t i) const { return nodes_[i]; }

  // ---- reading ----
  [[nodiscard]] const std::string* attr(std::uint32_t element, std::string_view name) const;
  [[nodiscard]] std::string_view attr_or(std::uint32_t element, std::string_view name,
                                         std::string_view fallback) const;
  // Element children, optionally only those named `name`.
  [[nodiscard]] std::vector<std::uint32_t> elements(std::uint32_t parent, std::string_view name = {}) const;
  [[nodiscard]] std::uint32_t first(std::uint32_t parent, std::string_view name) const;  // kNone if none
  // The concatenated text and CDATA under a node.
  [[nodiscard]] std::string text_of(std::uint32_t node) const;
  // True when `node` sits under `ancestor` (or is it).
  [[nodiscard]] bool within(std::uint32_t node, std::uint32_t ancestor) const;

  // ---- editing (each marks what it touches, so write() rewrites only that) ----
  void set_attr(std::uint32_t element, std::string_view name, std::string_view value);
  // True when the attribute was there.
  bool remove_attr(std::uint32_t element, std::string_view name);
  // Replaces the text under an element with one text node.
  void set_text(std::uint32_t element, std::string_view text);
  // Detaches a node from its parent (it stays in the table, unreachable).
  void remove(std::uint32_t node);
  // Inserts `node` (detached) into `parent` before `before` (kNone = at the end).
  void insert(std::uint32_t parent, std::uint32_t node, std::uint32_t before = kNone);
  // A detached deep copy of a node; unchanged copies write their source bytes.
  [[nodiscard]] std::uint32_t clone(std::uint32_t node);
  // A new detached element with no attributes or children.
  [[nodiscard]] std::uint32_t make_element(std::string_view name);
  // A new detached text node (`text` is plain, escaped when written).
  [[nodiscard]] std::uint32_t make_text(std::string_view text);

  // The whitespace in front of a node, if the text node just before it is only
  // whitespace (what an inserted sibling copies to keep the indentation).
  [[nodiscard]] std::string indent_before(std::uint32_t node) const;

 private:
  void touch(std::uint32_t node);  // mark node dirty and its ancestors below_dirty
  void touch_below(std::uint32_t node);
  void write_into(std::uint32_t node, std::string& out) const;
  [[nodiscard]] std::uint32_t clone_into(std::uint32_t node, std::uint32_t parent);

  std::string source_;
  std::vector<node> nodes_;
  std::uint32_t root_ = kNone;
};

// Escaping for written values: attribute values escape & < > " and the
// whitespace that attribute normalisation would fold (\t \n \r); text escapes
// & < > (and \r, which a parser would otherwise drop).
[[nodiscard]] std::string escape_attr(std::string_view s);
[[nodiscard]] std::string escape_text(std::string_view s);

}  // namespace mv::nle::xml
