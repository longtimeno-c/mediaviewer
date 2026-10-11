// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "geo/fold.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace mv::geo {
namespace {

struct fold_entry {
  char32_t from;
  char32_t to;  // 0: drop
};

constexpr fold_entry k_table[] = {
#include "geo/fold_table.inc"
};

static_assert(std::is_sorted(std::begin(k_table), std::end(k_table),
                             [](const fold_entry& a, const fold_entry& b) { return a.from < b.from; }));

void put_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// Decodes one code point at s[i]; returns its length, or 0 for an invalid sequence.
std::size_t decode(std::string_view s, std::size_t i, char32_t& out) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  std::size_t n = 0;
  char32_t c = 0;
  if (b0 < 0x80) {
    out = b0;
    return 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    n = 2;
    c = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    n = 3;
    c = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    n = 4;
    c = b0 & 0x07;
  } else {
    return 0;
  }
  if (i + n > s.size()) return 0;
  for (std::size_t k = 1; k < n; ++k) {
    const auto b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) return 0;
    c = (c << 6) | (b & 0x3F);
  }
  out = c;
  return n;
}

}  // namespace

std::string fold(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      out.push_back(b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : static_cast<char>(b));
      ++i;
      continue;
    }
    char32_t c = 0;
    const std::size_t n = decode(s, i, c);
    if (n == 0) {
      out.push_back(static_cast<char>(b));
      ++i;
      continue;
    }
    i += n;
    if (c >= 0x0300 && c <= 0x036F) continue;  // combining diacritical marks
    const auto* e = std::lower_bound(std::begin(k_table), std::end(k_table), c,
                                     [](const fold_entry& f, char32_t v) { return f.from < v; });
    if (e != std::end(k_table) && e->from == c) {
      if (e->to != 0) put_utf8(out, e->to);
      continue;
    }
    put_utf8(out, c);
  }
  return out;
}

}  // namespace mv::geo
