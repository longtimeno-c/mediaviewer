// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/xml.h"

#include <algorithm>
#include <cstring>

namespace mv::nle::xml {
namespace {

constexpr std::size_t kMaxSource = std::size_t{1} << 30;  // 1 GiB: offsets are 32-bit
constexpr std::size_t kMaxDepth = 1024;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool is_name_char(char c) { return !is_space(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != '\0'; }

bool all_space(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return is_space(c); });
}

void put_utf8(std::uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decodes the five predefined entities and character references. False on
// anything else (a declared entity, a bare '&').
bool decode(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c != '&') {
      out += c;
      continue;
    }
    const std::size_t semi = in.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) return false;
    const std::string_view ref = in.substr(i + 1, semi - i - 1);
    if (ref == "amp") {
      out += '&';
    } else if (ref == "lt") {
      out += '<';
    } else if (ref == "gt") {
      out += '>';
    } else if (ref == "quot") {
      out += '"';
    } else if (ref == "apos") {
      out += '\'';
    } else if (ref.size() >= 2 && ref[0] == '#') {
      const bool hex = ref[1] == 'x';
      const std::string_view digits = ref.substr(hex ? 2 : 1);
      if (digits.empty()) return false;
      std::uint32_t cp = 0;
      for (const char d : digits) {
        std::uint32_t v;
        if (d >= '0' && d <= '9') {
          v = static_cast<std::uint32_t>(d - '0');
        } else if (hex && d >= 'a' && d <= 'f') {
          v = static_cast<std::uint32_t>(d - 'a' + 10);
        } else if (hex && d >= 'A' && d <= 'F') {
          v = static_cast<std::uint32_t>(d - 'A' + 10);
        } else {
          return false;
        }
        cp = cp * (hex ? 16u : 10u) + v;
        if (cp > 0x10FFFF) return false;
      }
      if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
      put_utf8(cp, out);
    } else {
      return false;
    }
    i = semi;
  }
  return true;
}

struct parser {
  const std::string& s;
  std::vector<node>& nodes;
  std::size_t pos = 0;
  parse_error* error = nullptr;

  bool fail(std::size_t at, const char* message) {
    if (error) {
      error->offset = static_cast<std::uint32_t>(at);
      error->message = message;
    }
    return false;
  }

  bool starts(std::string_view lit) const { return s.compare(pos, lit.size(), lit) == 0; }

  std::uint32_t add(node n, std::uint32_t parent) {
    n.parent = parent;
    n.from_source = true;
    const auto id = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    nodes[parent].children.push_back(id);
    return id;
  }

  // A raw node ending just after `close` (searched from `from`).
  bool raw(node_kind kind, std::string_view close, std::size_t from, std::uint32_t parent, const char* what) {
    const std::size_t at = s.find(close, from);
    if (at == std::string::npos) return fail(pos, what);
    node n;
    n.kind = kind;
    n.begin = static_cast<std::uint32_t>(pos);
    n.end = static_cast<std::uint32_t>(at + close.size());
    if (kind == node_kind::cdata) n.text = s.substr(from, at - from);
    add(std::move(n), parent);
    pos = at + close.size();
    return true;
  }

