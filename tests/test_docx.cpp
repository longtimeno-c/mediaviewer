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
#include "codec/doclayout.h"
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

constexpr double kPx = 3200.0 / 792.0;  // a Letter page's pixels per point at kDocxLongEdge

std::uint32_t px(double pt) { return static_cast<std::uint32_t>(pt * kPx); }

// Ink in a rectangle given in points.
ink count_pt(const mv::codec::raster& r, double x0, double y0, double x1, double y1) {
  return count(r, px(x0), px(y0), px(x1), px(y1));
}

// A solid-colour JPEG, `w` x `h`.
std::string solid_jpeg(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint32_t w = 64, std::uint32_t h = 64) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
  }
  auto jpeg = mv::codec::encode_jpeg_rgba(rgba, w, h, 95);
  return jpeg ? std::string(jpeg->begin(), jpeg->end()) : std::string{};
}

bool have_fonts() {
  mv::codec::text::engine e;
  return e.usable();
}

}  // namespace

TEST_CASE("Image installs the document font reader", "[docx][text]") {
  // Referencing the pipeline keeps image/pipeline.cpp, and its font-reader
  // installer, in this binary.
  (void)mv::image::decode_bytes({});
  REQUIRE(mv::codec::text::font_reader_installed());
  if (!have_fonts()) SKIP("no readable system font");
  mv::codec::text::engine e;
  const auto word = e.shape("Hi", {});
  CHECK(word.font >= 0);
}

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

TEST_CASE("DOCX headers and footers draw on every page with their page fields", "[docx][furniture]") {
  if (!have_fonts()) SKIP("no readable system font");
  // Eleven pages. The header (red) is NUMPAGES, the footer (blue) PAGE: on
  // the first page "11" over "1", on the last "11" over "11".
  std::string body = fx::para("First page");
  for (int i = 2; i <= 11; ++i) body += fx::page_break() + fx::para("Page " + std::to_string(i));
  body += fx::sect(R"(<w:headerReference w:type="default" r:id="rId10"/><w:footerReference w:type="default" r:id="rId11"/>)");
  const std::string big = R"(<w:sz w:val="48"/>)";
  const std::string header = fx::story(
      "hdr", R"(<w:p><w:fldSimple w:instr=" NUMPAGES \* MERGEFORMAT "><w:r><w:rPr><w:color w:val="FF0000"/>)" + big +
                 R"(</w:rPr><w:t>99</w:t></w:r></w:fldSimple></w:p>)");
  const std::string footer = fx::story(
      "ftr", R"(<w:p><w:r><w:rPr><w:color w:val="0000FF"/>)" + big + R"(</w:rPr><w:fldChar w:fldCharType="begin"/></w:r>)"
             R"(<w:r><w:instrText xml:space="preserve"> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r>)"
             R"(<w:r><w:rPr><w:color w:val="0000FF"/>)" + big + R"(</w:rPr><w:t>99</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>)");
  const auto doc = fx::docx(body, {{"word/_rels/document.xml.rels", fx::rels({{"rId10", "header1.xml"}, {"rId11", "footer1.xml"}})},
                                   {"word/header1.xml", header},
                                   {"word/footer1.xml", footer}});
  auto first = mv::codec::decode(doc);
  REQUIRE(first);
  REQUIRE(first->page_count == 11);
  auto last = mv::codec::decode(doc, nullptr, 4, 10);
  REQUIRE(last);
  for (const auto* page : {&*first, &*last}) {
    // The header sits from 0.5" in the top margin, the footer ends 0.5" above the edge.
    CHECK(count_pt(*page, 0, 72, 612, 792).red == 0);
    CHECK(count_pt(*page, 0, 0, 612, 72).blue == 0);
    CHECK(count_pt(*page, 0, 720, 612, 792).red == 0);
  }
  const std::size_t h1 = count_pt(*first, 72, 30, 300, 72).red, f1 = count_pt(*first, 72, 700, 300, 760).blue;
  const std::size_t h11 = count_pt(*last, 72, 30, 300, 72).red, f11 = count_pt(*last, 72, 700, 300, 760).blue;
  CHECK(h1 > 500);
  CHECK(f1 > 200);
  CHECK(h1 > f1 * 3 / 2);  // "11" over "1"
  CHECK(f11 > f1 * 3 / 2);  // "11" on the last page
  CHECK(h11 == h1);         // the same count on every page; the cached "99" never drawn
}

