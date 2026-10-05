// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_pdf — our wrapper over Windows.Data.Pdf
// (page count, size, the BMP round trip), at the preview size so a run is fast.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto in = mv::fuzz::bytes(data, size);
  mv::fuzz::check(mv::codec::decode_pdf(in, 0, nullptr, 256), "decode_pdf");
  mv::fuzz::check(mv::codec::decode_pdf(in, 1, nullptr, 256), "decode_pdf page 1");
  return 0;
}
