// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_heic (libheif + libde265) and the HEIF
// sequence source.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::check(mv::codec::decode_heic(mv::fuzz::bytes(data, size)), "decode_heic");
  mv::fuzz::step_animation(mv::codec::open_heic_animation(mv::fuzz::shared(data, size)), 8);
  return 0;
}
