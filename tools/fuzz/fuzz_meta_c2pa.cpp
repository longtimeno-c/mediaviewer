// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_c2pa on a JUMBF manifest store: the
// box walk, the bounded CBOR decoder, JSON assertions, the COSE signature, and
// the DER certificate and timestamp readers.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::run_meta_reader([&](mv::meta::parse::sink& s) { mv::meta::parse::read_c2pa({data, size}, s); },
                            "read_c2pa");
  return 0;
}
