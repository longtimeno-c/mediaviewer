// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/xml.h"

namespace mv::codec::xml {
namespace {

constexpr int kMaxDepth = 256;

bool name_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' ||
         c == '_' || c == '-' || c == '.' || static_cast<unsigned char>(c) >= 0x80;
}

bool space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
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

}  // namespace

std::string_view local_name(std::string_view qname) noexcept {
  const auto colon = qname.find(':');
  return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

std::string_view reader::local() const noexcept { return local_name(name_); }

std::string_view reader::attr(std::string_view qname) const noexcept {
  for (const attribute& a : attrs_) {
    if (a.name == qname) return a.value;
  }
  return {};
}

bool reader::has(std::string_view qname) const noexcept {
  for (const attribute& a : attrs_) {
    if (a.name == qname) return true;
  }
  return false;
}

bool reader::decode(std::string_view raw, std::string& out) const {
  out.clear();
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c != '&') {
      out += c;
      continue;
    }
    const auto semi = raw.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12) return false;
    const std::string_view ent = raw.substr(i + 1, semi - i - 1);
    if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "amp") out += '&';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (ent.size() > 1 && ent[0] == '#') {
      std::uint32_t cp = 0;
      const bool hex = ent[1] == 'x' || ent[1] == 'X';
      for (std::size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
        const char d = ent[k];
        std::uint32_t v = 0;
        if (d >= '0' && d <= '9') v = static_cast<std::uint32_t>(d - '0');
        else if (hex && d >= 'a' && d <= 'f') v = static_cast<std::uint32_t>(d - 'a' + 10);
        else if (hex && d >= 'A' && d <= 'F') v = static_cast<std::uint32_t>(d - 'A' + 10);
        else return false;
        cp = cp * (hex ? 16u : 10u) + v;
        if (cp > 0x10FFFF) return false;
      }
      append_utf8(out, cp);
    } else {
      return false;  // no DTD, so no other entity can exist
    }
    i = semi;
  }
  return true;
}

token reader::read_tag() {
  // at_ is just past '<'.
  const std::size_t n = doc_.size();
  if (at_ < n && doc_[at_] == '?') {
    const auto close = doc_.find("?>", at_);
    if (close == std::string_view::npos) return token::error;
    at_ = close + 2;
    return token::eof;  // skipped: next() reads on (a loop, never recursion)
  }
  if (doc_.substr(at_, 3) == "!--") {
    const auto close = doc_.find("-->", at_ + 3);
    if (close == std::string_view::npos) return token::error;
    at_ = close + 3;
    return token::eof;  // skipped
  }
  if (doc_.substr(at_, 8) == "![CDATA[") {
    const auto close = doc_.find("]]>", at_ + 8);
    if (close == std::string_view::npos) return token::error;
    text_.assign(doc_.substr(at_ + 8, close - at_ - 8));
    at_ = close + 3;
    return token::text;
  }
  if (at_ < n && doc_[at_] == '!') return token::error;  // DOCTYPE and anything else declared

  const bool closing = at_ < n && doc_[at_] == '/';
  if (closing) ++at_;
  const std::size_t name_start = at_;
  while (at_ < n && name_char(doc_[at_])) ++at_;
  if (at_ == name_start) return token::error;
  name_ = doc_.substr(name_start, at_ - name_start);
  attrs_.clear();

  if (closing) {
    while (at_ < n && space(doc_[at_])) ++at_;
    if (at_ >= n || doc_[at_] != '>') return token::error;
    ++at_;
    if (depth_ <= 0) return token::error;
    --depth_;
    return token::end;
  }

  for (;;) {
    while (at_ < n && space(doc_[at_])) ++at_;
    if (at_ >= n) return token::error;
    if (doc_[at_] == '>') {
      ++at_;
      break;
    }
    if (doc_[at_] == '/') {
      if (at_ + 1 >= n || doc_[at_ + 1] != '>') return token::error;
      at_ += 2;
      pending_end_ = true;
      break;
    }
    const std::size_t an = at_;
    while (at_ < n && name_char(doc_[at_])) ++at_;
    if (at_ == an) return token::error;
    attribute a;
    a.name = doc_.substr(an, at_ - an);
    while (at_ < n && space(doc_[at_])) ++at_;
    if (at_ >= n || doc_[at_] != '=') return token::error;
    ++at_;
    while (at_ < n && space(doc_[at_])) ++at_;
    if (at_ >= n || (doc_[at_] != '"' && doc_[at_] != '\'')) return token::error;
    const char quote = doc_[at_++];
    const auto close = doc_.find(quote, at_);
    if (close == std::string_view::npos) return token::error;
    if (!decode(doc_.substr(at_, close - at_), a.value)) return token::error;
    at_ = close + 1;
    attrs_.push_back(std::move(a));
  }
  if (++depth_ > kMaxDepth) return token::error;
  return token::start;
}

token reader::next() {
  if (pending_end_) {
    pending_end_ = false;
    --depth_;
    attrs_.clear();
    return token::end;
  }
  const std::size_t n = doc_.size();
  while (at_ < n && doc_[at_] == '<') {
    ++at_;
    // read_tag answers eof for a comment or processing instruction it stepped
    // over; at the real end of input the check below decides.
    const token t = read_tag();
    if (t != token::eof) return t;
  }
  if (at_ >= n) return depth_ == 0 ? token::eof : token::error;
  const auto lt = doc_.find('<', at_);
  const std::size_t end = lt == std::string_view::npos ? n : lt;
  if (!decode(doc_.substr(at_, end - at_), text_)) return token::error;
  at_ = end;
  return token::text;
}

void reader::skip_element() {
  const int target = depth_ - 1;
  for (;;) {
    const token t = next();
    if (t == token::eof || t == token::error) return;
    if (t == token::end && depth_ == target) return;
  }
}

}  // namespace mv::codec::xml
