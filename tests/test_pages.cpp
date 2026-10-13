// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pages inside one navigation stop (docs/plans/audio-and-documents.md §2.3):
// codec::decode takes a page, says how many there are, and skips a TIFF's
// reduced-resolution subfiles; an ICO's sizes are pages too (issue #211; the
// HEIC half is in test_heif_avif.cpp). Ctrl+PageDown / PageUp are the keys.
#include "catch_compat.h"

#include <cstdint>
#include <vector>

#include "codec/decode.h"
#include "fixtures_tiff_ico.h"
#include "image/pipeline.h"
#include "shell/key_router.h"

using mv::status;

using fixtures::tiff_pages;

TEST_CASE("A multi-page TIFF decodes each page and counts only real pages", "[pages][tiff]") {
  const auto bytes = tiff_pages({{40}, {80}, {200, true}, {120}});
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

// A flat w x h 32-bit icon entry of one colour.
fixtures::ico_entry ico_size(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g,
                             std::uint8_t b) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < px.size(); i += 4) {
    px[i] = r;
    px[i + 1] = g;
    px[i + 2] = b;
    px[i + 3] = 255;
  }
  fixtures::ico_entry e;
  e.payload = fixtures::ico_dib(w, h, 32, px.data());
  e.w_byte = static_cast<std::uint8_t>(w);
  e.h_byte = static_cast<std::uint8_t>(h);
  e.bpp = 32;
  return e;
}

TEST_CASE("An ICO's sizes are pages, largest first", "[pages][ico]") {
  // Directory order is small, large, medium: pages follow the still's rank.
  const auto bytes = fixtures::ico_file({ico_size(16, 16, 255, 0, 0), ico_size(48, 48, 0, 255, 0),
                                         ico_size(32, 32, 0, 0, 255)});

  auto first = mv::codec::decode(bytes);
  REQUIRE(first);
  CHECK(first->page == 0);
  CHECK(first->page_count == 3);
  CHECK(first->width == 48);
  CHECK(first->rgba[1] == 255);  // green: the still is unchanged

  auto second = mv::codec::decode(bytes, nullptr, 4, 1);
  REQUIRE(second);
  CHECK(second->page == 1);
  CHECK(second->page_count == 3);
  CHECK(second->format == mv::codec::format_family::ico);
  CHECK(second->width == 32);
  CHECK(second->rgba[2] == 255);  // blue

  auto third = mv::codec::decode(bytes, nullptr, 4, 2);
  REQUIRE(third);
  CHECK(third->width == 16);
  CHECK(third->rgba[0] == 255);  // red

  auto past = mv::codec::decode(bytes, nullptr, 4, 3);
  REQUIRE_FALSE(past);
  CHECK(past.error() == status::invalid_arg);

  auto shown = mv::image::decode_bytes(bytes, nullptr, 4, 2);
  REQUIRE(shown);
  CHECK(shown->page == 2);
  CHECK(shown->page_count == 3);

  // A lone icon is one page.
  const auto lone = fixtures::ico_file({ico_size(16, 16, 9, 9, 9)});
  auto only = mv::codec::decode(lone);
  REQUIRE(only);
  CHECK(only->page_count == 1);
  auto none = mv::codec::decode(lone, nullptr, 4, 1);
  REQUIRE_FALSE(none);
  CHECK(none.error() == status::invalid_arg);
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
