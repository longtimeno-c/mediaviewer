// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer harness: codec::decode_raw_preview (embedded JPEG inside a RAW).
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  (void)mv::codec::looks_like_raw(b);
  mv::fuzz::check(mv::codec::decode_raw_preview(b), "decode_raw_preview");
  return 0;
}
