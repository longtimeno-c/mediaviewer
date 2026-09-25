// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_webp (libwebp / demux) and the WebP
// animation source directly.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::check(mv::codec::decode_webp(mv::fuzz::bytes(data, size)), "decode_webp");
  mv::fuzz::step_animation(mv::codec::open_webp_animation(mv::fuzz::shared(data, size)), 32);
  return 0;
}
