// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: the XMP packet read as RDF, so that a property Exiv2 did not list (an
// unregistered namespace it could not key, a struct it flattened) is shown raw
// instead of dropped (docs/design/06 "Beyond Exiv2").
//
// RDF/XML as XMP writes it: rdf:Description elements whose attributes and child
// elements are properties; a property is text, an rdf:resource, a struct
// (rdf:parseType="Resource", a nested rdf:Description, or attributes on the
// element), or an rdf:Bag / Seq / Alt of rdf:li items, each of which can be any
// of these again. Leaves become rows. The XML reader refuses DOCTYPE (no entity
// expansion); depth, rows and the packet size are capped.
#include <string>
#include <vector>

#include "codec/xml.h"
#include "meta/parse.h"

namespace mv::meta::parse {
namespace {

using codec::xml::reader;
using codec::xml::token;

constexpr std::size_t kMaxPacket = 4u << 20;
constexpr int kMaxNest = 24;
constexpr std::size_t kMaxRows = 2000;
// Namespaces in scope at once (real packets declare a few dozen), and the total
// prefix comparisons one packet may cost (~tens of ms), so a hostile packet of
// thousands of declarations over every element is bounded (#290 review: 4 s).
constexpr std::size_t kMaxScope = 256;
constexpr std::size_t kMaxLookupWork = 32'000'000;

constexpr std::string_view kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view kXmpNs = "http://ns.adobe.com/xap/1.0/";
constexpr std::string_view kIptcExt = "http://iptc.org/std/Iptc4xmpExt/2008-02-29/";

struct ns {
  std::string prefix, uri;
  int depth;
};

class walker {
 public:
  walker(std::string_view packet, listed_fn listed, void* ctx, sink& out)
      : r_(packet), listed_(listed), ctx_(ctx), out_(out) {}

  void run() {
    for (std::size_t guard = 0; guard < 2'000'000; ++guard) {
      const token t = r_.next();
      if (t == token::eof || t == token::error) return;
      if (t == token::start) {
        declare();
        if (is(r_.name(), kRdf, "Description")) description({}, {}, 0);
      } else if (t == token::end) {
        undeclare();
      }
      if (rows_ >= kMaxRows || out_.full()) return;
    }
  }

 private:
  // xmlns declarations on the current start element enter scope at its depth.
  void declare() {
    for (std::size_t i = 0; i < r_.attribute_count(); ++i) {
      const std::string_view n = r_.attribute_name(i);
      if (n.rfind("xmlns:", 0) == 0 && scope_.size() < kMaxScope) {
        scope_.push_back({std::string(n.substr(6)), std::string(r_.attribute_value(i)), r_.depth()});
      }
    }
  }
  void undeclare() {
    while (!scope_.empty() && scope_.back().depth > r_.depth()) scope_.pop_back();
  }

  [[nodiscard]] std::string_view uri_of(std::string_view qname) const {
    const auto colon = qname.find(':');
    const std::string_view prefix = colon == std::string_view::npos ? std::string_view{} : qname.substr(0, colon);
    // Every element looks its prefix up; a packet that declares hundreds of
    // namespaces over hundreds of thousands of elements stops resolving
    // (and so stops yielding rows) once the budget is spent.
    if (lookups_ > kMaxLookupWork) return {};
    lookups_ += scope_.size() + 1;
    for (auto it = scope_.rbegin(); it != scope_.rend(); ++it) {
      if (it->prefix == prefix) return it->uri;
    }
    return {};
  }
  [[nodiscard]] bool is(std::string_view qname, std::string_view uri, std::string_view local) const {
    return codec::xml::local_name(qname) == local && uri_of(qname) == uri;
  }
  [[nodiscard]] bool is_rdf(std::string_view qname) const { return uri_of(qname) == kRdf; }
  [[nodiscard]] static bool is_xml_attr(std::string_view n) {
    return n.rfind("xmlns", 0) == 0 || n.rfind("xml:", 0) == 0;
  }

  // A leaf: `top` is the top-level property's qname, `tail` the Exiv2-style path.
  void leaf(std::string_view top_qname, const std::string& tail, const std::string& value) {
    const std::string_view uri = uri_of(top_qname);
    if (lookups_ > kMaxLookupWork) return;  // unresolved names are not rows
    const std::string_view local = codec::xml::local_name(top_qname);
    if (uri == kXmpNs && local == "CreatorTool" && out_.ev.xmp_creator_tool.empty()) {
      out_.ev.xmp_creator_tool = value.substr(0, 256);
    }
    if (uri == kIptcExt && local == "DigitalSourceType" && out_.ev.source_types.size() < 32) {
      out_.ev.source_types.push_back(value.substr(0, 256));
    }
    if (listed_ && listed_(ctx_, uri, tail)) return;
    const auto colon = top_qname.find(':');
    const std::string prefix(colon == std::string_view::npos ? std::string_view("ns") : top_qname.substr(0, colon));
    ++rows_;
    const std::string group = "Xmp." + prefix;
    (void)out_.add(origin::xmp_raw, group, tail, tail, value, group + "." + tail);
  }

