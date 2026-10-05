// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-built PDFs for the PDF tests: no generator, no corpus, the same bytes on
// every platform. Each page is a MediaBox, an optional /Rotate and a content
// stream; the cross-reference table is computed, so the file is well formed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mv::test::pdffx {

struct page {
  int width = 200;   // points
  int height = 100;
  int rotate = 0;    // /Rotate, degrees clockwise
  std::string content;  // the page's content stream, e.g. "0 0 1 rg 0 0 100 100 re f"
};

inline std::vector<std::uint8_t> make(const std::vector<page>& pages, const std::string& prefix = {}) {
  std::string out = prefix + "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<std::size_t> offsets;
  auto object = [&](const std::string& body) {
    offsets.push_back(out.size());
    out += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
  };
  // 1: catalog, 2: pages, then a page + content pair per page.
  object("<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (std::size_t i = 0; i < pages.size(); ++i) kids += std::to_string(3 + 2 * i) + " 0 R ";
  object("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>");
  for (std::size_t i = 0; i < pages.size(); ++i) {
    const page& p = pages[i];
    std::string dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(p.width) + " " +
                       std::to_string(p.height) + "] /Contents " + std::to_string(4 + 2 * i) + " 0 R";
    if (p.rotate != 0) dict += " /Rotate " + std::to_string(p.rotate);
    object(dict + " >>");
    object("<< /Length " + std::to_string(p.content.size()) + " >>\nstream\n" + p.content +
           "\nendstream");
  }
  const std::size_t xref = out.size();
  out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
  for (std::size_t off : offsets) {
    char line[32];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", off);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return {out.begin(), out.end()};
}

}  // namespace mv::test::pdffx
