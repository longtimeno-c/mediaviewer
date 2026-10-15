// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/text_recognition_mac.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <new>

namespace mv::shell {

text_recognition vision_text_recognizer::recognise(const text_image& image, text_layout& layout) noexcept {
  layout = {};
  if (image.width == 0 || image.height == 0 ||
      image.pixels.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
    return text_recognition::failed;
  }
  @autoreleasepool {
    // The pixels are only borrowed for the request: the provider has no
    // release callback and the CGImage is gone before this returns.
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, image.pixels.data(), image.pixels.size(), nullptr);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef cg = provider && space
                        ? CGImageCreate(image.width, image.height, 8, 32,
                                        static_cast<std::size_t>(image.width) * 4, space,
                                        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast),
                                        provider, nullptr, false, kCGRenderingIntentDefault)
                        : nullptr;
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    if (!cg) return text_recognition::failed;

    VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
    request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
    request.usesLanguageCorrection = YES;
    request.automaticallyDetectsLanguage = YES;
    VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
    NSError* error = nil;
    const BOOL ok = [handler performRequests:@[ request ] error:&error];
    CGImageRelease(cg);
    if (!ok) return text_recognition::failed;

    // Observations come back top to bottom, one per line. Each run between
    // whitespace is a word and gets its own box from Vision; a line Vision
    // cannot box word by word is split evenly instead. Vision's boxes are
    // normalised with the origin at the bottom left.
    NSCharacterSet* blanks = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    try {
      std::uint32_t line = 0;
      for (VNRecognizedTextObservation* observation in request.results) {
        VNRecognizedText* best = [observation topCandidates:1].firstObject;
        NSString* string = best.string;
        if (string.length == 0) continue;
        const std::size_t before = layout.words.size();
        bool all_boxed = true;
        NSUInteger at = 0;
        while (at < string.length && all_boxed) {
          const NSRange gap = [string rangeOfCharacterFromSet:blanks.invertedSet options:0
                                                        range:NSMakeRange(at, string.length - at)];
          if (gap.location == NSNotFound) break;
          NSRange end = [string rangeOfCharacterFromSet:blanks options:0
                                                  range:NSMakeRange(gap.location, string.length - gap.location)];
          const NSUInteger stop = end.location == NSNotFound ? string.length : end.location;
          const NSRange word = NSMakeRange(gap.location, stop - gap.location);
          at = stop;
          VNRectangleObservation* box = [best boundingBoxForRange:word error:nil];
          const char* utf8 = [string substringWithRange:word].UTF8String;
          if (!box || !utf8) {
            all_boxed = false;
            break;
          }
          const CGRect r = box.boundingBox;
          text_word w;
          w.x0 = static_cast<float>(CGRectGetMinX(r));
          w.x1 = static_cast<float>(CGRectGetMaxX(r));
          w.y0 = static_cast<float>(1.0 - CGRectGetMaxY(r));
          w.y1 = static_cast<float>(1.0 - CGRectGetMinY(r));
          w.line = line;
          w.text = utf8;
          layout.words.push_back(std::move(w));
        }
        if (!all_boxed) {
          layout.words.resize(before);
          const CGRect lb = observation.boundingBox;
          add_line_split_evenly(layout, line, string.UTF8String, static_cast<float>(CGRectGetMinX(lb)),
                                static_cast<float>(1.0 - CGRectGetMaxY(lb)), static_cast<float>(CGRectGetMaxX(lb)),
                                static_cast<float>(1.0 - CGRectGetMinY(lb)));
        }
        if (layout.words.size() != before) ++line;
      }
    } catch (const std::bad_alloc&) {
      layout = {};
      return text_recognition::failed;
    }
  }
  return text_recognition::ok;
}

}  // namespace mv::shell
