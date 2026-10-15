// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// text_reader.h on the Mac: Apple Vision's VNRecognizeTextRequest (accurate
// level, language correction on, languages detected), the same recogniser
// Photos' Live Text uses. In-process, on the calling worker; nothing is sent
// anywhere. ARC (cmake/darwin.cmake builds every .mm with -fobjc-arc).
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <cstring>

#include "addons/ai/text_reader.h"

namespace mv::ai {
namespace {

// Lines below this confidence are noise (a texture read as letters).
constexpr float kMinConfidence = 0.3f;

class vision_reader final : public text_reader {
 public:
  result<std::vector<std::string>> read(const rgb_frame& img) override {
    if (img.width == 0 || img.height == 0 || img.rgb.size() < static_cast<std::size_t>(img.width) * img.height * 3) {
      return err(status::invalid_arg);
    }
    @autoreleasepool {
      // RGB to RGBX: Core Graphics takes no 24-bit image.
      const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
      std::vector<std::uint8_t> rgbx(n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        rgbx[i * 4] = img.rgb[i * 3];
        rgbx[i * 4 + 1] = img.rgb[i * 3 + 1];
        rgbx[i * 4 + 2] = img.rgb[i * 3 + 2];
        rgbx[i * 4 + 3] = 255;
      }
      CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgbx.data(), rgbx.size(), nullptr);
      CGImageRef cg = CGImageCreate(img.width, img.height, 8, 32, static_cast<std::size_t>(img.width) * 4, cs,
                                    static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast), dp, nullptr, false,
                                    kCGRenderingIntentDefault);
      CGDataProviderRelease(dp);
      CGColorSpaceRelease(cs);
      if (!cg) return err(status::out_of_memory);
      VNRecognizeTextRequest* req = [[VNRecognizeTextRequest alloc] init];
      req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
      req.usesLanguageCorrection = YES;
      if (@available(macOS 13.0, *)) req.automaticallyDetectsLanguage = YES;
      VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
      NSError* error = nil;
      const BOOL ok = [handler performRequests:@[ req ] error:&error];
      std::vector<std::string> lines;
      if (ok) {
        for (VNRecognizedTextObservation* o in req.results) {
          VNRecognizedText* best = [o topCandidates:1].firstObject;
          if (!best || best.confidence < kMinConfidence) continue;
          const char* s = best.string.UTF8String;
          if (s && *s) lines.emplace_back(s);
        }
      }
      CGImageRelease(cg);
      if (!ok) return err(status::io);
      return lines;
    }
  }
  const std::string& spec_key() const noexcept override { return key_; }

 private:
  std::string key_ = "ocr/vision-accurate/1";
};

}  // namespace

std::unique_ptr<text_reader> make_text_reader() { return std::make_unique<vision_reader>(); }

}  // namespace mv::ai