  bool doctype(std::uint32_t parent) {
    const std::size_t start = pos;
    std::size_t i = pos + 9;  // after "<!DOCTYPE"
    int brackets = 0;
    char quote = 0;
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '[') {
        ++brackets;
      } else if (c == ']') {
        --brackets;
      } else if (c == '>' && brackets <= 0) {
        break;
      }
    }
    if (i >= s.size()) return fail(start, "unterminated DOCTYPE");
    node n;
    n.kind = node_kind::other;
    n.begin = static_cast<std::uint32_t>(start);
    n.end = static_cast<std::uint32_t>(i + 1);
    add(std::move(n), parent);
    pos = i + 1;
    return true;
  }

  bool run(std::uint32_t& root) {
    nodes.clear();
    node doc;
    doc.kind = node_kind::document;
    doc.from_source = true;
    doc.end = static_cast<std::uint32_t>(s.size());
    nodes.push_back(std::move(doc));
    std::vector<std::uint32_t> open;  // element stack
    root = kNone;
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) {
      node bom;
      bom.kind = node_kind::other;
      bom.begin = 0;
      bom.end = 3;
      add(std::move(bom), 0);
      pos = 3;
    }
    while (pos < s.size()) {
      const std::uint32_t parent = open.empty() ? 0 : open.back();
      if (s[pos] != '<') {
        const std::size_t lt = s.find('<', pos);
        const std::size_t end = lt == std::string::npos ? s.size() : lt;
        node t;
        t.kind = node_kind::text;
        t.begin = static_cast<std::uint32_t>(pos);
        t.end = static_cast<std::uint32_t>(end);
        const std::string_view raw_text(s.data() + pos, end - pos);
        if (open.empty() && !all_space(raw_text)) return fail(pos, "text outside the document element");
        if (!decode(raw_text, t.text)) return fail(pos, "unknown entity or bare '&' in text");
        add(std::move(t), parent);
        pos = end;
        continue;
      }
      if (starts("<!--")) {
        if (!raw(node_kind::other, "-->", pos + 4, parent, "unterminated comment")) return false;
        continue;
      }
      if (starts("<![CDATA[")) {
        if (open.empty()) return fail(pos, "CDATA outside the document element");
        if (!raw(node_kind::cdata, "]]>", pos + 9, parent, "unterminated CDATA")) return false;
        continue;
      }
      if (starts("<!DOCTYPE")) {
        if (!open.empty() || root != kNone) return fail(pos, "DOCTYPE inside the document");
        if (!doctype(parent)) return false;
        continue;
      }
      if (starts("<?")) {
        if (!raw(node_kind::other, "?>", pos + 2, parent, "unterminated processing instruction")) return false;
        continue;
      }
      if (starts("<!")) return fail(pos, "unsupported markup declaration");
      if (starts("</")) {
        if (open.empty()) return fail(pos, "end tag with no open element");
        std::size_t i = pos + 2;
        const std::size_t name_start = i;
        while (i < s.size() && is_name_char(s[i])) ++i;
        const std::string_view name(s.data() + name_start, i - name_start);
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size() || s[i] != '>') return fail(pos, "malformed end tag");
        node& e = nodes[open.back()];
        if (name != e.name) return fail(pos, "end tag does not match the open element");
        e.close_begin = static_cast<std::uint32_t>(pos);
        e.end = static_cast<std::uint32_t>(i + 1);
        open.pop_back();
        pos = i + 1;
        continue;
      }
      // A start tag.
      if (open.empty() && root != kNone) return fail(pos, "a second document element");
      if (open.size() >= kMaxDepth) return fail(pos, "elements nested too deep");
      node e;
      e.kind = node_kind::element;
      e.begin = static_cast<std::uint32_t>(pos);
      std::size_t i = pos + 1;
      const std::size_t name_start = i;
      while (i < s.size() && is_name_char(s[i])) ++i;
      if (i == name_start) return fail(pos, "missing element name");
      e.name.assign(s, name_start, i - name_start);
      for (;;) {
        const std::size_t ws = i;
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size()) return fail(pos, "unterminated start tag");
        if (s[i] == '>' || (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>')) {
          e.self_closing = s[i] == '/';
          const std::size_t close = i + (e.self_closing ? 2 : 1);
          e.tag_tail.assign(s, ws, close - ws);
          e.open_end = static_cast<std::uint32_t>(close);
          i = close;
          break;
        }
        if (i == ws) return fail(i, "attributes must be separated by whitespace");
        const std::size_t an = i;
        while (i < s.size() && is_name_char(s[i])) ++i;
        if (i == an) return fail(i, "malformed attribute");
        attribute a;
        a.name.assign(s, an, i - an);
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size() || s[i] != '=') return fail(i, "attribute without a value");
        ++i;
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return fail(i, "attribute value must be quoted");
        const char q = s[i];
        const std::size_t vs = i + 1;
        const std::size_t ve = s.find(q, vs);
        if (ve == std::string::npos) return fail(i, "unterminated attribute value");
        const std::string_view rawv(s.data() + vs, ve - vs);
        if (rawv.find('<') != std::string_view::npos) return fail(vs, "'<' in an attribute value");
        if (!decode(rawv, a.value)) return fail(vs, "unknown entity or bare '&' in an attribute");
        for (const attribute& other : e.attrs) {
          if (other.name == a.name) return fail(an, "duplicate attribute");
        }
        a.raw.assign(s, ws, ve + 1 - ws);
        e.attrs.push_back(std::move(a));
        i = ve + 1;
      }
      const bool self_closing = e.self_closing;
      if (self_closing) {
        e.close_begin = static_cast<std::uint32_t>(i);
        e.end = static_cast<std::uint32_t>(i);
      }
      const std::uint32_t id = add(std::move(e), parent);
      if (parent == 0) root = id;
      if (!self_closing) open.push_back(id);
      pos = i;
    }
    if (!open.empty()) return fail(nodes[open.back()].begin, "element not closed");
    if (root == kNone) return fail(0, "no document element");
    return true;
  }
};

}  // namespace

