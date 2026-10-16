// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PDF through the OS renderer (docs/plans/audio-and-documents.md §2.4):
// CoreGraphics on macOS, Windows.Data.Pdf on Windows, the same expectations
// on both. Hand-built files (pdf_fixture.h), so no corpus.
#include "catch_compat.h"

#include <cstdint>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "image/pipeline.h"
#include "pdf_fixture.h"

using mv::status;
namespace pdffx = mv::test::pdffx;

namespace {

struct rgb {
  int r, g, b;
};

rgb at(const mv::codec::raster& r, std::uint32_t x, std::uint32_t y) {
  const std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * r.width + x) * 4u;
  return {p[0], p[1], p[2]};
}

bool white(rgb c) { return c.r > 230 && c.g > 230 && c.b > 230; }
bool blue(rgb c) { return c.r < 60 && c.g < 90 && c.b > 180; }
bool red(rgb c) { return c.r > 180 && c.g < 80 && c.b < 80; }

// Page 1: 200 x 100 pt, the left half blue. Page 2: 100 x 200 pt with its
// bottom half red, turned 90° clockwise by /Rotate — so on screen it is
// landscape with the red on the left.
std::vector<std::uint8_t> two_pages(const std::string& prefix = {}) {
  return pdffx::make({{200, 100, 0, "0 0 1 rg 0 0 100 100 re f"},
                      {100, 200, 90, "1 0 0 rg 0 0 100 100 re f"}},
                     prefix);
}

}  // namespace

TEST_CASE("PDF is recognised by its header, even after leading bytes", "[pdf][probe]") {
  CHECK(mv::codec::probe(two_pages()) == mv::codec::format_family::pdf);
  CHECK(mv::codec::probe(two_pages(std::string(40, ' '))) == mv::codec::format_family::pdf);
  CHECK(std::string(mv::codec::format_name(mv::codec::format_family::pdf)) == "PDF");
  const std::vector<std::uint8_t> late(2000, ' ');
  std::vector<std::uint8_t> past = late;
  past.insert(past.end(), {'%', 'P', 'D', 'F', '-'});
  CHECK(mv::codec::probe(past) == mv::codec::format_family::unknown);  // not in the first 1 KiB
}

TEST_CASE("A PDF page renders on white at the long-edge size, rotation applied", "[pdf]") {
  const auto bytes = two_pages();
  auto first = mv::codec::decode(bytes);
  REQUIRE(first);
  CHECK(first->format == mv::codec::format_family::pdf);
  CHECK(first->page == 0);
  CHECK(first->page_count == 2);
  CHECK(first->width == mv::codec::kPdfLongEdge);
  CHECK(first->height == mv::codec::kPdfLongEdge / 2);
  CHECK(blue(at(*first, first->width / 4, first->height / 2)));
  CHECK(white(at(*first, first->width * 3 / 4, first->height / 2)));

  auto second = mv::codec::decode(bytes, nullptr, 4, 1);
  REQUIRE(second);
  CHECK(second->page == 1);
  CHECK(second->width == mv::codec::kPdfLongEdge);  // portrait page, turned landscape
  CHECK(second->height == mv::codec::kPdfLongEdge / 2);
  CHECK(red(at(*second, second->width / 4, second->height / 2)));
  CHECK(white(at(*second, second->width * 3 / 4, second->height / 2)));

  auto past = mv::codec::decode(bytes, nullptr, 4, 2);
  REQUIRE_FALSE(past);
  CHECK(past.error() == status::invalid_arg);

  // Through the display pipeline: sRGB in, sRGB out, page count carried.
  auto shown = mv::image::decode_bytes(bytes, nullptr, 4, 1);
  REQUIRE(shown);
  CHECK(shown->page_count == 2);
}

TEST_CASE("A broken PDF is an error, not a crash", "[pdf]") {
  std::vector<std::uint8_t> junk = {'%', 'P', 'D', 'F', '-', '1', '.', '4', '\n'};
  junk.resize(300, 'x');
  auto r = mv::codec::decode(junk);
  REQUIRE_FALSE(r);
  CHECK(r.error() != status::ok);
}
