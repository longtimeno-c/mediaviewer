// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer harness: codec::decode_gif (giflib; frame 0 composited) and the GIF
// animation source directly.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::check(mv::codec::decode_gif(mv::fuzz::bytes(data, size)), "decode_gif");
  mv::fuzz::step_animation(mv::codec::open_gif_animation(mv::fuzz::shared(data, size)), 32);
  return 0;
}
