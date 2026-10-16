// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PPTX (issue #429): a slide a page, drawn by codec/pptx.cpp over the DOCX
// text path. Hand-built packages (pptx_fixture.h), so no corpus.
#include "catch_compat.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "codec/decode.h"
#include "docx_fixture.h"
#include "image/pipeline.h"
#include "pptx_fixture.h"

using mv::status;
namespace fx = mv::test::pptxfx;

namespace {

struct rgb {
  int r, g, b;
};

rgb at(const mv::codec::raster& r, double fx_, double fy_) {
  const auto x = static_cast<std::uint32_t>(fx_ * r.width);
  const auto y = static_cast<std::uint32_t>(fy_ * r.height);
  const std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * r.width + x) * 4u;
  return {p[0], p[1], p[2]};
}
bool near(rgb c, int r, int g, int b) {
  return std::abs(c.r - r) < 24 && std::abs(c.g - g) < 24 && std::abs(c.b - b) < 24;
}
// Pixels darker than mid grey in a box of the slide (fractions of its size).
int dark(const mv::codec::raster& r, double x0, double y0, double x1, double y1) {
  int n = 0;
  for (auto y = static_cast<std::uint32_t>(y0 * r.height); y < static_cast<std::uint32_t>(y1 * r.height); ++y) {
    for (auto x = static_cast<std::uint32_t>(x0 * r.width); x < static_cast<std::uint32_t>(x1 * r.width); ++x) {
      const std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * r.width + x) * 4u;
      if (p[0] + p[1] + p[2] < 3 * 110) ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("PPTX is told from DOCX by its parts, even past the first KiB", "[pptx][probe]") {
  const auto deck = fx::pptx({fx::slide("")});
  CHECK(mv::codec::probe(deck) == mv::codec::format_family::pptx);
  CHECK(std::string(mv::codec::format_name(mv::codec::format_family::pptx)) == "PPTX");
  // A long [Content_Types].xml first: the ppt/ parts start past 1 KiB, and the
  // central directory says what it is.
  const std::string pad(4000, ' ');
  const auto late = fx::pptx({fx::slide("")}, 960, 540, R"(<a:srgbClr val="FFFFFF"/>)", {}, {}, pad);
  CHECK(mv::codec::probe(late) == mv::codec::format_family::pptx);
  // Only the first bytes: not enough to tell, and a package stays a DOCX as before.
  CHECK(mv::codec::probe(std::span(late).first(512)) == mv::codec::format_family::docx);
}

TEST_CASE("A PPTX is a page per slide at the slide's shape", "[pptx]") {
  const auto deck = fx::pptx({fx::slide(""), fx::slide(""), fx::slide("")});
  auto first = mv::codec::decode(deck);
  REQUIRE(first);
  CHECK(first->format == mv::codec::format_family::pptx);
  CHECK(first->page == 0);
  CHECK(first->page_count == 3);
  CHECK(first->width == mv::codec::kPptxLongEdge);
  CHECK(first->height == mv::codec::kPptxLongEdge * 9 / 16);
  auto third = mv::codec::decode(deck, nullptr, 4, 2);
  REQUIRE(third);
  CHECK(third->page == 2);
  auto past = mv::codec::decode(deck, nullptr, 4, 3);
  REQUIRE_FALSE(past);
  CHECK(past.error() == status::invalid_arg);

  // 4:3 and through the display pipeline's preview size.
  const auto square = fx::pptx({fx::slide("")}, 720, 540);
  auto preview = mv::image::decode_preview(square);
  REQUIRE(preview);
  CHECK(preview->width == mv::codec::kPptxPreviewEdge);
  CHECK(preview->height == mv::codec::kPptxPreviewEdge * 3 / 4);
}

TEST_CASE("PPTX backgrounds come from the slide, else the master, theme colours mapped", "[pptx]") {
  const auto deck = fx::pptx({fx::slide(""), fx::slide("", fx::solid_bg(R"(<a:schemeClr val="accent1"/>)"))},
                             960, 540, R"(<a:srgbClr val="C00000"/>)");
  auto master_bg = mv::codec::decode_pptx(deck, 0, nullptr, 320);
  REQUIRE(master_bg);
  CHECK(near(at(*master_bg, 0.5, 0.5), 0xC0, 0x00, 0x00));
  auto own_bg = mv::codec::decode_pptx(deck, 1, nullptr, 320);
  REQUIRE(own_bg);
  CHECK(near(at(*own_bg, 0.5, 0.5), 0x00, 0xB0, 0x50));  // accent1
}

TEST_CASE("PPTX shapes are filled at their place; geometry and lumMod apply", "[pptx]") {
  // A blue rect in the left half, an ellipse in the right half whose corner
  // stays white, and a theme colour darkened by lumMod.
  const std::string shapes =
      fx::sp(0, 0, 480, 540, "0000FF") + fx::sp(480, 0, 480, 540, "FF0000", {}, "ellipse") +
      R"(<p:sp><p:nvSpPr><p:cNvPr id="9" name="d"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>)"
      R"(<a:xfrm><a:off x="0" y="6604000"/><a:ext cx="1270000" cy="254000"/></a:xfrm><a:prstGeom prst="rect"/>)"
      R"(<a:solidFill><a:schemeClr val="accent1"><a:lumMod val="50000"/></a:schemeClr></a:solidFill></p:spPr></p:sp>)";
  auto slide = mv::codec::decode_pptx(fx::pptx({fx::slide(shapes)}), 0, nullptr, 960);
  REQUIRE(slide);
  CHECK(near(at(*slide, 0.25, 0.5), 0, 0, 255));
  CHECK(near(at(*slide, 0.75, 0.5), 255, 0, 0));
  CHECK(near(at(*slide, 0.51, 0.02), 255, 255, 255));  // outside the ellipse
  const rgb dim = at(*slide, 0.05, 0.99);
  CHECK(dim.g > 60);
  CHECK(dim.g < 110);  // accent1 (00B050) at half its lightness
}

TEST_CASE("PPTX text: a title placeholder takes the master's place and size", "[pptx]") {
  // The slide's title has no xfrm: it sits where the master's title does
  // (36, 20, 888 x 80 pt), in its 44 pt.
  const std::string title = fx::sp(0, 0, 0, 0, "", fx::para("Quarterly results", "lang=\"en\""), "rect",
                                   R"(<p:ph type="title"/>)");
  auto slide = mv::codec::decode_pptx(fx::pptx({fx::slide(title)}), 0, nullptr, 960);
  REQUIRE(slide);
  CHECK(dark(*slide, 0.03, 0.03, 0.6, 0.19) > 300);  // the title's ink
  CHECK(dark(*slide, 0.0, 0.25, 1.0, 1.0) == 0);     // nothing below it
}

TEST_CASE("PPTX text: body bullets, alignment and the slide number", "[pptx]") {
  const std::string body = fx::sp(0, 0, 0, 0, "", fx::para("First point") + fx::para("Second point"), "rect",
                                  R"(<p:ph idx="1"/>)");
  // Centred text in a box on the right; and the slide number field.
  const std::string centred = fx::sp(480, 300, 480, 100, "", fx::para("Mid", R"(sz="4000")", R"(algn="ctr")"));
  const std::string number =
      R"(<p:sp><p:nvSpPr><p:cNvPr id="5" name="n"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="6096000"/>)"
      R"(<a:ext cx="2540000" cy="762000"/></a:xfrm><a:prstGeom prst="rect"/></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/>)"
      R"(<a:p><a:fld id="{1}" type="slidenum"><a:rPr sz="4000"/><a:t>&#8249;#&#8250;</a:t></a:fld></a:p></p:txBody></p:sp>)";
  const auto deck = fx::pptx({fx::slide(body + centred + number), fx::slide(number)});
  auto slide = mv::codec::decode_pptx(deck, 0, nullptr, 960);
  REQUIRE(slide);
  // The bullet hangs at the body's left (36 pt), the text after it at 36 + 18 pt.
  CHECK(dark(*slide, 0.035, 0.22, 0.06, 0.32) > 4);
  CHECK(dark(*slide, 0.06, 0.22, 0.3, 0.32) > 100);
  // Centred: ink about the box's middle (720 pt), none at its left edge.
  CHECK(dark(*slide, 0.70, 0.56, 0.80, 0.75) > 50);
  CHECK(dark(*slide, 0.50, 0.56, 0.62, 0.75) == 0);
  // The number field is the slide's own number, not the cached text.
  CHECK(dark(*slide, 0.0, 0.85, 0.2, 1.0) > 30);
  auto second = mv::codec::decode_pptx(deck, 1, nullptr, 960);
  REQUIRE(second);
  CHECK(dark(*second, 0.0, 0.85, 0.2, 1.0) > 30);
  CHECK(slide->rgba != second->rgba);
}

TEST_CASE("PPTX tables draw their cells' fills and text", "[pptx]") {
  const std::string table =
      R"(<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="4" name="t"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>)"
      R"(<p:xfrm><a:off x="1270000" y="1270000"/><a:ext cx="5080000" cy="1270000"/></p:xfrm><a:graphic><a:graphicData uri="t"><a:tbl>)"
      R"(<a:tblGrid><a:gridCol w="2540000"/><a:gridCol w="2540000"/></a:tblGrid><a:tr h="1270000">)"
      R"(<a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:rPr sz="2000"/><a:t>A</a:t></a:r></a:p></a:txBody><a:tcPr><a:solidFill><a:srgbClr val="FFC000"/></a:solidFill></a:tcPr></a:tc>)"
      R"(<a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:rPr sz="2000"/><a:t>Bee</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc>)"
      R"(</a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>)";
  auto slide = mv::codec::decode_pptx(fx::pptx({fx::slide(table)}), 0, nullptr, 960);
  REQUIRE(slide);
  CHECK(near(at(*slide, 0.20, 0.30), 0xFF, 0xC0, 0x00));     // the first cell's fill
  CHECK(near(at(*slide, 0.40, 0.30), 0xFF, 0xFF, 0xFF));     // the second has none
  CHECK(dark(*slide, 0.31, 0.18, 0.45, 0.30) > 20);          // "Bee"
}

TEST_CASE("decode_pptx_pages draws the slides asked for from one open", "[pptx][page-sidebar]") {
  const auto deck = fx::pptx({fx::slide(fx::sp(0, 0, 960, 540, "FF0000")), fx::slide(fx::sp(0, 0, 960, 540, "00FF00")),
                              fx::slide(fx::sp(0, 0, 960, 540, "0000FF"))});
  std::vector<mv::codec::raster> got;
  const std::vector<std::uint32_t> want = {2, 0, 7};
  auto count = mv::codec::decode_pages(deck, want, 200, nullptr, &got, [](void* u, mv::codec::raster&& r) {
    static_cast<std::vector<mv::codec::raster>*>(u)->push_back(std::move(r));
    return true;
  });
  REQUIRE(count);
  CHECK(count.value() == 3);
  REQUIRE(got.size() == 2);
  CHECK(got[0].page == 2);
  CHECK(near(at(got[0], 0.5, 0.5), 0, 0, 255));
  CHECK(got[1].page == 0);
  for (const auto& p : got) {
    auto one = mv::codec::decode_pptx(deck, p.page, nullptr, 200);
    REQUIRE(one);
    CHECK(one->rgba == p.rgba);
  }
}

TEST_CASE("Broken PPTX packages are errors, never crashes", "[pptx]") {
  const auto deck = fx::pptx({fx::slide(fx::para("x")), fx::slide("")});
  for (std::size_t cut : {deck.size() / 4, deck.size() / 2, deck.size() - 10}) {
    std::vector<std::uint8_t> part(deck.begin(), deck.begin() + static_cast<std::ptrdiff_t>(cut));
    (void)mv::codec::decode(part);  // any result but a crash
  }
  // A package with no presentation.
  const auto hollow = mv::test::docxfx::zip({{"ppt/slides/slide1.xml", "<x/>"}});
  CHECK(mv::codec::probe(hollow) == mv::codec::format_family::pptx);
  CHECK_FALSE(mv::codec::decode(hollow));
  // A slide that is not XML is a white page; the others still draw.
  auto broken = fx::pptx({fx::slide(fx::sp(0, 0, 960, 540, "FF0000")), "<p:sld><<<"});
  auto ok = mv::codec::decode_pptx(broken, 0, nullptr, 100);
  REQUIRE(ok);
  CHECK(near(at(*ok, 0.5, 0.5), 255, 0, 0));
  auto white = mv::codec::decode_pptx(broken, 1, nullptr, 100);
  REQUIRE(white);
  CHECK(near(at(*white, 0.5, 0.5), 255, 255, 255));
}

// Not run by default: `mv_tests "[.pptx-mutate]"` decodes thousands of
// mutations of a small deck (bytes of the package, then the slide's XML
// zipped again) and asks only that each returns. tools/fuzz/fuzz_pptx.cpp
// does the same under ASan on Windows.
TEST_CASE("PPTX mutations decode or fail, never crash", "[.pptx-mutate]") {
  const std::string body = fx::sp(0, 0, 0, 0, "", fx::para("Title"), "rect", R"(<p:ph type="title"/>)") +
                           fx::sp(0, 0, 0, 0, "", fx::para("One") + fx::para("Two", R"(sz="2000" b="1")", R"(lvl="1" algn="ctr")"),
                                  "rect", R"(<p:ph idx="1"/>)") +
                           fx::sp(100, 100, 200, 100, "00B050", fx::para("Box"), "roundRect") +
                           R"(<p:grpSp><p:nvGrpSpPr><p:cNvPr id="7" name="g"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>)"
                           R"(<a:xfrm><a:off x="0" y="0"/><a:ext cx="1270000" cy="1270000"/><a:chOff x="0" y="0"/><a:chExt cx="2540000" cy="2540000"/></a:xfrm></p:grpSpPr>)" +
                           fx::sp(10, 10, 50, 50, "FF0000", {}, "ellipse") + "</p:grpSp>";
  const auto seed = fx::pptx({fx::slide(body), fx::slide("", fx::solid_bg(R"(<a:schemeClr val="accent2"><a:tint val="50000"/></a:schemeClr>)"))});
  std::uint32_t state = 4242;
  auto rnd = [&state] {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };
  const auto mutate = [&](auto& bytes, const char* alphabet) {
    const int edits = 1 + static_cast<int>(rnd() % 8);
    for (int e = 0; e < edits && !bytes.empty(); ++e) {
      const std::size_t at = rnd() % bytes.size();
      switch (rnd() % 4) {
        case 0: bytes[at] = static_cast<std::decay_t<decltype(bytes[0])>>(rnd()); break;
        case 1: bytes[at] ^= static_cast<std::decay_t<decltype(bytes[0])>>(1u << (rnd() % 7)); break;
        case 2: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), alphabet[rnd() % 8]); break;
        default: if (bytes.size() > 64) bytes.resize(bytes.size() - rnd() % 32); break;
      }
    }
  };
  int decoded = 0;
  for (int i = 0; i < 3000; ++i) {
    auto bytes = seed;
    mutate(bytes, "<>/\"&=a:");
    if (mv::codec::decode_pptx(bytes, rnd() % 3, nullptr, 256)) ++decoded;
  }
  WARN(decoded << " of 3000 package mutations still decoded");
  decoded = 0;
  const std::string xml = fx::slide(body);
  for (int i = 0; i < 1500; ++i) {
    std::string s = xml;
    mutate(s, "<>/\"&=a:");
    if (mv::codec::decode_pptx(fx::pptx({s}), 0, nullptr, 256)) ++decoded;
  }
  WARN(decoded << " of 1500 XML mutations still decoded");
}
