// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/metal_layer.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "core/trace.h"

namespace mv::gfx {

namespace {

double screen_refresh_seconds(NSView* view) noexcept {
  NSScreen* screen = view.window.screen ?: [NSScreen mainScreen];
  const NSInteger fps = screen.maximumFramesPerSecond;
  if (fps <= 0) return 0.0;
  return 1.0 / static_cast<double>(fps);
}

}  // namespace

metal_layer::~metal_layer() { destroy(); }

expected metal_layer::attach(void* nsview, metal_device& dev, std::uint32_t width,
                             std::uint32_t height, double contents_scale) noexcept {
  if (!nsview || !dev.valid() || width == 0 || height == 0) return err(status::invalid_arg);
  destroy();

  NSView* view = (__bridge NSView*)nsview;
  CAMetalLayer* layer = (CAMetalLayer*)view.layer;
  if (![layer isKindOfClass:[CAMetalLayer class]]) {
    MV_LOG_ERROR("metal_layer: view backing layer is not CAMetalLayer");
    return err(status::invalid_arg);
  }

  layer.device = (__bridge id<MTLDevice>)dev.native_device();
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
  layer.framebufferOnly = YES;
  // 2, not 1: see the matching comment on MvMetalView's -makeBackingLayer
  // (src/shell/main_mac.mm) -- CAMetalLayer only accepts [2, 3], found on
  // real hardware (2026-09-18).
  layer.maximumDrawableCount = 2;
  layer.displaySyncEnabled = YES;
  layer.contentsScale = contents_scale > 0.0 ? contents_scale : 1.0;
  layer.drawableSize = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
  if (CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB)) {
    layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
  }

  view_ = nsview;
  layer_ = (__bridge void*)layer;
  width_ = width;
  height_ = height;
  refresh_seconds_ = screen_refresh_seconds(view);
  return {};
}

void metal_layer::destroy() noexcept {
  layer_ = nullptr;
  view_ = nullptr;
  width_ = 0;
  height_ = 0;
  refresh_seconds_ = 0.0;
}

expected metal_layer::resize(std::uint32_t width, std::uint32_t height,
                             double contents_scale) noexcept {
  if (!layer_ || width == 0 || height == 0) return err(status::invalid_arg);
  CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_;
  layer.contentsScale = contents_scale > 0.0 ? contents_scale : 1.0;
  layer.drawableSize = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
  width_ = width;
  height_ = height;
  refresh_output_info();
  return {};
}

void metal_layer::refresh_output_info() noexcept {
  if (!view_) {
    refresh_seconds_ = 0.0;
    return;
  }
  refresh_seconds_ = screen_refresh_seconds((__bridge NSView*)view_);
}

}  // namespace mv::gfx
