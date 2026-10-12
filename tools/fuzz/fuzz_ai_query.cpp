// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#291): the Local search query language (addons/ai/query.h):
// parse, resolve against two people, suggest, and the shared fold. Typed text
// reaches it on every keystroke. Pure: no index, no file.
//
// Invariants: the tag terms (camera:, lens:, made:) come out folded and never
// empty; in: that starts with a digit is never a place; a plan's tag words are
// folded and never a connector.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "addons/ai/query.h"
#include "core/fold.h"

namespace {

[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

bool valid_utf8(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = c < 0x80 ? 0 : (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2
                                 : (c >= 0xF0 && c <= 0xF4) ? 3 : 99;
    if (n == 99 || s.size() - i <= n) return false;
    for (std::size_t k = 1; k <= n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    const auto c1 = n ? static_cast<unsigned char>(s[i + 1]) : 0;
    // No overlong form, no surrogate, nothing past U+10FFFF.
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) ||
        (c == 0xF4 && c1 >= 0x90)) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

void folded_terms(const std::vector<std::string>& terms, const char* who) {
  for (const std::string& t : terms) {
    if (t.empty() || mv::text::fold(t) != t) fail(who);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 4096) return 0;  // a search field, not a document
  const std::string_view typed(reinterpret_cast<const char*>(data), size);
  namespace q = mv::ai::query;
  const q::parsed p = q::parse(typed);
  // Hosts hand over valid UTF-8. On invalid bytes the fold passes them
  // through and a dropped mark may join two of them into a letter, so "is
  // folded" (fold(x) == x) is checked on valid input only.
  const bool valid = valid_utf8(typed);
  if (valid) {
    folded_terms(p.cameras, "camera: not folded");
    folded_terms(p.not_cameras, "-camera: not folded");
    folded_terms(p.lenses, "lens: not folded");
    folded_terms(p.made, "made: not folded");
  }
  for (const std::string& place : p.places) {
    if (place.empty()) fail("empty place");
  }
  const q::person_name people[] = {{1, "Sam Smith"}, {2, "Zoë Ångström"}};
  const q::plan pl = q::resolve(p, people);
  for (const std::string& w : pl.tag_words) {
    if (w.empty() || w == "or" || (valid && mv::text::fold(w) != w)) fail("tag word");
  }
  (void)q::suggest(typed, people, 5);
  if (valid) {
    const std::string f = mv::text::fold(typed);
    if (mv::text::fold(f) != f) fail("fold is not idempotent");
  }
  (void)q::name_matches(typed, pl.files, pl.not_files);
  return 0;
}
