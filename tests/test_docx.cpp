// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// DOCX (docs/plans/audio-and-documents.md §2.5): the zip and XML readers under
// it, and pages laid out and drawn by codec/ — text, headings, lists, tables,
// images, breaks — from hand-built packages (docx_fixture.h).
#include "catch_compat.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "codec/text.h"
#include "codec/xml.h"
#include "codec/zip.h"
#include "docx_fixture.h"
#include "image/pipeline.h"

using mv::status;
namespace fx = mv::test::docxfx;

namespace {

struct ink {
  std::size_t dark = 0, red = 0, blue = 0, black = 0;
};

ink count(const mv::codec::raster& r, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
  ink out;
  for (std::uint32_t y = y0; y < y1 && y < r.height; ++y) {
    for (std::uint32_t x = x0; x < x1 && x < r.width; ++x) {
      const std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * r.width + x) * 4;
      if (p[0] < 128 && p[1] < 128 && p[2] < 128) ++out.dark;
      if (p[0] < 40 && p[1] < 40 && p[2] < 40) ++out.black;
      if (p[0] > 150 && p[1] < 80 && p[2] < 80) ++out.red;
      if (p[0] < 80 && p[1] < 80 && p[2] > 150) ++out.blue;
    }
  }
  return out;
}

bool have_fonts() {
  mv::codec::text::engine e;
  return e.usable();
}

}  // namespace

TEST_CASE("The zip reader reads stored and deflated parts, and refuses bad ones", "[docx][zip]") {
  const auto bytes = fx::zip({{"a.txt", "hello"}, {"b.txt", std::string(5000, 'x'), true}});
  auto zip = mv::codec::zip::archive::open(bytes);
  REQUIRE(zip);
  REQUIRE(zip->entries().size() == 2);
  auto a = zip->read("a.txt");
  REQUIRE(a);
  CHECK(std::string(a->begin(), a->end()) == "hello");
  auto b = zip->read("B.TXT");  // part names are case-insensitive
  REQUIRE(b);
  CHECK(b->size() == 5000);
  CHECK_FALSE(zip->read("b.txt", 100));  // over the cap: refused before inflating
  CHECK_FALSE(zip->read("missing"));

  auto cut = bytes;
  cut.resize(cut.size() / 2);
  CHECK_FALSE(mv::codec::zip::archive::open(cut));
}

TEST_CASE("The XML reader decodes entities and refuses a DOCTYPE", "[docx][xml]") {
  using mv::codec::xml::reader;
  using mv::codec::xml::token;
  reader r(R"(<?xml version="1.0"?><!-- c --><a x="1 &amp; 2"><b/>t&lt;&#x41;&#66;<![CDATA[<c>]]></a>)");
  REQUIRE(r.next() == token::start);
  CHECK(r.name() == "a");
  CHECK(r.attr("x") == "1 & 2");
  REQUIRE(r.next() == token::start);
  CHECK(r.name() == "b");
  REQUIRE(r.next() == token::end);
  REQUIRE(r.next() == token::text);
  CHECK(r.text() == "t<AB");
  REQUIRE(r.next() == token::text);
  CHECK(r.text() == "<c>");
  REQUIRE(r.next() == token::end);
  CHECK(r.next() == token::eof);

  reader doctype(R"(<!DOCTYPE x [<!ENTITY a "aaaa">]><x>&a;</x>)");
  CHECK(doctype.next() == token::error);
  reader unknown(R"(<x>&nope;</x>)");
  REQUIRE(unknown.next() == token::start);
  CHECK(unknown.next() == token::error);

  // Many comments are a loop, not a recursion.
  std::string many = "<x>";
  for (int i = 0; i < 200000; ++i) many += "<!---->";
  many += "</x>";
  reader deep(many);
  REQUIRE(deep.next() == token::start);
  CHECK(deep.next() == token::end);
}

