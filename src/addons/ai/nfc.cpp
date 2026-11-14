// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/nfc.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace mv::ai {
namespace {

struct decomp_row {
  char32_t c, a, b;
};
struct ccc_row {
  char32_t c;
  unsigned char k;
};
struct compose_row {
  char32_t a, b, c;
};

constexpr decomp_row kDecomp[] = {
#define MV_NFC_DECOMP
#include "addons/ai/nfc_table.inc"
#undef MV_NFC_DECOMP
};
constexpr ccc_row kCcc[] = {
#define MV_NFC_CCC
#include "addons/ai/nfc_table.inc"
#undef MV_NFC_CCC
};
constexpr compose_row kCompose[] = {
#define MV_NFC_COMPOSE
#include "addons/ai/nfc_table.inc"
#undef MV_NFC_COMPOSE
};

constexpr char32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr char32_t kLCount = 19, kVCount = 21, kTCount = 28, kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;

unsigned ccc_of(char32_t c) {
  if (c < 0x300) return 0;
  const auto* it = std::lower_bound(std::begin(kCcc), std::end(kCcc), c,
                                    [](const ccc_row& r, char32_t v) { return r.c < v; });
  return it != std::end(kCcc) && it->c == c ? it->k : 0;
}

void decompose(char32_t c, std::u32string& out) {
  if (c >= kSBase && c < kSBase + kSCount) {
    const char32_t i = c - kSBase;
    out.push_back(kLBase + i / kNCount);
    out.push_back(kVBase + (i % kNCount) / kTCount);
    if (const char32_t t = i % kTCount) out.push_back(kTBase + t);
    return;
  }
  if (c >= 0xC0) {
    const auto* it = std::lower_bound(std::begin(kDecomp), std::end(kDecomp), c,
                                      [](const decomp_row& r, char32_t v) { return r.c < v; });
    if (it != std::end(kDecomp) && it->c == c) {
      decompose(it->a, out);
      if (it->b) decompose(it->b, out);
      return;
    }
  }
  out.push_back(c);
}

char32_t compose_pair(char32_t a, char32_t b) {
  if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount) {
    return kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
  }
  if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0 && b > kTBase && b < kTBase + kTCount) {
    return a + (b - kTBase);
  }
  const compose_row want{a, b, 0};
  const auto* it = std::lower_bound(std::begin(kCompose), std::end(kCompose), want, [](const compose_row& x, const compose_row& y) {
    return x.a != y.a ? x.a < y.a : x.b < y.b;
  });
  return it != std::end(kCompose) && it->a == a && it->b == b ? it->c : 0;
}

bool decode(std::string_view s, std::u32string& out) {
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    char32_t c = 0;
    int extra = 0;
    if (b < 0x80) {
      c = b;
    } else if (b >= 0xC2 && b <= 0xDF) {
      c = b & 0x1Fu;
      extra = 1;
    } else if (b >= 0xE0 && b <= 0xEF) {
      c = b & 0x0Fu;
      extra = 2;
    } else if (b >= 0xF0 && b <= 0xF4) {
      c = b & 0x07u;
      extra = 3;
    } else {
      return false;
    }
    if (extra != 0 && i + static_cast<std::size_t>(extra) >= s.size()) return false;
    for (int k = 1; k <= extra; ++k) {
      const auto n = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
      if ((n & 0xC0u) != 0x80u) return false;
      c = (c << 6) | (n & 0x3Fu);
    }
    if ((extra == 2 && c < 0x800) || (extra == 3 && c < 0x10000) || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
      return false;
    }
    out.push_back(c);
    i += 1 + static_cast<std::size_t>(extra);
  }
  return true;
}

void put(std::string& out, char32_t c) {
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

}  // namespace

std::string nfc(std::string_view s) {
  // Nearly every name: nothing to do.
  if (std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
    return std::string(s);
  }
  std::u32string in;
  if (!decode(s, in)) return std::string(s);
  std::u32string d;
  d.reserve(in.size() + 8);
  for (char32_t c : in) decompose(c, d);
  // Canonical order: marks of one run, by class, stable.
  for (std::size_t i = 0; i < d.size();) {
    if (ccc_of(d[i]) == 0) {
      ++i;
      continue;
    }
    std::size_t e = i;
    while (e < d.size() && ccc_of(d[e]) != 0) ++e;
    std::stable_sort(d.begin() + static_cast<std::ptrdiff_t>(i), d.begin() + static_cast<std::ptrdiff_t>(e),
                     [](char32_t a, char32_t b) { return ccc_of(a) < ccc_of(b); });
    i = e;
  }
  // Compose: each mark onto the last starter unless a mark of its class or a
  // starter stands between them.
  std::u32string c;
  c.reserve(d.size());
  std::size_t starter = std::u32string::npos;
  unsigned last = 0;  // the class of what was appended last
  for (char32_t x : d) {
    const unsigned k = ccc_of(x);
    if (starter != std::u32string::npos) {
      const bool blocked = c.size() - 1 != starter && (last == 0 || last >= k);
      if (!blocked) {
        if (const char32_t merged = compose_pair(c[starter], x)) {
          c[starter] = merged;
          continue;
        }
      }
    }
    if (k == 0) starter = c.size();
    last = k;
    c.push_back(x);
  }
  std::string out;
  out.reserve(s.size());
  for (char32_t x : c) put(out, x);
  return out;
}

}  // namespace mv::ai
