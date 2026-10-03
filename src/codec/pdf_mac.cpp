// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PDF on macOS through CoreGraphics (docs/plans/audio-and-documents.md §2.4):
// the system's own renderer, no bundled PDF library. CoreGraphics is a C API,
// so this is plain C++ and runs on any worker thread.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <new>

#include "codec/card.h"
#include "codec/decode.h"

namespace mv::codec {
namespace {

template <class T>
struct cf_release {
  T ref = nullptr;
  ~cf_release() {
    if (ref) CFRelease(ref);
  }
};

raster locked_page() {
  const card c = make_card(card_kind::locked, 1200, 1600);
  raster out;
  out.format = format_family::pdf;
  out.width = c.width;
  out.height = c.height;
  out.rgba = c.rgba;
  out.tagged_srgb = true;
  return out;
}

}  // namespace

result<raster> decode_pdf(std::span<const std::uint8_t> bytes, std::uint32_t page,
                          const job_context* ctx, std::uint32_t long_edge) {
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kPdfLongEdge);
  if (probe(bytes) != format_family::pdf) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  // No copy: CoreGraphics reads the caller's bytes, which outlive this call.
  cf_release<CFDataRef> data{CFDataCreateWithBytesNoCopy(
      kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()), kCFAllocatorNull)};
  if (!data.ref) return err(status::out_of_memory);
  cf_release<CGDataProviderRef> provider{CGDataProviderCreateWithCFData(data.ref)};
  if (!provider.ref) return err(status::out_of_memory);
  CGPDFDocumentRef raw_doc = CGPDFDocumentCreateWithProvider(provider.ref);
  if (!raw_doc) return err(status::corrupt);
  struct doc_release {
    CGPDFDocumentRef d;
    ~doc_release() { CGPDFDocumentRelease(d); }
  } doc{raw_doc};

  // A file that opens with the empty password (owner-password-only, the
  // common "no printing" kind) shows; one that needs a password is the card.
  if (CGPDFDocumentIsEncrypted(doc.d) && !CGPDFDocumentIsUnlocked(doc.d) &&
      !CGPDFDocumentUnlockWithPassword(doc.d, "")) {
    if (page != 0) return err(status::invalid_arg);
    return locked_page();
  }

  const std::size_t count = CGPDFDocumentGetNumberOfPages(doc.d);
  if (count == 0) return err(status::corrupt);
  if (page >= count) return err(status::invalid_arg);
  CGPDFPageRef pdf_page = CGPDFDocumentGetPage(doc.d, page + 1);  // 1-based
  if (!pdf_page) return err(status::corrupt);

  const CGRect box = CGPDFPageGetBoxRect(pdf_page, kCGPDFCropBox);
  const int rotation = ((CGPDFPageGetRotationAngle(pdf_page) % 360) + 360) % 360;
  const bool quarter = rotation == 90 || rotation == 270;
  const double page_w = quarter ? box.size.height : box.size.width;
  const double page_h = quarter ? box.size.width : box.size.height;
  if (!(page_w > 0.5) || !(page_h > 0.5)) return err(status::corrupt);
  const double scale = static_cast<double>(long_edge) / std::max(page_w, page_h);
  const auto width = static_cast<std::uint32_t>(std::max(1.0, std::round(page_w * scale)));
  const auto height = static_cast<std::uint32_t>(std::max(1.0, std::round(page_h * scale)));

  raster out;
  out.format = format_family::pdf;
  out.width = width;
  out.height = height;
  out.tagged_srgb = true;
  out.page = page;
  out.page_count = static_cast<std::uint32_t>(count);
  try {
    out.rgba.assign(static_cast<std::size_t>(width) * height * 4u, 255);  // white paper
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  cf_release<CGColorSpaceRef> srgb{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
  if (!srgb.ref) return err(status::internal);
  CGContextRef raw_ctx = CGBitmapContextCreate(
      out.rgba.data(), width, height, 8, static_cast<std::size_t>(width) * 4u, srgb.ref,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  if (!raw_ctx) return err(status::out_of_memory);
  struct ctx_release {
    CGContextRef c;
    ~ctx_release() { CGContextRelease(c); }
  } cg{raw_ctx};

  CGContextSetInterpolationQuality(cg.c, kCGInterpolationHigh);
  CGContextSetShouldAntialias(cg.c, true);
  // Applied to the page's points in reverse order of these calls: move the
  // crop box to the origin, turn it by /Rotate (clockwise on screen), scale.
  // CGPDFPageGetDrawingTransform would do this but never scales a page up.
  CGContextScaleCTM(cg.c, static_cast<CGFloat>(width) / page_w, static_cast<CGFloat>(height) / page_h);
  const CGFloat bw = box.size.width;
  const CGFloat bh = box.size.height;
  switch (rotation) {
    case 90:
      CGContextTranslateCTM(cg.c, 0, bw);
      CGContextRotateCTM(cg.c, static_cast<CGFloat>(-M_PI / 2));
      break;
    case 180:
      CGContextTranslateCTM(cg.c, bw, bh);
      CGContextRotateCTM(cg.c, static_cast<CGFloat>(M_PI));
      break;
    case 270:
      CGContextTranslateCTM(cg.c, bh, 0);
      CGContextRotateCTM(cg.c, static_cast<CGFloat>(M_PI / 2));
      break;
    default:
      break;
  }
  CGContextTranslateCTM(cg.c, -box.origin.x, -box.origin.y);
  CGContextClipToRect(cg.c, box);
  CGContextDrawPDFPage(cg.c, pdf_page);
  CGContextFlush(cg.c);
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  // Opaque paper: premultiplied and straight alpha agree.
  for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  return out;
}

}  // namespace mv::codec
