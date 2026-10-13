// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_bmff over memory: top-level boxes, a
// HEIF meta box's items / references / properties (with any ICC in colr), and the
// C2PA uuid / c2pa box. Both the still and the clip walk.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  for (const bool stills : {true, false}) {
    mv::fuzz::run_meta_reader(
        [&](mv::meta::parse::sink& s) {
          mv::meta::parse::memory_source src({data, size});
          mv::meta::parse::read_bmff(src, stills, s);
        },
        "read_bmff");
  }
  return 0;
}
