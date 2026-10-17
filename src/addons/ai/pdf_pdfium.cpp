// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// pdf_reader.h over PDFium (bblanchon/pdfium-binaries, pinned in cmake/ai.cmake),
// opened by path at run time with infer's dylib helpers, as ONNX Runtime is.
// Only its C headers are compiled in: every entry point is looked up by name,
// so a pack without the library, or a library missing one of them, is simply
// a pack that does not read PDFs.
#include "addons/ai/pdf_reader.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <vector>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include "addons/ai/platform.h"
#include "infer/ort.h"

namespace mv::ai {
namespace {

#if defined(_WIN32)
constexpr const char* kLibrary = "pdfium.dll";
#elif defined(__APPLE__)
constexpr const char* kLibrary = "libpdfium.dylib";
#else
constexpr const char* kLibrary = "libpdfium.so";
#endif

// PDFium is not thread-safe: one call into it at a time, process-wide (the
// library is initialised once and never torn down while the pack is loaded).
std::mutex& pdfium_mutex() {
  static std::mutex m;
  return m;
}

// UTF-16 code units (FPDFText_GetText's "UCS-2", surrogate pairs included in
// practice) to UTF-8. An unpaired surrogate is dropped.
void append_utf16(std::string& out, const unsigned short* s, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t c = s[i];
    if (c == 0) break;
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        ++i;
      } else {
        continue;
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      continue;
    }
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
}

// FPDF_FILEACCESS over a file stream: PDFium reads the blocks it needs, so a
// large PDF is never read whole into memory.
struct file_access {
  std::ifstream in;
  static int get_block(void* param, unsigned long position, unsigned char* buf, unsigned long size) {
    auto* self = static_cast<file_access*>(param);
    self->in.clear();
    self->in.seekg(static_cast<std::streamoff>(position));
    self->in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(size));
    return self->in.gcount() == static_cast<std::streamsize>(size) ? 1 : 0;
  }
};

class pdfium_reader final : public pdf_reader {
 public:
  ~pdfium_reader() override {
    // The library stays initialised and loaded: a worker may be the last to
    // let go, and FPDF_DestroyLibrary + dlclose on a pack still in use is not
    // worth the risk for a few MB until the process exits.
  }

