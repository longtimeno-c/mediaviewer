// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: the Explorer thumbnail handler's entry point (PR 15,
// plan/09: "Fuzz the handler entry points specifically, not just the
// decoders behind them"). shellext::render_thumbnail is everything
// IThumbnailProvider::GetThumbnail does with the bytes Explorer hands it,
// minus the deadline thread (a fuzzer wants the work on its own thread).
#include "fuzz_common.h"

#include "shellext/thumb_request.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  // The sizes Explorer asks for: small icons, Extra large, and a high-DPI clamp.
  for (std::uint32_t cx : {32u, 256u, 4096u}) {
    auto t = mv::shellext::render_thumbnail(b, cx);
    if (!t) continue;
    const auto& img = t.value();
    if (img.width == 0 || img.height == 0) mv::fuzz::fail("thumbnail: empty");
    if (img.width > mv::shellext::kMaxThumbEdge || img.height > mv::shellext::kMaxThumbEdge) {
      mv::fuzz::fail("thumbnail: over the edge cap");
    }
    if (img.bgra.size() < static_cast<std::uint64_t>(img.width) * img.height * 4) {
      mv::fuzz::fail("thumbnail: short pixel buffer");
    }
  }
  return 0;
}
