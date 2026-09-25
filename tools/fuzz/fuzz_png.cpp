// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_png (libspng) and the APNG chunk walker.
#include "codec/apng.h"
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  mv::fuzz::check(mv::codec::decode_png(b), "decode_png");
  // APNG: walk the chunks, rebuild each frame as a standalone PNG, decode it.
  auto info = mv::codec::parse_apng(b);
  if (info) {
    std::size_t n = 0;
    for (const auto& frame : info.value().frames) {
      if (++n > 8) break;
      auto png = mv::codec::apng_frame_png(info.value(), frame);
      if (png) mv::fuzz::check(mv::codec::decode_png(png.value()), "apng frame");
    }
  }
  return 0;
}