TEST_CASE("DOCX first-page and even headers follow titlePg and evenAndOddHeaders", "[docx][furniture]") {
  if (!have_fonts()) SKIP("no readable system font");
  std::string body = fx::para("One") + fx::page_break() + fx::para("Two") + fx::page_break() + fx::para("Three");
  body += fx::sect(R"(<w:headerReference w:type="default" r:id="rId1"/><w:headerReference w:type="first" r:id="rId2"/>)"
                   R"(<w:headerReference w:type="even" r:id="rId3"/><w:titlePg/>)");
  const auto colour_header = [](const std::string& rgb) {
    return fx::story("hdr", fx::para("Header text here", {}, R"(<w:color w:val=")" + rgb + R"("/>)"));
  };
  const auto doc = fx::docx(body, {{"word/_rels/document.xml.rels", fx::rels({{"rId1", "header1.xml"}, {"rId2", "header2.xml"}, {"rId3", "header3.xml"}})},
                                   {"word/header1.xml", colour_header("FF0000")},
                                   {"word/header2.xml", colour_header("0000FF")},
                                   {"word/header3.xml", colour_header("000000")},
                                   {"word/settings.xml", R"(<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:evenAndOddHeaders/></w:settings>)"}});
  auto p0 = mv::codec::decode(doc, nullptr, 4, 0);
  auto p1 = mv::codec::decode(doc, nullptr, 4, 1);
  auto p2 = mv::codec::decode(doc, nullptr, 4, 2);
  REQUIRE(p0);
  REQUIRE(p1);
  REQUIRE(p2);
  CHECK(count_pt(*p0, 0, 0, 612, 72).blue > 100);   // first
  CHECK(count_pt(*p1, 0, 0, 612, 72).black > 100);  // page 2: even
  CHECK(count_pt(*p1, 0, 0, 612, 72).red == 0);
  CHECK(count_pt(*p2, 0, 0, 612, 72).red > 100);    // page 3: default
}

TEST_CASE("A DOCX footnote sits at the foot of the page that cites it", "[docx][notes]") {
  if (!have_fonts()) SKIP("no readable system font");
  const std::string body = fx::para("Page one has no note.") + fx::page_break() +
                           fx::para_with_note("This sentence cites a note", 1) + fx::para("More body text.") + fx::sect();
  const auto doc = fx::docx(body, {{"word/footnotes.xml", fx::notes("footnotes", {{1, fx::note_para("The note, in blue.", "0000FF")}})}});
  auto p0 = mv::codec::decode(doc, nullptr, 4, 0);
  auto p1 = mv::codec::decode(doc, nullptr, 4, 1);
  REQUIRE(p0);
  REQUIRE(p1);
  REQUIRE(p1->page_count == 2);
  CHECK(count(*p0, 0, 0, p0->width, p0->height).blue == 0);
  // The note: blue, above the 1" bottom margin and below everything else.
  CHECK(count_pt(*p1, 72, 660, 540, 720).blue > 300);
  CHECK(count_pt(*p1, 0, 0, 612, 640).blue == 0);
  // The separator: a short black rule from the left margin, over the note.
  bool rule = false;
  for (double y = 640; y < 710 && !rule; y += 0.25) {
    const ink row = count_pt(*p1, 72, y, 200, y + 0.25);
    rule = row.black > static_cast<std::size_t>(px(120));
  }
  CHECK(rule);
  // The reference mark in the body is small and raised: ink just above the
  // body line's x-height, right of the text, on the first body line.
  CHECK(count_pt(*p1, 72, 72, 540, 100).dark > 300);
}

TEST_CASE("DOCX footnotes that do not fit move to the next page", "[docx][notes]") {
  if (!have_fonts()) SKIP("no readable system font");
  // A note about 40 lines long cited at the very bottom of a full page.
  std::string note;
  for (int i = 0; i < 40; ++i) note += fx::note_para("Long note line " + std::to_string(i), "0000FF");
  std::string body;
  for (int i = 0; i < 26; ++i) body += fx::para("Filler paragraph " + std::to_string(i));
  body += fx::para_with_note("Cites the long note", 1) + fx::sect();
  const auto doc = fx::docx(body, {{"word/footnotes.xml", fx::notes("footnotes", {{1, note}})}});
  auto first = mv::codec::decode(doc);
  REQUIRE(first);
  CHECK(first->page_count >= 2);
  std::size_t blue = 0;
  for (std::uint32_t p = 0; p < first->page_count; ++p) {
    auto page = mv::codec::decode(doc, nullptr, 4, p);
    REQUIRE(page);
    blue += count(*page, 0, 0, page->width, page->height).blue;
  }
  CHECK(blue > 1000);  // drawn somewhere, never lost
}

