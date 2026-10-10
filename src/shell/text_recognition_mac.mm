// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/text_recognition_mac.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <new>

namespace mv::shell {

text_recognition vision_text_recognizer::recognise(const text_image& image, std::string& text) noexcept {
  text.clear();
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

    // Observations come back top to bottom; one line each, as Preview copies.
    try {
      for (VNRecognizedTextObservation* observation in request.results) {
        VNRecognizedText* best = [observation topCandidates:1].firstObject;
        const char* utf8 = best.string.UTF8String;
        if (!utf8 || !*utf8) continue;
        text += utf8;
        text.push_back('\n');
      }
    } catch (const std::bad_alloc&) {
      text.clear();
      return text_recognition::failed;
    }
  }
  return text_recognition::ok;
}

}  // namespace mv::shell