TEST_CASE("DOCX is recognised as a zip with word/ parts", "[docx][probe]") {
  const auto doc = fx::docx(fx::para("Hello"));
  CHECK(mv::codec::probe(doc) == mv::codec::format_family::docx);
  CHECK(std::string(mv::codec::format_name(mv::codec::format_family::docx)) == "DOCX");
  const auto other = fx::zip({{"readme.txt", "not a document"}});
  CHECK(mv::codec::probe(other) == mv::codec::format_family::unknown);
  // A package with no main document is not drawn.
  const auto hollow = fx::zip({{"[Content_Types].xml", fx::kContentTypes}});
  CHECK(mv::codec::probe(hollow) == mv::codec::format_family::docx);
  CHECK_FALSE(mv::codec::decode(hollow));
}

TEST_CASE("A DOCX page draws its text, headings in their style", "[docx]") {
  if (!have_fonts()) SKIP("no readable system font");
  const auto doc = fx::docx(fx::para("Title here", R"(<w:pStyle w:val="Heading1"/>)") +
                            fx::para("Body text that is plain and black, on a white page."));
  auto page = mv::codec::decode(doc);
  REQUIRE(page);
  CHECK(page->format == mv::codec::format_family::docx);
  CHECK(page->page_count == 1);
  CHECK(page->height == mv::codec::kDocxLongEdge);           // US Letter, portrait
  CHECK(page->width == 2473);                               // 612 / 792 of 3200
  // Margins are white; the heading is red, the body below it dark.
  const ink margin = count(*page, 0, 0, page->width, 280);
  CHECK(margin.dark == 0);
  // The 24 pt heading's line spans about 291–417 px (its space before is
  // dropped at the top of the page); the body line follows 8 pt later.
  const ink heading = count(*page, 280, 285, page->width - 280, 430);
  CHECK(heading.red > 500);
  const ink body = count(*page, 280, 440, page->width - 280, 560);
  CHECK(body.dark > 500);
  CHECK(body.red == 0);
}

TEST_CASE("DOCX pages break where the text runs out, the same at every size", "[docx][pages]") {
  if (!have_fonts()) SKIP("no readable system font");
  std::string body;
  for (int i = 0; i < 120; ++i) body += fx::para("Paragraph " + std::to_string(i) + " with some words in it.");
  body += R"(<w:p><w:r><w:br w:type="page"/></w:r></w:p>)" + fx::para("After the break.");
  const auto doc = fx::docx(body);

  auto first = mv::codec::decode(doc);
  REQUIRE(first);
  CHECK(first->page_count >= 3);
  const std::uint32_t pages = first->page_count;
  auto preview = mv::image::decode_preview(doc);
  REQUIRE(preview);
  CHECK(preview->width <= mv::codec::kDocxPreviewEdge);
  auto small = mv::codec::decode_docx(doc, 0, nullptr, 512);
  REQUIRE(small);
  CHECK(small->page_count == pages);  // layout is in points, not pixels

  auto last = mv::codec::decode(doc, nullptr, 4, pages - 1);
  REQUIRE(last);
  CHECK(last->page == pages - 1);
  CHECK(count(*last, 0, 0, last->width, last->height).dark > 100);  // "After the break."
  auto past = mv::codec::decode(doc, nullptr, 4, pages);
  REQUIRE_FALSE(past);
  CHECK(past.error() == status::invalid_arg);
}

