// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: what a drop onto the Final Cut Pro panel parses
// (docs/design/29): the lossless XML tree and the timeline model over it, the
// new version it writes, and the bridge state the agent reads. Whatever
// parses must write back byte for byte with no edit. Seeds: tests/data/fcpxml.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "nle/bridge_state.h"
#include "nle/timeline.h"
#include "nle/xml.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);
  mv::nle::xml::document d;
  if (d.parse(text, nullptr) && d.write() != text) std::abort();  // lossless or it is a bug
  mv::nle::timeline t;
  if (t.load(text, nullptr)) {
    (void)t.describe();
    (void)t.media_paths();
    (void)t.length();
    const std::string v = t.write_version("V (Claude 1)");
    mv::nle::timeline again;
    if (!again.load(v, nullptr)) std::abort();  // a version always reads back
  }
  (void)mv::nle::bridge_state_from_json(text);
  return 0;
}