TEST_CASE("A two-column DOCX section fills both columns", "[docx][columns]") {
  if (!have_fonts()) SKIP("no readable system font");
  std::string body;
  for (int i = 0; i < 40; ++i) body += fx::para("Column line " + std::to_string(i));
  body += fx::sect(R"(<w:cols w:num="2" w:space="720"/>)");
  auto page = mv::codec::decode(fx::docx(body));
  REQUIRE(page);
  CHECK(page->page_count == 1);
  // Columns: 72-288 pt and 324-540 pt; the gutter between stays empty.
  CHECK(count_pt(*page, 72, 72, 288, 720).dark > 2000);
  CHECK(count_pt(*page, 324, 72, 540, 720).dark > 2000);
  CHECK(count_pt(*page, 290, 0, 322, 792).dark == 0);

  // A column break sends what follows to the top of the next column.
  const auto broken = fx::docx(fx::para("Left") + R"(<w:p><w:r><w:br w:type="column"/><w:t>Right</w:t></w:r></w:p>)" +
                               fx::sect(R"(<w:cols w:num="2" w:space="720" w:sep="1"/>)"));
  auto b = mv::codec::decode(broken);
  REQUIRE(b);
  CHECK(count_pt(*b, 324, 72, 540, 100).dark > 100);
  // The separator line runs down the gutter's middle.
  CHECK(count_pt(*b, 305, 75, 307, 95).black > 0);
}

TEST_CASE("DOCX columns before a continuous break are balanced", "[docx][columns]") {
  if (!have_fonts()) SKIP("no readable system font");
  std::string body = R"(<w:p><w:pPr>)" + fx::sect() + R"(</w:pPr><w:r><w:t>Intro, one column</w:t></w:r></w:p>)";
  for (int i = 0; i < 19; ++i) body += fx::para("Balanced line " + std::to_string(i));
  body += R"(<w:p><w:pPr>)" + fx::sect(R"(<w:type w:val="continuous"/><w:cols w:num="2" w:space="720"/>)") +
          R"(</w:pPr><w:r><w:t>Last of the columns</w:t></w:r></w:p>)";
  body += fx::para("After the columns, full width", {}, R"(<w:color w:val="FF0000"/>)") + fx::sect(R"(<w:type w:val="continuous"/>)");
  auto page = mv::codec::decode(fx::docx(body));
  REQUIRE(page);
  CHECK(page->page_count == 1);
  // Twenty paragraphs, ten a column: the right column starts beside the left.
  CHECK(count_pt(*page, 324, 90, 540, 130).dark > 200);
  // And the red line follows the shorter, balanced columns, not twenty lines.
  std::uint32_t red_top = 0;
  for (std::uint32_t y = 0; y < page->height && red_top == 0; ++y) {
    if (count(*page, 0, y, page->width, y + 1).red > 0) red_top = y;
  }
  CHECK(red_top > px(200));
  CHECK(red_top < px(400));
}

TEST_CASE("A DOCX text box draws its shape and its text", "[docx][shapes]") {
  if (!have_fonts()) SKIP("no readable system font");
  // A red 3" x 1" box at 1" / 5" from the page's corner, white text in it.
  const std::string box = fx::shape("rect", "FF0000", {}, fx::para("Text inside the box", {}, R"(<w:color w:val="FFFFFF"/><w:sz w:val="32"/>)"));
  const std::string ellipse = fx::shape("ellipse", "0000FF", "000000");
  const std::string body = "<w:p>" + fx::anchor(box, 914400, 4572000, 2743200, 914400, "<wp:wrapNone/>", "page", "page") +
                           fx::anchor(ellipse, 914400, 6400800, 1828800, 914400, "<wp:wrapNone/>", "page", "page") +
                           "<w:r><w:t>Body</w:t></w:r></w:p>" + fx::sect();
  auto page = mv::codec::decode(fx::docx(body));
  REQUIRE(page);
  const ink fill = count_pt(*page, 75, 363, 285, 429);
  CHECK(fill.red > static_cast<std::size_t>(px(200) * px(55)));  // mostly red
  // The text: white pixels inside the red box, near its top.
  std::size_t white = 0;
  for (std::uint32_t y = px(364); y < px(400); ++y) {
    for (std::uint32_t x = px(80); x < px(280); ++x) {
      const std::uint8_t* p = page->rgba.data() + (static_cast<std::size_t>(y) * page->width + x) * 4;
      if (p[0] > 220 && p[1] > 220 && p[2] > 220) ++white;
    }
  }
  CHECK(white > 500);
  CHECK(count_pt(*page, 75, 434, 285, 500).red == 0);  // nothing past the box
  // The ellipse: blue at its centre, white in its bounding box's corners.
  CHECK(count_pt(*page, 130, 523, 150, 537).blue > 1000);
  CHECK(count_pt(*page, 73, 505, 80, 510).blue == 0);
  CHECK(count_pt(*page, 72, 504, 216, 576).black > 100);  // its outline
}

