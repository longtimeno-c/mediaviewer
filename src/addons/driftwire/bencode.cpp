// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/bencode.h"

#include <algorithm>

namespace mv::driftwire::bencode {

namespace {

bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

std::optional<value> parse(std::string_view& in, int depth) {
  if (in.empty() || depth > kMaxDepth) return std::nullopt;
  const char c = in.front();
  if (c == 'i') {
    in.remove_prefix(1);
    bool neg = false;
    if (!in.empty() && in.front() == '-') {
      neg = true;
      in.remove_prefix(1);
    }
    if (in.empty() || !digit(in.front())) return std::nullopt;
    if (in.front() == '0' && (neg || (in.size() > 1 && in[1] != 'e'))) return std::nullopt;  // no "-0", no "03"
    std::int64_t v = 0;
    std::size_t n = 0;
    while (n < in.size() && digit(in[n])) {
      if (n >= 18) return std::nullopt;  // fits in int64
      v = v * 10 + (in[n] - '0');
      ++n;
    }
    if (n >= in.size() || in[n] != 'e') return std::nullopt;
    in.remove_prefix(n + 1);
    return value::num(neg ? -v : v);
  }
  if (digit(c)) {
    std::size_t len = 0, n = 0;
    if (c == '0' && in.size() > 1 && in[1] != ':') return std::nullopt;
    while (n < in.size() && digit(in[n])) {
      if (n >= 6) return std::nullopt;
      len = len * 10 + static_cast<std::size_t>(in[n] - '0');
      ++n;
    }
    if (n >= in.size() || in[n] != ':') return std::nullopt;
    in.remove_prefix(n + 1);
    if (len > in.size()) return std::nullopt;
    value v = value::bytes(std::string(in.substr(0, len)));
    in.remove_prefix(len);
    return v;
  }
  if (c == 'l') {
    in.remove_prefix(1);
    value v;
    v.k = value::kind::list;
    while (!in.empty() && in.front() != 'e') {
      if (v.l.size() >= 4096) return std::nullopt;
      auto x = parse(in, depth + 1);
      if (!x) return std::nullopt;
      v.l.push_back(std::move(*x));
    }
    if (in.empty()) return std::nullopt;
    in.remove_prefix(1);
    return v;
  }
  if (c == 'd') {
    in.remove_prefix(1);
    value v;
    v.k = value::kind::dict;
    while (!in.empty() && in.front() != 'e') {
      if (v.d.size() >= 256) return std::nullopt;
      auto key = parse(in, depth + 1);
      if (!key || key->k != value::kind::bytes) return std::nullopt;
      auto x = parse(in, depth + 1);
      if (!x) return std::nullopt;
      for (const auto& [k2, unused] : v.d) {
        if (k2 == key->s) return std::nullopt;  // a key twice
      }
      v.d.emplace_back(std::move(key->s), std::move(*x));
    }
    if (in.empty()) return std::nullopt;
    in.remove_prefix(1);
    return v;
  }
  return std::nullopt;
}

}  // namespace

const value* value::get(std::string_view key) const noexcept {
  if (k != kind::dict) return nullptr;
  for (const auto& [name, v] : d) {
    if (name == key) return &v;
  }
  return nullptr;
}

const std::string* value::str(std::string_view key) const noexcept {
  const value* v = get(key);
  return v && v->k == kind::bytes ? &v->s : nullptr;
}

std::optional<std::int64_t> value::integer(std::string_view key) const noexcept {
  const value* v = get(key);
  if (!v || v->k != kind::integer) return std::nullopt;
  return v->i;
}

std::optional<value> decode(std::string_view in, std::string_view* rest) {
  if (in.size() > kMaxInput) return std::nullopt;
  auto v = parse(in, 0);
  if (!v) return std::nullopt;
  if (rest) {
    *rest = in;
  } else if (!in.empty()) {
    return std::nullopt;
  }
  return v;
}

void encode(const value& v, std::string& out) {
  switch (v.k) {
    case value::kind::integer:
      out += 'i';
      out += std::to_string(v.i);
      out += 'e';
      break;
    case value::kind::bytes:
      out += std::to_string(v.s.size());
      out += ':';
      out += v.s;
      break;
    case value::kind::list:
      out += 'l';
      for (const auto& x : v.l) encode(x, out);
      out += 'e';
      break;
    case value::kind::dict: {
      std::vector<const std::pair<std::string, value>*> sorted;
      sorted.reserve(v.d.size());
      for (const auto& p : v.d) sorted.push_back(&p);
      std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->first < b->first; });
      out += 'd';
      for (const auto* p : sorted) {
        out += std::to_string(p->first.size());
        out += ':';
        out += p->first;
        encode(p->second, out);
      }
      out += 'e';
      break;
    }
  }
}

std::string encode(const value& v) {
  std::string out;
  encode(v, out);
  return out;
}

}  // namespace mv::driftwire::bencode
