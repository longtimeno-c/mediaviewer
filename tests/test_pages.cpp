// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pages inside one navigation stop (docs/plans/audio-and-documents.md §2.3):
// codec::decode takes a page, says how many there are, and skips a TIFF's
// reduced-resolution subfiles; Ctrl+PageDown / PageUp are the keys.
#include "catch_compat.h"

#include <cstdint>
#include <vector>

#include "codec/decode.h"
#include "fixtures_tiff_ico.h"
#include "image/pipeline.h"
#include "shell/key_router.h"

using mv::status;

namespace {

struct page_spec {
  std::uint8_t grey;
  bool reduced = false;  // a thumbnail IFD, not a page
};

// One 8 x 8 grey strip per directory, in order.
std::vector<std::uint8_t> multipage_tiff(const std::vector<page_spec>& pages) {
  using namespace fixtures;
  tiff_sink sink;
  TIFF* tif = TIFFClientOpen("fixture", "w", &sink, tiff_sink_read, tiff_sink_write, tiff_sink_seek,
                             tiff_sink_close, tiff_sink_size, tiff_sink_map, tiff_sink_unmap);
  if (!tif) return {};
  bool ok = true;
  for (const page_spec& p : pages) {
    ok &= TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 8u) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 8u) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 8u) == 1;
    if (p.reduced) ok &= TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE) == 1;
    std::vector<std::uint8_t> row(8, p.grey);
    for (std::uint32_t y = 0; y < 8 && ok; ++y) ok &= TIFFWriteScanline(tif, row.data(), y, 0) == 1;
    ok &= TIFFWriteDirectory(tif) == 1;
  }
  TIFFClose(tif);
  if (!ok) return {};
  return std::move(sink.data);
}

}  // namespace

TEST_CASE("A multi-page TIFF decodes each page and counts only real pages", "[pages][tiff]") {
  const auto bytes = multipage_tiff({{40}, {80}, {200, true}, {120}});
  REQUIRE_FALSE(bytes.empty());

  auto first = mv::codec::decode(bytes);
  REQUIRE(first);
  CHECK(first->page == 0);
  CHECK(first->page_count == 3);  // the reduced-resolution IFD is not a page
  CHECK(first->rgba[0] == 40);

  auto second = mv::codec::decode(bytes, nullptr, 4, 1);
  REQUIRE(second);
  CHECK(second->page == 1);
  CHECK(second->rgba[0] == 80);

  auto third = mv::codec::decode(bytes, nullptr, 4, 2);
  REQUIRE(third);
  CHECK(third->rgba[0] == 120);  // skipped the thumbnail between them

  auto past = mv::codec::decode(bytes, nullptr, 4, 3);
  REQUIRE_FALSE(past);
  CHECK(past.error() == status::invalid_arg);

  // The display pipeline carries the count through.
  auto shown = mv::image::decode_bytes(bytes, nullptr, 4, 1);
  REQUIRE(shown);
  CHECK(shown->page == 1);
  CHECK(shown->page_count == 3);
}

TEST_CASE("A single-page file is one page, and has no page 1", "[pages]") {
  const std::uint8_t grey = 7;
  fixtures::tiff_spec spec;
  spec.spp = 1;
  spec.photometric = PHOTOMETRIC_MINISBLACK;
  const auto bytes = fixtures::tiff_write(spec, &grey);
  REQUIRE_FALSE(bytes.empty());
  auto only = mv::codec::decode(bytes);
  REQUIRE(only);
  CHECK(only->page_count == 1);
  auto none = mv::codec::decode(bytes, nullptr, 4, 1);
  REQUIRE_FALSE(none);
  CHECK(none.error() == status::invalid_arg);
}

TEST_CASE("Ctrl+PageDown / PageUp turn pages; plain PageDown still skips ten", "[pages][router]") {
  using namespace mv::shell;
  key_router r;
  view_state still;
  still.item = item_kind::still;
  const key_event down_ctrl{key::page_down, mod_ctrl, false, false};
  const key_event up_ctrl{key::page_up, mod_ctrl, false, false};
  const key_event down_plain{key::page_down, mod_none, false, false};
  CHECK(r.on_key(down_ctrl, still).command == command_id::next_page);
  CHECK(r.on_key(up_ctrl, still).command == command_id::prev_page);
  CHECK(r.on_key(down_plain, still).command == command_id::skip_forward);
}
