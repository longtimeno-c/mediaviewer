// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer harness: codec::decode_jpeg (libjpeg-turbo) and jpeg_dimensions.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  (void)mv::codec::jpeg_dimensions(b);
  // The last byte picks the DCT scale, so 1:1 and the preview scales are all reached.
  static constexpr int kScale[] = {1, 2, 4, 8};
  const int scale = size ? kScale[data[size - 1] & 3] : 1;
  mv::fuzz::check(mv::codec::decode_jpeg(b, nullptr, scale), "decode_jpeg");
  return 0;
}
