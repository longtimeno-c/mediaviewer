// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_jpeg: the segment walk, JFIF / JFXX /
// COM / APP14, and APP11 JUMBF reassembly into the C2PA reader.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::run_meta_reader([&](mv::meta::parse::sink& s) { mv::meta::parse::read_jpeg({data, size}, s); },
                            "read_jpeg");
  return 0;
}
