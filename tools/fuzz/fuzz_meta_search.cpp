// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#291): meta::read_search_facts, the folder tag pass's
// bounded read (search.h: a 512 KB head, the TIFF / RAW 8 MB retry, the
// ISO-BMFF walk, a clip through libavformat, the XMP sidecar), then the text
// it builds: folded_text, search_words, match_fields, why_of. The read takes
// a path, so each input is written to one scratch file first.
//
// Invariants: no field is empty, none holds the field separator or a line
// break, the folded text has one separator fewer than there are fields, and
// every field's own first word matches the file and names a field.
//
// Not in MV_FUZZ_HARNESSES yet: it links mv_meta (Exiv2, FFmpeg), which the
// Windows fuzz configuration does not instrument or deploy. Built and run on
// the Mac with Homebrew LLVM for the #291 review (15 min, no finding):
//   clang++ -fsanitize=fuzzer-no-link,address,undefined on src/meta/*.cpp and
//   this file, linked with the Release mv_codec / mv_image / mv_gfx / mv_io /
//   mv_core archives and the vcpkg Exiv2 / FFmpeg dylibs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "meta/search.h"

namespace {

[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

const std::string& scratch() {
  static const std::string path = [] {
    const char* dir = std::getenv("TMPDIR");
    std::string p = (dir && *dir) ? dir : "/tmp";
    if (p.back() != '/') p.push_back('/');
    return p + "mv_fuzz_meta_search_" + std::to_string(::getpid()) + ".jpg";
  }();
  return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  {
    std::FILE* f = std::fopen(scratch().c_str(), "wb");
    if (!f) return 0;
    if (size) std::fwrite(data, 1, size, f);
    std::fclose(f);
  }
  auto facts = mv::meta::read_search_facts(scratch());
  if (!facts) return 0;
  const auto& fields = facts->fields;
  for (const std::string& s : fields) {
    if (s.empty()) fail("empty field");
    for (char c : s) {
      if (c == mv::meta::kFieldSeparator || c == '\n' || c == '\r') fail("separator or line break in a field");
    }
  }
  const std::string folded = mv::meta::folded_text(fields);
  std::size_t seps = 0;
  for (char c : folded) seps += c == mv::meta::kFieldSeparator ? 1 : 0;
  if (!fields.empty() && seps != fields.size() - 1) fail("separator count");
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto words = mv::meta::search_words(fields[i]);
    if (words.empty()) continue;  // a field of marks or spaces only
    const std::string one[] = {words[0]};
    const int at = mv::meta::match_fields(folded, one);
    if (at < 0 || static_cast<std::size_t>(at) >= fields.size()) fail("a field's own word does not match");
    (void)mv::meta::why_of(fields, at, 8);
  }
  return 0;
}