TEST_CASE("A DOCX table draws its grid; a list draws its numbers", "[docx]") {
  if (!have_fonts()) SKIP("no readable system font");
  const std::string table =
      R"(<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/></w:tblPr><w:tblGrid><w:gridCol w:w="4680"/><w:gridCol w:w="4680"/></w:tblGrid>)"
      R"(<w:tr><w:tc>)" + fx::para("A1") + R"(</w:tc><w:tc>)" + fx::para("B1") + R"(</w:tc></w:tr>)"
      R"(<w:tr><w:tc>)" + fx::para("A2") + R"(</w:tc><w:tc>)" + fx::para("B2") + R"(</w:tc></w:tr></w:tbl>)";
  const auto doc = fx::docx(table);
  auto page = mv::codec::decode(doc);
  REQUIRE(page);
  // Grid rules: a full-width black line near the top margin (72 pt = 291 px).
  std::size_t rule_rows = 0;
  for (std::uint32_t y = 280; y < 320; ++y) {
    if (count(*page, 400, y, page->width - 400, y + 1).black > (page->width - 800) * 9 / 10) ++rule_rows;
  }
  CHECK(rule_rows >= 1);

  const std::string list =
      fx::para("First", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>)") +
      fx::para("Second", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>)");
  auto listed = mv::codec::decode(fx::docx(list));
  REQUIRE(listed);
  // The number hangs left of the 0.5" indent: ink between 1" + 0.25" and 1" + 0.5".
  const double px_per_pt = 3200.0 / 792.0;
  const auto label_x0 = static_cast<std::uint32_t>((72 + 18) * px_per_pt);
  const auto label_x1 = static_cast<std::uint32_t>((72 + 36) * px_per_pt);
  CHECK(count(*listed, label_x0, 280, label_x1, 400).dark > 50);
}

TEST_CASE("A DOCX draws its inline pictures at their extent", "[docx]") {
  if (!have_fonts()) SKIP("no readable system font");
  std::vector<std::uint8_t> rgba(64 * 64 * 4);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = 0;
    rgba[i + 1] = 0;
    rgba[i + 2] = 255;
    rgba[i + 3] = 255;
  }
  auto jpeg = mv::codec::encode_jpeg_rgba(rgba, 64, 64, 95);
  REQUIRE(jpeg);
  const std::string rels =
      R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
      R"(<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/image1.jpeg"/>)"
      R"(</Relationships>)";
  // 2 x 1 inch (1828800 x 914400 EMU).
  const std::string drawing =
      R"(<w:p><w:r><w:drawing><wp:inline xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">)"
      R"(<wp:extent cx="1828800" cy="914400"/><a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:graphicData>)"
      R"(<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:blipFill><a:blip r:embed="rId9"/></pic:blipFill></pic:pic>)"
      R"(</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>)";
  const auto doc = fx::docx(drawing, {{"word/_rels/document.xml.rels", rels},
                                      {"word/media/image1.jpeg", std::string(jpeg->begin(), jpeg->end())}});
  auto page = mv::codec::decode(doc);
  REQUIRE(page);
  const double px_per_pt = 3200.0 / 792.0;
  // The picture's middle: 1" + 1" across, 1" + 0.5" down.
  const auto cx = static_cast<std::uint32_t>(144 * px_per_pt);
  const auto cy = static_cast<std::uint32_t>(108 * px_per_pt);
  CHECK(count(*page, cx - 20, cy - 20, cx + 20, cy + 20).blue > 1500);
  // And nothing blue past its right edge.
  const auto past = static_cast<std::uint32_t>(220 * px_per_pt);
  CHECK(count(*page, past, cy - 20, past + 40, cy + 20).blue == 0);
}

TEST_CASE("A broken DOCX is an error, not a crash", "[docx]") {
  auto bytes = fx::docx(fx::para("x"));
  bytes.resize(bytes.size() - 30);  // the end of central directory goes
  CHECK_FALSE(mv::codec::decode(bytes));
  const auto bad_xml = fx::docx("<w:p><w:r><w:t>unclosed");
  auto r = mv::codec::decode(bad_xml);
  // Unreadable XML still yields what was read (here: one empty page) or an error.
  if (r) CHECK(r->page_count == 1);
}

