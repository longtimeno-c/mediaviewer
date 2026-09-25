// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: open_animation (GIF, WebP, APNG, HEIC, AVIF by magic),
// stepping frames, rewinding, and decode_animation under a small byte budget.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::step_animation(mv::codec::open_animation(mv::fuzz::shared(data, size)), 64);
  (void)mv::codec::decode_animation(mv::fuzz::bytes(data, size), nullptr, 64u * 1024u * 1024u);
  return 0;
}
