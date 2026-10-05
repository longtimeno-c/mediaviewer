// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PDF encryption on macOS: a file that needs a password is the locked card;
// one locked only by an owner password (opens with the empty one) shows.
// CoreGraphics writes the encrypted files, since a hand-built one would need
// RC4 and MD5 here. Windows has the same rule in pdf_win.cpp.
#include "catch_compat.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <vector>

#include "codec/decode.h"

namespace {

std::vector<std::uint8_t> encrypted_pdf(NSString* user, NSString* owner) {
  NSMutableData* data = [NSMutableData data];
  CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData((__bridge CFMutableDataRef)data);
  NSMutableDictionary* info = [NSMutableDictionary dictionary];
  if (user) info[(__bridge NSString*)kCGPDFContextUserPassword] = user;
  if (owner) info[(__bridge NSString*)kCGPDFContextOwnerPassword] = owner;
  CGRect box = CGRectMake(0, 0, 200, 100);
  CGContextRef ctx = CGPDFContextCreate(consumer, &box, (__bridge CFDictionaryRef)info);
  CGPDFContextBeginPage(ctx, nullptr);
  CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, 100, 100));
  CGPDFContextEndPage(ctx);
  CGPDFContextClose(ctx);
  CGContextRelease(ctx);
  CGDataConsumerRelease(consumer);
  const auto* p = static_cast<const std::uint8_t*>(data.bytes);
  return {p, p + data.length};
}

}  // namespace

TEST_CASE("A PDF that needs a password shows the locked card", "[pdf][mac]") {
  const auto bytes = encrypted_pdf(@"secret", @"owner");
  REQUIRE(mv::codec::probe(bytes) == mv::codec::format_family::pdf);
  auto r = mv::codec::decode(bytes);
  REQUIRE(r);
  CHECK(r->page_count == 1);
  CHECK(r->width == 1200);
  CHECK(r->height == 1600);
  CHECK(r->rgba[0] < 60);  // the card's dark background, not white paper
  auto past = mv::codec::decode(bytes, nullptr, 4, 1);
  CHECK_FALSE(past);
}

TEST_CASE("A PDF locked only by an owner password renders", "[pdf][mac]") {
  const auto bytes = encrypted_pdf(nil, @"owner");
  auto r = mv::codec::decode(bytes);
  REQUIRE(r);
  CHECK(r->width == mv::codec::kPdfLongEdge);
  const std::uint8_t* left = r->rgba.data() + (static_cast<std::size_t>(r->height / 2) * r->width + r->width / 4) * 4;
  CHECK(left[2] > 180);  // blue
  CHECK(left[0] < 60);
}
