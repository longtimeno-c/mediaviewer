// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 (docs/plans/document-search.md D1): a document's text for Local search.
// The passage rule (src/addons/ai/passages.h) is pure C++ and runs everywhere;
// the PDFium cases read hand-built PDFs (pdf_fixture.h) through the library
// the pack ships, staged by the build (MV_AI_PDFIUM_LIB), and skip without it.
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "abi/addon_media.h"
#include "addons/ai/passages.h"
#include "addons/ai/pdf_reader.h"
#include "docx_fixture.h"
#include "import_fixture.h"
#include "pdf_fixture.h"

namespace fs = std::filesystem;
namespace pdffx = mv::test::pdffx;
using mv::ai::passage;
using mv::ai::passage_builder;

namespace {

std::string words_of(std::size_t n, const char* stem) {
  std::string out;
  for (std::size_t i = 0; i < n; ++i) out += (i ? " " : "") + std::string(stem) + std::to_string(i);
  return out;
}

struct temp_file {
  fs::path path;
  explicit temp_file(const std::vector<std::uint8_t>& bytes, const char* name) {
    path = fs::temp_directory_path() / (std::string("mv-ai-doc-") + name);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  [[nodiscard]] std::string utf8() const {
    const auto u = path.u8string();
    return {u.begin(), u.end()};
  }
};

std::unique_ptr<mv::ai::pdf_reader> pdfium() {
#ifdef MV_AI_PDFIUM_LIB
  if (fs::exists(MV_AI_PDFIUM_LIB)) return mv::ai::make_pdf_reader_at(MV_AI_PDFIUM_LIB);
#endif
  return nullptr;
}

// A page of `lines`, Helvetica 10 pt, one line under the other.
pdffx::page text_page(const std::vector<std::string>& lines) {
  pdffx::page p;
  p.width = 612;
  p.height = 792;
  p.content = "BT /F1 10 Tf 12 TL 72 720 Td";
  for (const std::string& l : lines) p.content += " (" + l + ") Tj T*";
  p.content += " ET";
  return p;
}

}  // namespace

TEST_CASE("passages: a few sentences each, never across a page", "[ai][documents]") {
  passage_builder b;
  // A heading joins the paragraph after it; a long paragraph closes at a
  // sentence end past kPassageWords.
  b.add(0, "Residential lease", true);
  b.add(0, words_of(30, "a") + ". " + words_of(30, "b") + ". " + words_of(10, "c") + ".", true);
  b.add(1, "Second page text.", true);
  b.finish();
  const std::vector<passage> got = b.take();
  REQUIRE(got.size() == 3);
  CHECK(got[0].page == 0);
  CHECK(got[0].text.rfind("Residential lease a0", 0) == 0);
  CHECK(got[1].page == 0);
  CHECK(got[1].text.rfind("c0", 0) == 0);  // the second sentence closed the first passage
  CHECK(got[2].page == 1);
  CHECK(got[2].text == "Second page text.");
  // `words`: folded, led by the last words of the passage before.
  CHECK(got[2].words.rfind("c2 c3 c4 c5 c6 c7 c8 c9 second page text", 0) == 0);
  CHECK(got[0].words.rfind("residential lease a0", 0) == 0);

  // A paragraph with no sentence end still closes at kPassageMaxWords.
  passage_builder long_one;
  long_one.add(3, words_of(200, "w"), true);
  long_one.finish();
  const auto parts = long_one.take();
  REQUIRE(parts.size() == 3);
  for (const passage& p : parts) CHECK(p.page == 3);

  // The cap: kMaxPassages, then nothing more.
  passage_builder full;
  for (std::size_t i = 0; i < mv::ai::kMaxPassages + 10; ++i) full.add(static_cast<std::uint32_t>(i), "x.", true);
  full.finish();
  CHECK(full.made() == mv::ai::kMaxPassages);
  CHECK(full.full());
}

TEST_CASE("passages: typographic text matches what is typed", "[ai][documents]") {
  CHECK(mv::ai::plain_text("\xE2\x80\x9CThe \xEF\xAC\x81rst o\xEF\xAC\x80" "er\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s") ==
        "\"The first offer\"  -  it's");
  CHECK(mv::ai::plain_text("co\xC2\xAD" "operate\xC2\xA0now") == "cooperate now");
  CHECK(mv::ai::match_words("Aus KÖLN, it\xE2\x80\x99s") == std::vector<std::string>{"aus", "koln", "its"});
  // A PDF page: line breaks are spaces, a hyphenated word is whole again, a
  // blank line ends a paragraph.
  CHECK(mv::ai::pdf_paragraphs("Payment is due in thirty\r\ndays. The infor-\r\nmation is\r\n\r\nNew para\r\n") ==
        std::vector<std::string>{"Payment is due in thirty days. The information is", "New para"});
  CHECK(mv::ai::pdf_paragraphs("Call 555-\r\n1234").front() == "Call 555- 1234");  // not a word break
  CHECK(mv::ai::pdf_paragraphs("").empty());
}

TEST_CASE("PDFium reads a PDF's text page by page", "[ai][documents][pdfium]") {
  auto reader = pdfium();
  if (!reader) SKIP("no PDFium staged (MV_AI_PDFIUM_LIB)");
  const auto bytes = pdffx::make({text_page({"Invoice 4471", "for the lease of the hall."}),
                                  text_page({"Items and quantities."}),
                                  text_page({"Payment terms: net 30 days."})},
                                 {}, pdffx::text_resources());
  temp_file f(bytes, "three.pdf");
  std::vector<std::pair<std::uint32_t, std::string>> pages;
  auto n = reader->read(f.utf8(), 0, mv::ai::pdf_limits{}, [&](std::uint32_t page, std::string_view text) {
    pages.emplace_back(page, std::string(text));
    return true;
  });
  REQUIRE(n);
  CHECK(n.value() == 3);
  REQUIRE(pages.size() == 3);
  CHECK(pages[0].first == 0);
  CHECK(pages[0].second.find("Invoice 4471") != std::string::npos);
  CHECK(pages[0].second.find("lease of the hall") != std::string::npos);
  CHECK(pages[2].second.find("net 30 days") != std::string::npos);

  // From a page (a resume), and stopped by the visitor.
  pages.clear();
  n = reader->read(f.utf8(), 2, mv::ai::pdf_limits{}, [&](std::uint32_t page, std::string_view text) {
    pages.emplace_back(page, std::string(text));
    return true;
  });
  REQUIRE(n);
  REQUIRE(pages.size() == 1);
  CHECK(pages[0].first == 2);
  n = reader->read(f.utf8(), 0, mv::ai::pdf_limits{}, [](std::uint32_t, std::string_view) { return false; });
  CHECK(n.error() == mv::status::cancelled);

  // The page cap.
  mv::ai::pdf_limits two;
  two.max_pages = 2;
  pages.clear();
  n = reader->read(f.utf8(), 0, two, [&](std::uint32_t page, std::string_view text) {
    pages.emplace_back(page, std::string(text));
    return true;
  });
  REQUIRE(n);
  CHECK(pages.size() == 2);

  // A page with nothing to read (a scan): visited with "".
  pdffx::page drawing;
  drawing.content = "0 0 1 rg 0 0 100 100 re f";
  temp_file scan(pdffx::make({drawing}), "scan.pdf");
  pages.clear();
  n = reader->read(scan.utf8(), 0, mv::ai::pdf_limits{}, [&](std::uint32_t page, std::string_view text) {
    pages.emplace_back(page, std::string(text));
    return true;
  });
  REQUIRE(n);
  REQUIRE(pages.size() == 1);
  CHECK(pages[0].second.empty());
}

// A page with an inline image (a 2 x 2 grey picture) drawn over w x h points.
pdffx::page picture_page(int w, int h) {
  pdffx::page p;
  p.width = 612;
  p.height = 792;
  p.content = "q " + std::to_string(w) + " 0 0 " + std::to_string(h) +
              " 40 40 cm BI /W 2 /H 2 /CS /G /BPC 8 ID \x10\x80\x80\x10 EI Q";
  return p;
}

TEST_CASE("PDFium: the pages mostly covered by a picture (#354 D2)", "[ai][documents][pdfium]") {
  auto reader = pdfium();
  if (!reader) SKIP("no PDFium staged (MV_AI_PDFIUM_LIB)");
  // Words; a full-page picture (a scan); a small one; words; a picture just
  // over half the page; one just under.
  const auto bytes = pdffx::make({text_page({"Cover"}), picture_page(540, 720), picture_page(100, 100),
                                  text_page({"More words"}), picture_page(500, 500), picture_page(480, 480)});
  temp_file f(bytes, "pictures.pdf");
  auto look = reader->look(f.utf8(), mv::ai::pdf_limits{}, 0.5f);
  REQUIRE(look);
  CHECK(look->pages == 6);
  CHECK(look->pictures == std::vector<std::uint32_t>{1, 4});
  // The page cap holds here too.
  mv::ai::pdf_limits two;
  two.max_pages = 2;
  look = reader->look(f.utf8(), two, 0.5f);
  REQUIRE(look);
  CHECK(look->pages == 6);
  CHECK(look->pictures == std::vector<std::uint32_t>{1});
  // Not a PDF: an error, never a crash.
  const std::vector<std::uint8_t> junk(64, 0x42);
  temp_file bad(junk, "junk.pdf");
  CHECK_FALSE(reader->look(bad.utf8(), mv::ai::pdf_limits{}, 0.5f));
}

TEST_CASE("PDFium: a broken or missing PDF is an error, never a crash", "[ai][documents][pdfium]") {
  auto reader = pdfium();
  if (!reader) SKIP("no PDFium staged (MV_AI_PDFIUM_LIB)");
  const auto good = pdffx::make({text_page({"Hello"})}, {}, pdffx::text_resources());
  const auto noop = [](std::uint32_t, std::string_view) { return true; };
  // Cut anywhere, and garbage: an error or whatever pages it still finds.
  for (std::size_t cut : {good.size() / 4, good.size() / 2, good.size() - 20}) {
    auto bytes = good;
    bytes.resize(cut);
    temp_file f(bytes, "cut.pdf");
    (void)reader->read(f.utf8(), 0, mv::ai::pdf_limits{}, noop);
  }
  temp_file junk(std::vector<std::uint8_t>(4096, 0x5A), "junk.pdf");
  CHECK(reader->read(junk.utf8(), 0, mv::ai::pdf_limits{}, noop).error() == mv::status::corrupt);
  temp_file empty(std::vector<std::uint8_t>{}, "empty.pdf");
  CHECK_FALSE(reader->read(empty.utf8(), 0, mv::ai::pdf_limits{}, noop));
  CHECK(reader->read((fs::temp_directory_path() / "mv-ai-doc-missing.pdf").string(), 0, mv::ai::pdf_limits{}, noop)
            .error() == mv::status::io);
  // A library that is not there is no reader.
  CHECK(mv::ai::make_pdf_reader_at((fs::temp_directory_path() / "no-pdfium-here.dylib").string()) == nullptr);
}

TEST_CASE("the host reads a DOCX's text and draws a document's page as a moment", "[ai][documents][host]") {
  // #354: host table document_text (DOCX through codec::docx_text) and
  // video_frame_rgb with pts_ms a page: a document result's tile.
  namespace fx = mv::test::docxfx;
  using mv::test::write_bytes;
  mv::test::scratch_dir dir("ai-doc-host");
  const std::string body = fx::para("Residential lease") + R"(<w:p><w:r><w:br w:type="page"/></w:r></w:p>)" +
                           fx::para("Two months notice.");
  write_bytes(dir / "lease.docx", fx::docx(body));
  const auto utf8 = [](const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
  };
  std::vector<mv::addon::document_line> lines;
  REQUIRE(mv::addon::media::document_text(utf8(dir / "lease.docx"), [&](const mv::addon::document_line& l) {
    lines.push_back(l);
    return true;
  }));
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].text == "Residential lease");
  CHECK(lines[0].page == 0);
  CHECK(lines[1].page == 1);
  CHECK(lines[1].page_count == 2);
  // A PDF's text is the pack's (PDFium), never the host's; a password-protected
  // DOCX (an OLE container, not a zip) is locked, not broken.
  write_bytes(dir / "a.pdf", pdffx::make({text_page({"Hello"})}, {}, pdffx::text_resources()));
  CHECK(mv::addon::media::document_text(utf8(dir / "a.pdf"), [](const auto&) { return true; }).error() ==
        mv::status::unsupported_format);
  std::vector<std::uint8_t> ole = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  ole.resize(4096, 0);
  write_bytes(dir / "locked.docx", ole);
  CHECK(mv::addon::media::document_text(utf8(dir / "locked.docx"), [](const auto&) { return true; }).error() ==
        mv::status::permission_denied);

  // The second page as a 512 px tile, and past the end an error.
  auto page = mv::addon::media::video_frame(utf8(dir / "lease.docx"), 1, 512);
  REQUIRE(page);
  CHECK(std::max(page->width, page->height) == 512);
  CHECK(page->rgb.size() == static_cast<std::size_t>(page->width) * page->height * 3);
  CHECK_FALSE(mv::addon::media::video_frame(utf8(dir / "lease.docx"), 5, 512));
  auto pdf_page = mv::addon::media::video_frame(utf8(dir / "a.pdf"), 0, 256);
  REQUIRE(pdf_page);
  CHECK(std::max(pdf_page->width, pdf_page->height) == 256);
}
