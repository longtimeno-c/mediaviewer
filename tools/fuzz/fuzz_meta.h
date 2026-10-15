// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: shared invariant for the meta/parse.h harnesses. A reader may return any
// rows for any input, but never more than the sink's cap, never a value much past
// its cap, and every string valid UTF-8: the panes split and render these
// without checking again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "meta/parse.h"

namespace mv::fuzz {

[[noreturn]] inline void meta_fail(const char* what) {
  std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

inline bool utf8_ok(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (c < 0x80) n = 0;
    else if (c >= 0xC2 && c <= 0xDF) n = 1;
    else if (c >= 0xE0 && c <= 0xEF) n = 2;
    else if (c >= 0xF0 && c <= 0xF4) n = 3;
    else return false;
    if (s.size() - i <= n) return false;
    for (std::size_t k = 1; k <= n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n + 1;
  }
  return true;
}

// Runs `read(sink)` with a small cap and checks every row it left.
template <typename F>
void run_meta_reader(F&& read, const char* who) {
  std::vector<meta::property> rows;
  meta::parse::evidence ev;
  meta::parse::sink sink{rows, ev, 512};
  read(sink);
  if (rows.size() > 512) meta_fail(who);
  for (const auto& p : rows) {
    if (p.value.size() > 4096 + 3 || !utf8_ok(p.value) || !utf8_ok(p.raw_tag) || !utf8_ok(p.label) ||
        !utf8_ok(p.group)) {
      meta_fail(who);
    }
  }
}

}  // namespace mv::fuzz
