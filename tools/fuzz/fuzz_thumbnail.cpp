// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: the Explorer thumbnail handler's entry point (PR 15,
// docs/design/09: "Fuzz the handler entry points specifically, not just the
// decoders behind them"). shellext::render_thumbnail is everything
// IThumbnailProvider::GetThumbnail does with the bytes Explorer hands it,
// minus the deadline thread (a fuzzer wants the work on its own thread).
//
// Issue #610: a file that is not a still takes the clip path, as in the
// handler: shellext::classify on the head, then render_poster through a
// seekable reader over the input (seeds: tests/data/seeds/video).
#include "fuzz_common.h"

#include "shellext/thumb_poster.h"
#include "shellext/thumb_request.h"

namespace {

void check(const mv::result<mv::shellext::bgra_thumb>& t) {
  if (!t) return;
  const auto& img = t.value();
  if (img.width == 0 || img.height == 0) mv::fuzz::fail("thumbnail: empty");
  if (img.width > mv::shellext::kMaxThumbEdge || img.height > mv::shellext::kMaxThumbEdge) {
    mv::fuzz::fail("thumbnail: over the edge cap");
  }
  if (img.bgra.size() < static_cast<std::uint64_t>(img.width) * img.height * 4) {
    mv::fuzz::fail("thumbnail: short pixel buffer");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  if (mv::shellext::classify(b) == mv::shellext::source_kind::clip) {
    // One size: a clip's cost is the demux and the decode, not the scale.
    mv::shellext::span_source source(b);
    check(mv::shellext::render_poster(source, 256));
    return 0;
  }
  // The sizes Explorer asks for: small icons, Extra large, and a high-DPI clamp.
  for (std::uint32_t cx : {32u, 256u, 4096u}) check(mv::shellext::render_thumbnail(b, cx));
  return 0;
}