// Not a check: `MV_DOCX_DUMP=out.jpg mv_tests "[.docx-dump]"` writes a sample
// page (or MV_DOCX_FILE's page MV_DOCX_PAGE) to look at by eye.
TEST_CASE("DOCX sample page, written for a look", "[.docx-dump]") {
  const char* out = std::getenv("MV_DOCX_DUMP");
  if (!out) SKIP("set MV_DOCX_DUMP");
  std::vector<std::uint8_t> doc;
  if (const char* in = std::getenv("MV_DOCX_FILE")) {
    std::ifstream f(in, std::ios::binary);
    doc.assign(std::istreambuf_iterator<char>(f), {});
  } else {
    const std::string body =
        fx::para("Title here", R"(<w:pStyle w:val="Heading1"/>)") +
        fx::para("Body text that is plain and black, on a white page. It goes on for a while so that "
                 "it wraps onto a second line, and the paragraph is justified.",
                 R"(<w:jc w:val="both"/>)") +
        fx::para("Bold and italic", {}, "<w:b/><w:i/>") +
        fx::para("First item", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>)") +
        fx::para("Second item", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>)") +
        fx::para("A bullet", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="2"/></w:numPr>)") +
        R"(<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/></w:tblPr><w:tblGrid><w:gridCol w:w="4680"/><w:gridCol w:w="4680"/></w:tblGrid>)"
        R"(<w:tr><w:tc>)" + fx::para("A1") + R"(</w:tc><w:tc>)" +
        fx::para("B1 has more text in it than fits on one line of its cell") + R"(</w:tc></w:tr></w:tbl>)" +
        fx::para("Centered, underlined, \xE6\xBC\xA2\xE5\xAD\x97 and \xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC",
                 R"(<w:jc w:val="center"/>)", R"(<w:u w:val="single"/>)");
    doc = fx::docx(body);
  }
  const char* page_env = std::getenv("MV_DOCX_PAGE");
  const auto page = static_cast<std::uint32_t>(page_env ? std::atoi(page_env) : 0);
  auto r = mv::codec::decode(doc, nullptr, 4, page);
  REQUIRE(r);
  WARN("page " << (r->page + 1) << " of " << r->page_count << ", " << r->width << "x" << r->height);
  auto jpeg = mv::codec::encode_jpeg_rgba(r->rgba, r->width, r->height, 85);
  REQUIRE(jpeg);
  std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(jpeg->data()),
                                             static_cast<std::streamsize>(jpeg->size()));
}

// Not run by default: `mv_tests "[.docx-mutate]"` decodes thousands of
// mutations of a small package (byte flips, inserts, truncation, inside the
// stored XML) and asks only that each returns. The Windows broken-corpus sweep
// and tools/fuzz/fuzz_docx.cpp do the same under ASan.
TEST_CASE("DOCX mutations decode or fail, never crash", "[.docx-mutate]") {
  std::vector<fx::part> parts = {
      {"[Content_Types].xml", fx::kContentTypes},
      {"word/document.xml",
       fx::document(fx::para("Title", R"(<w:pStyle w:val="Heading1"/>)") +
                    fx::para("Item", R"(<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>)") +
                    R"(<w:tbl><w:tblGrid><w:gridCol w:w="2000"/></w:tblGrid><w:tr><w:tc>)" + fx::para("A") +
                    R"(</w:tc></w:tr></w:tbl>)")},
      {"word/styles.xml", fx::kStyles},
      {"word/numbering.xml", fx::kNumbering},
  };
  const auto seed = fx::zip(parts);
  std::uint32_t state = 12345;
  auto rnd = [&state] {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };
  int decoded = 0;
  for (int i = 0; i < 3000; ++i) {
    auto bytes = seed;
    const int edits = 1 + static_cast<int>(rnd() % 8);
    for (int e = 0; e < edits; ++e) {
      const std::size_t at = rnd() % bytes.size();
      switch (rnd() % 4) {
        case 0: bytes[at] = static_cast<std::uint8_t>(rnd()); break;
        case 1: bytes[at] ^= static_cast<std::uint8_t>(1u << (rnd() % 8)); break;
        case 2: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::uint8_t>("<>/\"&=w:"[rnd() % 8])); break;
        default: if (bytes.size() > 64) bytes.resize(bytes.size() - rnd() % 32); break;
      }
    }
    auto r = mv::codec::decode_docx(bytes, rnd() % 3, nullptr, 256);
    if (r) ++decoded;
  }
  WARN(decoded << " of 3000 mutations still decoded");
}
