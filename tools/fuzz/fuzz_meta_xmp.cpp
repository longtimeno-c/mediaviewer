// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#290): meta::parse::read_xmp: the RDF walk over the XML pull
// reader (namespaces, structs, arrays, attributes), with nothing pre-listed.
#include "fuzz_meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  mv::fuzz::run_meta_reader([&](mv::meta::parse::sink& s) { mv::meta::parse::read_xmp(text, nullptr, nullptr, s); },
                            "read_xmp");
  return 0;
}
