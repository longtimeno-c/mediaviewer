// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_raw (LibRaw) and looks_like_raw.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  (void)mv::codec::looks_like_raw(b);
  mv::fuzz::check(mv::codec::decode_raw(b), "decode_raw");
  return 0;
}
