// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_icc: the header, the tag table, and
// desc / text / mluc decoding.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::run_meta_reader([&](mv::meta::parse::sink& s) { (void)mv::meta::parse::read_icc({data, size}, s); },
                            "read_icc");
  return 0;
}
