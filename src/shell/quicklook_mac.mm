// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Finder thumbnails for the D5 still set (PR 20, docs/design/15) and the clip
// containers macOS cannot thumbnail itself (issue #610: Matroska, WebM, AVI,
// MPEG-TS): the principal class of MediaViewerThumbnails.appex, a Quick Look
// thumbnail extension.
//
// Out of process by construction: macOS runs the extension in its own
// sandboxed process, never inside Finder, so a decoder that crashes on a
// corrupt HEIC or RAW kills this process and Finder keeps running (the PR 20
// verify). This is the Mac twin of docs/design/09's out-of-process Explorer handler.
//
// A still's pixels come from the same path the filmstrip uses (image::make_thumb_jpeg:
// JPEG DCT scaling, the RAW's embedded preview, else a full decode, ICC ->
// sRGB, box fit to 512). A clip's come from the Explorer handler's portable
// poster path (shellext/thumb_poster.h: the filmstrip's poster frame, display
// matrix applied, read through the file rather than mapped whole, under the
// handler's 4 s deadline). A Finder thumbnail therefore matches the app's own
// thumbnail. Which path is decided by the file's first bytes, not its type.
// Nothing is cached here. The app's SQLite thumbnail cache lives outside this
// extension's sandbox, and Quick Look keeps its own cache.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "image/thumb.h"
#include "shellext/thumb_poster.h"
#include "shellext/thumb_request.h"

namespace {

// A thumbnail request for a still larger than this is refused rather than
// mapped: an extension that runs out of memory is killed mid-request, and a
// 1 GB "photo" in a camera dump is a damaged file, not a photo. A clip is
// never mapped, so it has no such cap.
constexpr NSUInteger kMaxSourceBytes = 512u * 1024u * 1024u;

NSError* no_thumbnail() {
  return [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:nil];
}

// JPEG bytes from make_thumb_jpeg -> a CGImage. ImageIO decodes a baseline
// JPEG we encoded ourselves; it never sees the user's original file.
CGImageRef copy_cgimage(const std::vector<std::uint8_t>& jpeg) {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, jpeg.data(), static_cast<CFIndex>(jpeg.size()));
  if (!data) return nullptr;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!source) return nullptr;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  return image;
}

// A clip poster's top-down BGRA (premultiplied where not opaque) -> a CGImage.
// Little-endian 32-bit with alpha first is B, G, R, A in memory.
CGImageRef copy_cgimage(const mv::shellext::bgra_thumb& t) {
  const std::size_t bytes = static_cast<std::size_t>(t.width) * t.height * 4u;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, t.bgra.data(), static_cast<CFIndex>(bytes));
  if (!data) return nullptr;
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CFRelease(data);
  if (!provider) return nullptr;
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  const CGBitmapInfo info =
      kCGBitmapByteOrder32Little |
      static_cast<CGBitmapInfo>(t.has_alpha ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst);
  CGImageRef image = CGImageCreate(t.width, t.height, 8, 32, static_cast<std::size_t>(t.width) * 4u, srgb,
                                   info, provider, nullptr, false, kCGRenderingIntentDefault);
  CGColorSpaceRelease(srgb);
  CGDataProviderRelease(provider);
  return image;
}

// The file's first bytes, for shellext::classify. Empty when unreadable.
std::vector<std::uint8_t> read_head(NSURL* url) {
  NSFileHandle* file = [NSFileHandle fileHandleForReadingFromURL:url error:nil];
  if (!file) return {};
  NSData* head = [file readDataUpToLength:mv::shellext::kProbeBytes error:nil];
  [file closeAndReturnError:nil];
  if (!head || head.length == 0) return {};
  const auto* p = static_cast<const std::uint8_t*>(head.bytes);
  return std::vector<std::uint8_t>(p, p + head.length);
}

// A clip: the poster frame, read through the file under the shared deadline.
CGImageRef copy_clip_poster(QLFileThumbnailRequest* request) {
  const CGSize box = request.maximumSize;
  const double edge = std::ceil(std::max(box.width, box.height) * std::max<CGFloat>(1.0, request.scale));
  const auto cx = static_cast<std::uint32_t>(std::clamp(edge, 16.0, double(mv::shellext::kMaxThumbEdge)));
  auto source = mv::shellext::open_file_source(request.fileURL.fileSystemRepresentation);
  if (!source) return nullptr;
  auto thumb = mv::shellext::render_poster_by(std::move(source), cx);
  if (!thumb) return nullptr;
  return copy_cgimage(thumb.value());
}

// A still: mapped, not copied (a 60 MB RAW costs address space, not 60 MB of
// heap), then the filmstrip's JPEG-512.
CGImageRef copy_still(NSURL* url, NSError** error) {
  NSData* bytes = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
  if (!bytes) return nullptr;
  if (bytes.length == 0 || bytes.length > kMaxSourceBytes) return nullptr;
  const std::span<const std::uint8_t> src(static_cast<const std::uint8_t*>(bytes.bytes), bytes.length);
  auto jpeg = mv::image::make_thumb_jpeg(src);
  if (!jpeg) return nullptr;
  return copy_cgimage(jpeg.value());
}

}  // namespace

@interface MVThumbnailProvider : QLThumbnailProvider
@end

@implementation MVThumbnailProvider

- (void)provideThumbnailForFileRequest:(QLFileThumbnailRequest*)request
                     completionHandler:(void (^)(QLThumbnailReply* _Nullable reply,
                                                 NSError* _Nullable error))handler {
  const std::vector<std::uint8_t> head = read_head(request.fileURL);
  if (head.empty()) {
    handler(nil, no_thumbnail());
    return;
  }

  NSError* readError = nil;
  CGImageRef image = mv::shellext::classify(head) == mv::shellext::source_kind::clip
                         ? copy_clip_poster(request)
                         : copy_still(request.fileURL, &readError);
  if (!image) {
    // An unreadable, unsupported, corrupt or too-slow file: no thumbnail,
    // Finder shows its generic icon. No path or filename is logged (rule 6).
    handler(nil, readError ? readError : no_thumbnail());
    return;
  }

  // Fit the image inside the requested box, preserving aspect. Quick Look
  // scales the context by request.scale itself.
  const CGSize box = request.maximumSize;
  const CGFloat iw = static_cast<CGFloat>(CGImageGetWidth(image));
  const CGFloat ih = static_cast<CGFloat>(CGImageGetHeight(image));
  const CGFloat fit = MIN(box.width / iw, box.height / ih);
  const CGSize size = CGSizeMake(MAX(1.0, floor(iw * fit)), MAX(1.0, floor(ih * fit)));

  // Handed to ARC so the drawing block keeps the image alive. Quick Look may
  // run the block after this method returns, on another thread.
  id retained = CFBridgingRelease(image);
  QLThumbnailReply* reply = [QLThumbnailReply
      replyWithContextSize:size
      currentContextDrawingBlock:^BOOL {
        CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
        if (!ctx) return NO;
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, CGRectMake(0, 0, size.width, size.height),
                           (__bridge CGImageRef)retained);
        return YES;
      }];
  handler(reply, nil);
}

@end
