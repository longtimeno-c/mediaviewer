// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer harness: codec::decode_ico (BMP and PNG entries).
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::fuzz::check(mv::codec::decode_ico(mv::fuzz::bytes(data, size)), "decode_ico");
  return 0;
}