TEST_CASE("A DOCX VML text box (mc:Fallback) draws too", "[docx][shapes]") {
  if (!have_fonts()) SKIP("no readable system font");
  const std::string body =
      R"(<w:p><w:r><mc:AlternateContent><mc:Choice Requires="wpg"><w:t>never</w:t></mc:Choice><mc:Fallback><w:pict>)"
      R"(<v:rect style="position:absolute;margin-left:72pt;margin-top:360pt;width:216pt;height:72pt;mso-position-horizontal-relative:page;mso-position-vertical-relative:page" fillcolor="#ff0000" stroked="f">)"
      R"(<v:textbox><w:txbxContent>)" + fx::para("VML box", {}, R"(<w:color w:val="FFFFFF"/>)") +
      R"(</w:txbxContent></v:textbox><w10:wrap type="none"/></v:rect></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>)" + fx::sect();
  auto page = mv::codec::decode(fx::docx(body));
  REQUIRE(page);
  CHECK(count_pt(*page, 75, 363, 285, 429).red > static_cast<std::size_t>(px(200) * px(50)));
}

TEST_CASE("A floating DOCX picture with square wrap pushes text aside", "[docx][floats]") {
  if (!have_fonts()) SKIP("no readable system font");
  const std::string jpeg = solid_jpeg(0, 0, 255);
  REQUIRE_FALSE(jpeg.empty());
  std::string words;
  for (int i = 0; i < 60; ++i) words += "wrapping words go around ";
  const auto make = [&](const std::string& wrap) {
    // A 2" square at the column's left edge, from the paragraph's top.
    const std::string body = "<w:p>" + fx::anchor(fx::picture("rId9"), 0, 0, 1828800, 1828800, wrap) +
                             R"(<w:r><w:t xml:space="preserve">)" + words + "</w:t></w:r></w:p>" + fx::sect();
    return fx::docx(body, {{"word/_rels/document.xml.rels", fx::rels({{"rId9", "media/image1.jpeg"}})},
                           {"word/media/image1.jpeg", jpeg}});
  };
  auto square = mv::codec::decode(make(R"(<wp:wrapSquare wrapText="bothSides"/>)"));
  REQUIRE(square);
  CHECK(count_pt(*square, 80, 80, 208, 208).blue > static_cast<std::size_t>(px(120) * px(120)));  // the picture
  CHECK(count_pt(*square, 72, 72, 214, 214).dark == 0);   // no text over it
  CHECK(count_pt(*square, 220, 72, 540, 210).dark > 2000);  // text beside it
  CHECK(count_pt(*square, 72, 222, 200, 300).dark > 500);   // and under it, from the margin

  auto band = mv::codec::decode(make("<wp:wrapTopAndBottom/>"));
  REQUIRE(band);
  CHECK(count_pt(*band, 220, 72, 540, 214).dark == 0);  // nothing beside it
  CHECK(count_pt(*band, 72, 220, 540, 300).dark > 1000);

  auto none = mv::codec::decode(make("<wp:wrapNone/>"));
  REQUIRE(none);
  CHECK(count_pt(*none, 220, 72, 540, 100).dark > 200);  // text runs as if it were not there
}

