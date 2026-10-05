// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_docx — the zip reader, the XML reader, the
// styles / numbering / body readers and layout, at a small page size.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto in = mv::fuzz::bytes(data, size);
  mv::fuzz::check(mv::codec::decode_docx(in, 0, nullptr, 256), "decode_docx");
  mv::fuzz::check(mv::codec::decode_docx(in, 2, nullptr, 256), "decode_docx page 3");
  return 0;
}
