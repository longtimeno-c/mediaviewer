// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: codec::decode_docx — the zip reader, the XML reader, the
// styles / numbering / body readers and layout, at a small page size — and
// codec::docx_text (#354), the same readers and layout with no picture
// decoded, which Local search runs over every DOCX in an indexed folder.
#include "fuzz_common.h"

#include <cstdlib>
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
  mv::fuzz::check(mv::codec::decode_docx(in, 0, nullptr, 256), "decode_docx");
  mv::fuzz::check(mv::codec::decode_docx(in, 2, nullptr, 256), "decode_docx page 3");
  if (auto text = mv::codec::docx_text(in)) {
    std::uint32_t last = 0;
    for (const auto& p : *text) {
      // Pages never go back in reading order; no paragraph is empty.
      if (p.text.empty() || p.page < last) std::abort();
      last = p.page;
    }
  }
  return 0;
}