std::string escape_attr(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\t': out += "&#9;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string escape_text(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\r': out += "&#13;"; break;
      default: out += c;
    }
  }
  return out;
}

bool document::parse(std::string source, parse_error* error) {
  source_ = std::move(source);
  nodes_.clear();
  root_ = kNone;
  if (source_.size() >= kMaxSource) {
    if (error) *error = {0, "document too large"};
    source_.clear();
    return false;
  }
  parser p{source_, nodes_, 0, error};
  if (!p.run(root_)) {
    nodes_.clear();
    source_.clear();
    root_ = kNone;
    return false;
  }
  return true;
}

void document::write_into(std::uint32_t id, std::string& out) const {
  const node& n = nodes_[id];
  if (n.from_source && !n.dirty && !n.below_dirty) {
    out.append(source_, n.begin, n.end - n.begin);
    return;
  }
  switch (n.kind) {
    case node_kind::document:
      for (const std::uint32_t c : n.children) write_into(c, out);
      return;
    case node_kind::text:
      out += escape_text(n.text);
      return;
    case node_kind::cdata: {
      out += "<![CDATA[";
      std::string_view t = n.text;
      for (std::size_t at; (at = t.find("]]>")) != std::string_view::npos;) {
        out.append(t.substr(0, at + 2));
        out += "]]><![CDATA[";
        t.remove_prefix(at + 2);
      }
      out.append(t);
      out += "]]>";
      return;
    }
    case node_kind::other:
      if (n.from_source) out.append(source_, n.begin, n.end - n.begin);
      return;
    case node_kind::element:
      break;
  }
  const bool empty = n.children.empty();
  // The start tag, up to (not including) its closing "/>" or ">".
  std::string tail = n.from_source ? n.tag_tail : std::string(empty ? "/>" : ">");
  if (n.from_source && !n.dirty) {
    out.append(source_, n.begin, n.open_end - n.begin - n.tag_tail.size());
  } else {
    out += '<';
    out += n.name;
    for (const attribute& a : n.attrs) {
      if (!a.raw.empty()) {
        out += a.raw;
      } else {
        out += ' ';
        out += a.name;
        out += "=\"";
        out += escape_attr(a.value);
        out += '"';
      }
    }
  }
  const bool writes_self_closing = n.from_source ? (n.self_closing && empty) : empty;
  if (n.from_source && n.self_closing && !empty) {
    // "<x a='1'/>" that now holds children: "<x a='1'>...</x>".
    tail.erase(tail.size() - 2);
    tail += '>';
  }
  out += tail;
  if (writes_self_closing) return;
  for (const std::uint32_t c : n.children) write_into(c, out);
  if (n.from_source && !n.self_closing) {
    out.append(source_, n.close_begin, n.end - n.close_begin);
  } else {
    out += "</";
    out += n.name;
    out += '>';
  }
}

std::string document::write() const {
  std::string out;
  if (nodes_.empty()) return out;
  out.reserve(source_.size() + 256);
  write_into(0, out);
  return out;
}

std::string document::write(std::uint32_t id) const {
  std::string out;
  if (id < nodes_.size()) write_into(id, out);
  return out;
}

