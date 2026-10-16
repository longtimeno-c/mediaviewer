// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #354 — a PDF's text for Local search (docs/plans/document-search.md §2, owner
// call 2): PDFium, shipped in the Core pack beside mv_ai and loaded at run time
// (never linked), so the base installer does not grow and both OSes read the
// same text into the same index. The viewer keeps drawing pages with the OS.
//
// PDFium is hostile-input code in this process: it runs on the pack's
// background workers only, one document at a time (PDFium is not thread-safe),
// with a page cap and a time cap per document. A password-protected file is
// status::permission_denied (recorded as locked, never retried); one that does
// not open is status::corrupt.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::ai {

struct pdf_limits {
  std::uint32_t max_pages = 500;       // text from every page up to this (docs/design/17 "Documents")
  double max_seconds = 60.0;           // a document still being read after this stops there
  std::uint64_t max_file_bytes = 1ull << 31;  // PDFium's file access is 32-bit on Windows
};

class pdf_reader {
 public:
  virtual ~pdf_reader() = default;
  // Visits each page's text from `first` (0-based) in page order, as PDFium
  // lays it out (lines end in "\r\n"); a page with no text layer is visited
  // with "". `visit` false stops the read (status::cancelled). Returns the
  // document's page count. [worker-thread]
  [[nodiscard]] virtual result<std::uint32_t> read(
      const std::string& path, std::uint32_t first, const pdf_limits& limits,
      const std::function<bool(std::uint32_t page, std::string_view text)>& visit) = 0;
  // "PDFium": for Settings.
  [[nodiscard]] virtual const std::string& name() const noexcept = 0;
};

// PDFium from the pack's folder (platform::self_dir), or nullptr when it is
// not there (an older pack, a test build without it) or does not load.
[[nodiscard]] std::unique_ptr<pdf_reader> make_pdf_reader();
// The same from a library file (the tests).
[[nodiscard]] std::unique_ptr<pdf_reader> make_pdf_reader_at(const std::string& library_utf8);

}  // namespace mv::ai