  // Inside an rdf:Description (its start just read): attributes then children.
  void description(std::string_view top, const std::string& path, int nest) {
    for (std::size_t i = 0; i < r_.attribute_count(); ++i) {
      const std::string_view n = r_.attribute_name(i);
      if (is_xml_attr(n) || is_rdf(n)) continue;
      const std::string tail = path.empty() ? std::string(codec::xml::local_name(n)) : path + "/" + std::string(n);
      leaf(top.empty() ? n : top, tail, std::string(r_.attribute_value(i)));
    }
    children(top, path, nest, false);
  }

  // Reads child elements of the element just started as struct fields (or, when
  // `top_level`, as top-level properties) until its end.
  void children(std::string_view top, const std::string& path, int nest, bool) {
    const int depth = r_.depth();
    for (std::size_t guard = 0; guard < 1'000'000; ++guard) {
      const token t = r_.next();
      if (t == token::eof || t == token::error) return;
      if (t == token::end) {
        undeclare();
        if (r_.depth() < depth) return;
        continue;
      }
      if (t != token::start) continue;
      declare();
      const std::string qname(r_.name());
      if (is_rdf(qname)) {  // stray rdf element at struct level: not a field
        r_.skip_element();
        undeclare();
        continue;
      }
      const std::string top_q = top.empty() ? qname : std::string(top);
      const std::string tail = path.empty() ? std::string(codec::xml::local_name(qname)) : path + "/" + qname;
      property_node(top_q, tail, nest + 1);
      if (rows_ >= kMaxRows || out_.full()) return;
    }
  }

  // A property or an rdf:li element whose start was just read: consumes it.
  void property_node(const std::string& top, const std::string& path, int nest) {
    if (nest > kMaxNest) {
      r_.skip_element();
      undeclare();
      return;
    }
    const int depth = r_.depth();
    // rdf:resource, parseType, and shorthand struct fields on the element.
    bool structured = false;
    std::string value;
    bool have_value = false;
    std::vector<std::pair<std::string, std::string>> fields;
    bool resource_struct = false;
    for (std::size_t i = 0; i < r_.attribute_count(); ++i) {
      const std::string_view n = r_.attribute_name(i);
      if (is_xml_attr(n)) continue;
      if (is_rdf(n)) {
        const std::string_view local = codec::xml::local_name(n);
        if (local == "resource") {
          value = std::string(r_.attribute_value(i));
          have_value = true;
        } else if (local == "parseType" && r_.attribute_value(i) == "Resource") {
          resource_struct = true;
        }
        continue;
      }
      fields.emplace_back(std::string(n), std::string(r_.attribute_value(i)));
    }
    for (const auto& [n, v] : fields) {
      structured = true;
      leaf(top, path + "/" + n, v);
    }
    std::size_t item = 0;
    std::string text;
    for (std::size_t guard = 0; guard < 1'000'000; ++guard) {
      if (r_.depth() == depth - 1) break;  // a self-closing element already ended
      const token t = r_.next();
      if (t == token::eof || t == token::error) return;
      if (t == token::text) {
        if (text.size() < 65536) text += r_.text();
        continue;
      }
      if (t == token::end) {
        undeclare();
        if (r_.depth() < depth) break;
        continue;
      }
      // A child element.
      declare();
      const std::string qname(r_.name());
      if (is(qname, kRdf, "Bag") || is(qname, kRdf, "Seq") || is(qname, kRdf, "Alt")) {
        structured = true;
        const int container = r_.depth();
        for (std::size_t g2 = 0; g2 < 1'000'000; ++g2) {
          const token u = r_.next();
          if (u == token::eof || u == token::error) return;
          if (u == token::end) {
            undeclare();
            if (r_.depth() < container) break;
            continue;
          }
          if (u != token::start) continue;
          declare();
          if (is(r_.name(), kRdf, "li")) {
            ++item;
            std::string tail = path + "[" + std::to_string(item) + "]";
            const std::string_view lang = r_.attr("xml:lang");
            if (!lang.empty()) tail += "[@xml:lang=" + std::string(lang) + "]";
            property_node(top, tail, nest + 1);
          } else {
            r_.skip_element();
            undeclare();
          }
          if (rows_ >= kMaxRows || out_.full()) return;
        }
      } else if (is(qname, kRdf, "Description")) {
        structured = true;
        description(top, path, nest + 1);
      } else if (resource_struct || !is_rdf(qname)) {
        // parseType="Resource" fields, or (leniently) a child element as a field.
        structured = true;
        property_node(top, path + "/" + qname, nest + 1);
      } else {
        r_.skip_element();
        undeclare();
      }
      if (rows_ >= kMaxRows || out_.full()) return;
    }
    if (!structured) {
      if (!have_value) {
        // Trim the whitespace XMP pretty-printing puts around text.
        const auto b = text.find_first_not_of(" \t\r\n");
        const auto e = text.find_last_not_of(" \t\r\n");
        value = b == std::string::npos ? std::string{} : text.substr(b, e - b + 1);
      }
      leaf(top, path, value);
    }
  }

  reader r_;
  listed_fn listed_;
  void* ctx_;
  sink& out_;
  std::vector<ns> scope_;
  std::size_t rows_ = 0;
  mutable std::size_t lookups_ = 0;
};

}  // namespace

void read_xmp(std::string_view packet, listed_fn listed, void* ctx, sink& out) {
  if (packet.empty() || packet.size() > kMaxPacket) return;
  try {
    walker w(packet, listed, ctx, out);
    w.run();
  } catch (...) {
  }
}

}  // namespace mv::meta::parse
