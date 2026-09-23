// SPDX-License-Identifier: GPL-2.0-or-later
// Finder thumbnails for the D5 still set (PR 20, plan/15): the principal class
// of MediaViewerThumbnails.appex, a Quick Look thumbnail extension.
//
// Out of process by construction: macOS runs the extension in its own
// sandboxed process, never inside Finder, so a decoder that crashes on a
// corrupt HEIC or RAW kills this process and Finder keeps running (the PR 20
// verify). This is the Mac twin of plan/09's out-of-process Explorer handler.
//
// The pixels come from the same path the filmstrip uses (image::make_thumb_jpeg:
// JPEG DCT scaling, the RAW's embedded preview, else a full decode, ICC ->
// sRGB, box fit to 512). A Finder thumbnail therefore matches the app's own
// thumbnail. Nothing is cached here. The app's SQLite thumbnail cache lives
// outside this extension's sandbox, and Quick Look keeps its own cache.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <cstdint>
#include <span>

#include "image/thumb.h"

namespace {

// A thumbnail request for a file larger than this is refused rather than
// mapped: an extension that runs out of memory is killed mid-request, and a
// 1 GB "photo" in a camera dump is a damaged file, not a photo.
constexpr NSUInteger kMaxSourceBytes = 512u * 1024u * 1024u;

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

}  // namespace

@interface MVThumbnailProvider : QLThumbnailProvider
@end

@implementation MVThumbnailProvider

- (void)provideThumbnailForFileRequest:(QLFileThumbnailRequest*)request
                     completionHandler:(void (^)(QLThumbnailReply* _Nullable reply,
                                                 NSError* _Nullable error))handler {
  NSError* readError = nil;
  // Mapped, not copied: a 60 MB RAW costs address space, not 60 MB of heap.
  NSData* bytes = [NSData dataWithContentsOfURL:request.fileURL
                                        options:NSDataReadingMappedIfSafe
                                          error:&readError];
  if (!bytes) {
    handler(nil, readError);
    return;
  }
  if (bytes.length == 0 || bytes.length > kMaxSourceBytes) {
    handler(nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:nil]);
    return;
  }

  const std::span<const std::uint8_t> src(static_cast<const std::uint8_t*>(bytes.bytes), bytes.length);
  auto jpeg = mv::image::make_thumb_jpeg(src);
  if (!jpeg) {
    // An unsupported or corrupt file: no thumbnail, Finder shows its generic
    // icon. No path or filename is logged (rule 6).
    handler(nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:nil]);
    return;
  }

  CGImageRef image = copy_cgimage(jpeg.value());
  if (!image) {
    handler(nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:nil]);
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