TEST_CASE("A right-to-left DOCX paragraph aligns right and reorders", "[docx][rtl]") {
  if (!have_fonts()) SKIP("no readable system font");
  auto page = mv::codec::decode(fx::docx(fx::para("Short line", R"(<w:bidi/>)") + fx::para("Short line") + fx::sect()));
  REQUIRE(page);
  // The bidi paragraph's first line hugs the right margin; the plain one the left.
  CHECK(count_pt(*page, 72, 72, 300, 88).dark == 0);
  CHECK(count_pt(*page, 450, 72, 540, 88).dark > 100);
  CHECK(count_pt(*page, 72, 95, 200, 115).dark > 100);
  CHECK(count_pt(*page, 400, 95, 540, 115).dark == 0);

  // Hebrew in a right-to-left paragraph is drawn (its fallback font found).
  auto hebrew = mv::codec::decode(fx::docx(
      fx::para("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D 123", R"(<w:bidi/>)") + fx::sect()));
  REQUIRE(hebrew);
  CHECK(count_pt(*hebrew, 400, 72, 540, 90).dark > 100);
}

TEST_CASE("The bidi subset orders a line's pieces", "[docx][rtl]") {
  using mv::codec::doclayout::bidi_class;
  using mv::codec::doclayout::visual_order;
  using B = bidi_class;
  // Hebrew words and a number in a right-to-left paragraph: the number
  // reads left to right at the left end.
  const B rtl_line[] = {B::R, B::WS, B::R, B::WS, B::EN};
  CHECK(visual_order(rtl_line, 1) == std::vector<std::size_t>{4, 3, 2, 1, 0});
  // Hebrew inside an English sentence: only the Hebrew run turns round.
  const B ltr_line[] = {B::L, B::WS, B::R, B::WS, B::R, B::WS, B::L};
  CHECK(visual_order(ltr_line, 0) == std::vector<std::size_t>{0, 1, 4, 3, 2, 5, 6});
  // English words in a right-to-left paragraph keep their order, at the right.
  const B latin[] = {B::L, B::WS, B::L};
  CHECK(visual_order(latin, 1) == std::vector<std::size_t>{0, 1, 2});
  // Digits after Arabic are Arabic numbers; trailing space goes to the end.
  const B arabic[] = {B::AL, B::WS, B::EN, B::WS};
  std::vector<int> levels;
  CHECK(visual_order(arabic, 0, &levels) == std::vector<std::size_t>{2, 1, 0, 3});
  CHECK(levels == std::vector<int>{1, 1, 2, 0});

  CHECK(mv::codec::doclayout::classify("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D") == B::R);
  CHECK(mv::codec::doclayout::classify("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85") == B::AL);
  CHECK(mv::codec::doclayout::classify("2026") == B::EN);
  CHECK(mv::codec::doclayout::classify("(!)") == B::ON);
  // A word is split where its direction turns.
  const auto toks = mv::codec::doclayout::tokens_of("abc\xD7\xA9\xD7\x9C" "12 x");
  CHECK(toks == std::vector<std::string>{"abc", "\xD7\xA9\xD7\x9C", "12", " ", "x"});
}

TEST_CASE("DOCX pages drawn together match pages drawn one by one", "[docx][pages]") {
  if (!have_fonts()) SKIP("no readable system font");
  const auto doc = fx::report(solid_jpeg(40, 160, 60));
  struct got {
    std::vector<mv::codec::raster> pages;
  } sink;
  const std::uint32_t want[] = {2, 0, 99};
  auto count = mv::codec::decode_docx_pages(doc, want, 1024, nullptr, &sink, [](void* user, mv::codec::raster&& r) {
    static_cast<got*>(user)->pages.push_back(std::move(r));
    return true;
  });
  REQUIRE(count);
  REQUIRE(*count >= 3);
  REQUIRE(sink.pages.size() == 2);  // page 99 is past the end: skipped
  CHECK(sink.pages[0].page == 2);
  CHECK(sink.pages[1].page == 0);
  for (const auto& r : sink.pages) {
    CHECK(r.page_count == *count);
    auto one = mv::codec::decode_docx(doc, r.page, nullptr, 1024);
    REQUIRE(one);
    CHECK(one->width == r.width);
    CHECK(one->height == r.height);
    CHECK(one->rgba == r.rgba);
  }
  // A sink that says stop gets one page.
  int calls = 0;
  auto stopped = mv::codec::decode_docx_pages(doc, want, 256, nullptr, &calls, [](void* user, mv::codec::raster&&) {
    ++*static_cast<int*>(user);
    return false;
  });
  REQUIRE(stopped);
  CHECK(calls == 1);
}