const std::string* document::attr(std::uint32_t element, std::string_view name) const {
  if (element >= nodes_.size()) return nullptr;
  for (const attribute& a : nodes_[element].attrs) {
    if (a.name == name) return &a.value;
  }
  return nullptr;
}

std::string_view document::attr_or(std::uint32_t element, std::string_view name, std::string_view fallback) const {
  const std::string* v = attr(element, name);
  return v ? std::string_view(*v) : fallback;
}

std::vector<std::uint32_t> document::elements(std::uint32_t parent, std::string_view name) const {
  std::vector<std::uint32_t> out;
  if (parent >= nodes_.size()) return out;
  for (const std::uint32_t c : nodes_[parent].children) {
    const node& n = nodes_[c];
    if (n.kind == node_kind::element && (name.empty() || n.name == name)) out.push_back(c);
  }
  return out;
}

std::uint32_t document::first(std::uint32_t parent, std::string_view name) const {
  if (parent >= nodes_.size()) return kNone;
  for (const std::uint32_t c : nodes_[parent].children) {
    const node& n = nodes_[c];
    if (n.kind == node_kind::element && n.name == name) return c;
  }
  return kNone;
}

std::string document::text_of(std::uint32_t id) const {
  std::string out;
  if (id >= nodes_.size()) return out;
  std::vector<std::uint32_t> stack{id};
  // Depth first, children in order.
  while (!stack.empty()) {
    const std::uint32_t c = stack.back();
    stack.pop_back();
    const node& n = nodes_[c];
    if (n.kind == node_kind::text || n.kind == node_kind::cdata) out += n.text;
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

bool document::within(std::uint32_t id, std::uint32_t ancestor) const {
  for (std::uint32_t c = id; c != kNone && c < nodes_.size(); c = nodes_[c].parent) {
    if (c == ancestor) return true;
  }
  return false;
}

void document::touch(std::uint32_t id) {
  nodes_[id].dirty = true;
  touch_below(nodes_[id].parent);
}

void document::touch_below(std::uint32_t id) {
  for (std::uint32_t c = id; c != kNone && c < nodes_.size(); c = nodes_[c].parent) {
    if (nodes_[c].below_dirty && c != id) break;  // already marked up to the top
    nodes_[c].below_dirty = true;
  }
}

void document::set_attr(std::uint32_t element, std::string_view name, std::string_view value) {
  node& n = nodes_[element];
  for (attribute& a : n.attrs) {
    if (a.name == name) {
      if (a.value == value) return;
      a.value.assign(value);
      a.raw.clear();
      touch(element);
      return;
    }
  }
  n.attrs.push_back({std::string(name), std::string(value), {}});
  touch(element);
}

bool document::remove_attr(std::uint32_t element, std::string_view name) {
  node& n = nodes_[element];
  const auto it = std::find_if(n.attrs.begin(), n.attrs.end(), [&](const attribute& a) { return a.name == name; });
  if (it == n.attrs.end()) return false;
  n.attrs.erase(it);
  touch(element);
  return true;
}

void document::set_text(std::uint32_t element, std::string_view text) {
  // Keep a single text child where there was one (indentation-free text such
  // as a title's <text>), else replace all character data.
  node& n = nodes_[element];
  std::vector<std::uint32_t> kept;
  for (const std::uint32_t c : n.children) {
    const node_kind k = nodes_[c].kind;
    if (k == node_kind::text || k == node_kind::cdata) {
      nodes_[c].parent = kNone;
    } else {
      kept.push_back(c);
    }
  }
  const std::uint32_t t = make_text(text);
  nodes_[t].parent = element;
  nodes_[element].children = std::move(kept);
  nodes_[element].children.insert(nodes_[element].children.begin(), t);
  touch_below(element);
}

void document::remove(std::uint32_t id) {
  const std::uint32_t parent = nodes_[id].parent;
  if (parent == kNone) return;
  auto& kids = nodes_[parent].children;
  const auto it = std::find(kids.begin(), kids.end(), id);
  if (it == kids.end()) return;
  // Take the indentation in front of it too, so the file keeps its shape.
  auto first = it;
  if (it != kids.begin()) {
    const node& prev = nodes_[*(it - 1)];
    if (prev.kind == node_kind::text && all_space(prev.text)) first = it - 1;
  }
  for (auto j = first; j != it + 1; ++j) nodes_[*j].parent = kNone;
  kids.erase(first, it + 1);
  touch_below(parent);
}

std::string document::indent_before(std::uint32_t id) const {
  const std::uint32_t parent = nodes_[id].parent;
  if (parent == kNone) return {};
  const auto& kids = nodes_[parent].children;
  const auto it = std::find(kids.begin(), kids.end(), id);
  if (it == kids.end() || it == kids.begin()) return {};
  const node& prev = nodes_[*(it - 1)];
  if (prev.kind != node_kind::text || !all_space(prev.text)) return {};
  // Only the last line's indentation.
  const std::size_t nl = prev.text.rfind('\n');
  return nl == std::string::npos ? prev.text : prev.text.substr(nl);
}

void document::insert(std::uint32_t parent, std::uint32_t id, std::uint32_t before) {
  auto& kids = nodes_[parent].children;
  auto at = before == kNone ? kids.end() : std::find(kids.begin(), kids.end(), before);
  // Copy the indentation of the sibling it goes next to.
  std::string indent;
  if (at != kids.end()) {
    indent = indent_before(*at);
  } else {
    for (auto r = kids.rbegin(); r != kids.rend(); ++r) {
      if (nodes_[*r].kind == node_kind::element) {
        indent = indent_before(*r);
        break;
      }
    }
  }
  const auto pos = static_cast<std::size_t>(at - kids.begin());
  // Before a sibling: the node, then the sibling's indentation again (the
  // whitespace already in front of the sibling now sits in front of the node).
  // At the end: the indentation, then the node.
  std::vector<std::uint32_t> add;
  const std::uint32_t t = indent.empty() ? kNone : make_text(indent);
  if (t != kNone && before == kNone) add.push_back(t);
  add.push_back(id);
  if (t != kNone && before != kNone) add.push_back(t);
  // make_text may have grown nodes_: re-take the reference.
  auto& kids2 = nodes_[parent].children;
  if (before == kNone && !indent.empty() && !kids2.empty()) {
    // Appending after the last element: the text after it (the closing tag's
    // indentation) stays last.
    std::size_t last_el = kids2.size();
    for (std::size_t k = kids2.size(); k-- > 0;) {
      if (nodes_[kids2[k]].kind == node_kind::element) {
        last_el = k;
        break;
      }
    }
    const std::size_t where = last_el == kids2.size() ? kids2.size() : last_el + 1;
    kids2.insert(kids2.begin() + static_cast<std::ptrdiff_t>(where), add.begin(), add.end());
  } else {
    kids2.insert(kids2.begin() + static_cast<std::ptrdiff_t>(pos), add.begin(), add.end());
  }
  for (const std::uint32_t a : add) nodes_[a].parent = parent;
  touch_below(parent);
}

std::uint32_t document::clone_into(std::uint32_t id, std::uint32_t parent) {
  node copy = nodes_[id];
  copy.parent = parent;
  copy.children.clear();
  const auto me = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(std::move(copy));
  const std::vector<std::uint32_t> kids = nodes_[id].children;
  for (const std::uint32_t c : kids) {
    const std::uint32_t k = clone_into(c, me);
    nodes_[me].children.push_back(k);
  }
  return me;
}

std::uint32_t document::clone(std::uint32_t id) { return clone_into(id, kNone); }

std::uint32_t document::make_element(std::string_view name) {
  node n;
  n.kind = node_kind::element;
  n.name.assign(name);
  nodes_.push_back(std::move(n));
  return static_cast<std::uint32_t>(nodes_.size() - 1);
}

std::uint32_t document::make_text(std::string_view text) {
  node n;
  n.kind = node_kind::text;
  n.text.assign(text);
  nodes_.push_back(std::move(n));
  return static_cast<std::uint32_t>(nodes_.size() - 1);
}

}  // namespace mv::nle::xml