  static std::unique_ptr<pdf_reader> open(const std::string& library) {
    void* lib = infer::dylib::open(library);
    if (!lib) return nullptr;
    auto r = std::unique_ptr<pdfium_reader>(new pdfium_reader());
    const auto sym = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(infer::dylib::symbol(lib, name));
      return fn != nullptr;
    };
    const bool ok = sym(r->init_, "FPDF_InitLibrary") && sym(r->load_, "FPDF_LoadCustomDocument") &&
                    sym(r->last_error_, "FPDF_GetLastError") && sym(r->page_count_, "FPDF_GetPageCount") &&
                    sym(r->load_page_, "FPDF_LoadPage") && sym(r->close_page_, "FPDF_ClosePage") &&
                    sym(r->close_doc_, "FPDF_CloseDocument") && sym(r->text_load_, "FPDFText_LoadPage") &&
                    sym(r->text_count_, "FPDFText_CountChars") && sym(r->text_get_, "FPDFText_GetText") &&
                    sym(r->text_close_, "FPDFText_ClosePage");
    if (!ok) {
      infer::dylib::close(lib);
      return nullptr;
    }
    // #354 D2, optional: without them look() answers unsupported and the
    // engine pictures the first pages alone.
    r->can_look_ = sym(r->count_objects_, "FPDFPage_CountObjects") && sym(r->get_object_, "FPDFPage_GetObject") &&
                   sym(r->object_type_, "FPDFPageObj_GetType") && sym(r->object_bounds_, "FPDFPageObj_GetBounds") &&
                   sym(r->page_width_, "FPDF_GetPageWidthF") && sym(r->page_height_, "FPDF_GetPageHeightF");
    {
      std::lock_guard lock(pdfium_mutex());
      static bool initialised = false;  // once per process, under the lock
      if (!initialised) {
        r->init_();
        initialised = true;
      }
    }
    return r;
  }

  result<std::uint32_t> read(const std::string& path, std::uint32_t first, const pdf_limits& limits,
                             const std::function<bool(std::uint32_t, std::string_view)>& visit) override {
    opened o(this);
    MV_TRY_VOID(o.open(path, limits));
    const FPDF_DOCUMENT doc = o.doc;
    const std::uint32_t pages = o.pages;

    const auto started = std::chrono::steady_clock::now();
    std::vector<unsigned short> buf;
    std::string text;
    std::uint32_t page = first;
    bool more = true;
    // Page by page, the lock taken for each: two workers with a PDF each take
    // turns, and neither holds the lock while the engine commits passages.
    const std::uint32_t last = std::min(pages, limits.max_pages);
    for (; more && page < last; ++page) {
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > limits.max_seconds) {
        break;  // the time cap: what was read stands
      }
      text.clear();
      {
        std::lock_guard lock(pdfium_mutex());
        FPDF_PAGE pg = load_page_(doc, static_cast<int>(page));
        if (pg) {
          FPDF_TEXTPAGE tp = text_load_(pg);
          if (tp) {
            // A page's text past 256 K characters is not prose worth indexing.
            const int count = std::min(text_count_(tp), 256 * 1024);
            if (count > 0) {
              buf.assign(static_cast<std::size_t>(count) + 1, 0);
              const int got = text_get_(tp, 0, count, buf.data());
              if (got > 0) append_utf16(text, buf.data(), static_cast<std::size_t>(got));
            }
            text_close_(tp);
          }
          close_page_(pg);
        }
      }
      more = visit(page, text);
    }
    if (!more) return err(status::cancelled);
    return pages;
  }

  result<pdf_look> look(const std::string& path, const pdf_limits& limits, float min_cover) override {
    if (!can_look_) return err(status::unsupported_format);
    opened o(this);
    MV_TRY_VOID(o.open(path, limits));
    pdf_look out;
    out.pages = o.pages;
    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t last = std::min(o.pages, limits.max_pages);
    for (std::uint32_t page = 0; page < last; ++page) {
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > limits.max_seconds) {
        break;  // the time cap: the pages looked at stand
      }
      std::lock_guard lock(pdfium_mutex());
      FPDF_PAGE pg = load_page_(o.doc, static_cast<int>(page));
      if (!pg) continue;
      const double w = page_width_(pg), h = page_height_(pg);
      double covered = 0;
      // The page's own image objects, each clipped to the page; overlaps count
      // twice, which only matters for a page of stacked pictures. A page with
      // thousands of objects is a drawing, not a picture: looked at no further.
      const int n = std::min(count_objects_(pg), 4096);
      for (int i = 0; i < n && w > 0 && h > 0; ++i) {
        FPDF_PAGEOBJECT obj = get_object_(pg, i);
        if (!obj || object_type_(obj) != FPDF_PAGEOBJ_IMAGE) continue;
        float l = 0, b = 0, r = 0, t = 0;
        if (!object_bounds_(obj, &l, &b, &r, &t)) continue;
        const double cw = std::min<double>(r, w) - std::max<double>(l, 0);
        const double ch = std::min<double>(t, h) - std::max<double>(b, 0);
        if (cw > 0 && ch > 0) covered += cw * ch;
      }
      close_page_(pg);
      if (w > 0 && h > 0 && covered / (w * h) > min_cover) out.pictures.push_back(page);
    }
    return out;
  }

  const std::string& name() const noexcept override { return name_; }

 private:
  // An open document (its file stream with it), closed under the lock.
  struct opened {
    explicit opened(pdfium_reader* r) : self(r) {}
    ~opened() {
      if (!doc) return;
      std::lock_guard lock(pdfium_mutex());
      self->close_doc_(doc);
    }
    opened(const opened&) = delete;
    opened& operator=(const opened&) = delete;
    expected open(const std::string& path, const pdf_limits& limits) {
      std::error_code ec;
      const std::filesystem::path fp(std::u8string(path.begin(), path.end()));
      const std::uintmax_t size = std::filesystem::file_size(fp, ec);
      if (ec) return err(status::io);
      if (size == 0 || size > limits.max_file_bytes || size > 0x7FFFFFFFull) return err(status::corrupt);
      fa.in.open(fp, std::ios::binary);
      if (!fa.in) return err(status::io);
      access.m_FileLen = static_cast<unsigned long>(size);
      access.m_GetBlock = &file_access::get_block;
      access.m_Param = &fa;
      std::lock_guard lock(pdfium_mutex());
      doc = self->load_(&access, nullptr);
      if (!doc) {
        const unsigned long e = self->last_error_();
        return err(e == FPDF_ERR_PASSWORD || e == FPDF_ERR_SECURITY ? status::permission_denied
                   : e == FPDF_ERR_FILE                             ? status::io
                                                                    : status::corrupt);
      }
      const int n = self->page_count_(doc);
      pages = n > 0 ? static_cast<std::uint32_t>(n) : 0;
      return {};
    }
    pdfium_reader* self;
    file_access fa;
    FPDF_FILEACCESS access{};
    FPDF_DOCUMENT doc = nullptr;
    std::uint32_t pages = 0;
  };

  pdfium_reader() = default;
  decltype(&FPDF_InitLibrary) init_ = nullptr;
  decltype(&FPDF_LoadCustomDocument) load_ = nullptr;
  decltype(&FPDF_GetLastError) last_error_ = nullptr;
  decltype(&FPDF_GetPageCount) page_count_ = nullptr;
  decltype(&FPDF_LoadPage) load_page_ = nullptr;
  decltype(&FPDF_ClosePage) close_page_ = nullptr;
  decltype(&FPDF_CloseDocument) close_doc_ = nullptr;
  decltype(&FPDFText_LoadPage) text_load_ = nullptr;
  decltype(&FPDFText_CountChars) text_count_ = nullptr;
  decltype(&FPDFText_GetText) text_get_ = nullptr;
  decltype(&FPDFText_ClosePage) text_close_ = nullptr;
  bool can_look_ = false;
  decltype(&FPDFPage_CountObjects) count_objects_ = nullptr;
  decltype(&FPDFPage_GetObject) get_object_ = nullptr;
  decltype(&FPDFPageObj_GetType) object_type_ = nullptr;
  decltype(&FPDFPageObj_GetBounds) object_bounds_ = nullptr;
  decltype(&FPDF_GetPageWidthF) page_width_ = nullptr;
  decltype(&FPDF_GetPageHeightF) page_height_ = nullptr;
  std::string name_ = "PDFium";
};

}  // namespace

std::unique_ptr<pdf_reader> make_pdf_reader_at(const std::string& library_utf8) {
  return pdfium_reader::open(library_utf8);
}

std::unique_ptr<pdf_reader> make_pdf_reader() {
  const std::string dir = platform::self_dir();
  if (dir.empty()) return nullptr;
  return make_pdf_reader_at(dir + "/" + kLibrary);
}

}  // namespace mv::ai