TEST_CASE("A DOCX with headers, footers, footnotes and two columns shows all of them", "[docx][verify]") {
  if (!have_fonts()) SKIP("no readable system font");
  const auto doc = fx::report(solid_jpeg(40, 160, 60));
  auto first = mv::codec::decode(doc);
  REQUIRE(first);
  REQUIRE(first->page_count >= 2);
  for (std::uint32_t p = 0; p < first->page_count; ++p) {
    auto page = mv::codec::decode(doc, nullptr, 4, p);
    REQUIRE(page);
    CHECK(count_pt(*page, 300, 30, 540, 60).red > 200);   // the header, right-aligned
    CHECK(count_pt(*page, 250, 735, 360, 760).dark > 200);  // "Page N of M", centred
  }
  // Page 1: both footnotes at its foot, the two columns side by side.
  CHECK(count_pt(*first, 72, 600, 540, 720).blue > 500);
  CHECK(count_pt(*first, 0, 0, 612, 560).blue == 0);
}

// Not a check: `MV_DOCX_DUMP=out.jpg mv_tests "[.docx-dump]"` writes a sample
// page (or MV_DOCX_FILE's page MV_DOCX_PAGE; MV_DOCX_REPORT=1 the verify
// report, MV_DOCX_SAVE=report.docx keeping its bytes) to look at by eye.
TEST_CASE("DOCX sample page, written for a look", "[.docx-dump]") {
  const char* out = std::getenv("MV_DOCX_DUMP");
  if (!out) SKIP("set MV_DOCX_DUMP");
  std::vector<std::uint8_t> doc;
  if (const char* in = std::getenv("MV_DOCX_FILE")) {
    std::ifstream f(in, std::ios::binary);
    doc.assign(std::istreambuf_iterator<char>(f), {});
  } else if (std::getenv("MV_DOCX_REPORT")) {
    doc = fx::report(solid_jpeg(40, 160, 60));  // the issue #429 verify file
    if (const char* save = std::getenv("MV_DOCX_SAVE")) {
      std::ofstream(save, std::ios::binary).write(reinterpret_cast<const char*>(doc.data()), static_cast<std::streamsize>(doc.size()));
    }
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
                    R"(</w:tc></w:tr></w:tbl>)" + fx::para_with_note("Noted", 1) +
                    R"(<w:p><w:pPr>)" + fx::sect(R"(<w:headerReference w:type="default" r:id="rId1"/><w:cols w:num="2" w:sep="1"/>)") +
                    R"(</w:pPr></w:p>)" + "<w:p>" +
                    fx::anchor(fx::shape("ellipse", "FF0000", "000000", fx::para("Box")), 0, 0, 900000, 500000) +
                    R"(<w:r><w:t>Wrapped text beside it</w:t></w:r></w:p>)" +
                    fx::para("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D (1) abc", R"(<w:bidi/>)") +
                    fx::sect(R"(<w:type w:val="continuous"/>)"))},
      {"word/styles.xml", fx::kStyles},
      {"word/numbering.xml", fx::kNumbering},
      {"word/_rels/document.xml.rels", fx::rels({{"rId1", "header1.xml"}})},
      {"word/header1.xml", fx::story("hdr", R"(<w:p><w:fldSimple w:instr="PAGE"><w:r><w:t>1</w:t></w:r></w:fldSimple></w:p>)")},
      {"word/footnotes.xml", fx::notes("footnotes", {{1, fx::note_para("A note")}})},
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

  // The same inside the XML, zipped again, so the layout sees the damage.
  const std::string xml = parts[1].data;
  int laid = 0;
  for (int i = 0; i < 1500; ++i) {
    std::string mutated = xml;
    const int edits = 1 + static_cast<int>(rnd() % 6);
    for (int e = 0; e < edits; ++e) {
      const std::size_t at = rnd() % mutated.size();
      switch (rnd() % 4) {
        case 0: mutated[at] = "0123456789-\"<>/w:"[rnd() % 17]; break;
        case 1: mutated.erase(at, std::min<std::size_t>(rnd() % 24, mutated.size() - at)); break;
        case 2: mutated.insert(at, mutated.substr(rnd() % mutated.size(), rnd() % 64)); break;
        default: mutated.insert(at, std::to_string(static_cast<int>(rnd()) - (1 << 23))); break;
      }
    }
    auto copy = parts;
    copy[1].data = mutated;
    auto r = mv::codec::decode_docx(fx::zip(copy), rnd() % 3, nullptr, 256);
    if (r) ++laid;
  }
  WARN(laid << " of 1500 XML mutations still decoded");
}
