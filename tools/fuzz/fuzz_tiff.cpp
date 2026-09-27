// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_tiff (libtiff).
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::check(mv::codec::decode_tiff(mv::fuzz::bytes(data, size)), "decode_tiff");
  return 0;
}
