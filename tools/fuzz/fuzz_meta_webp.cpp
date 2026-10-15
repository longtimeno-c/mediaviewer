// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_webp: the RIFF chunk walk.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::run_meta_reader([&](mv::meta::parse::sink& s) { mv::meta::parse::read_webp({data, size}, s); },
                            "read_webp");
  return 0;
}
