// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_pptx — the zip reader, the XML reader, the
// presentation, master, layout and slide readers and layout, at a small page size.
#include "fuzz_common.h"

#include <vector>

#include "codec/text.h"
#include "io/file.h"

namespace {

// This harness links codec without image, so it installs the reader image/
// would have installed. Without it every face stays unloaded.
std::vector<std::uint8_t> read_font(std::string_view path) {
  auto read = mv::io::read_all(path);
  if (!read) return {};
  return std::move(read).value();
}

struct install_font_reader {
  install_font_reader() noexcept { mv::codec::text::set_font_reader(&read_font); }
};
const install_font_reader g_install_font_reader;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto in = mv::fuzz::bytes(data, size);
  mv::fuzz::check(mv::codec::decode_pptx(in, 0, nullptr, 256), "decode_pptx");
  mv::fuzz::check(mv::codec::decode_pptx(in, 2, nullptr, 256), "decode_pptx page 3");
  return 0;
}
