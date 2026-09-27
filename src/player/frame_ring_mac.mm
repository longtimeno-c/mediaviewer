// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 19 -- the Metal half of the presentation ring (plan/05 "Surface
// ownership", plan/15 D9). frame_ring.cpp holds the lock-free index rings and is
// shared with Windows; only the slot's textures are host-specific.
//
// Metal has no planar NV12/P010 texture to take two views of, so a slot is two
// textures we own: luma (R8Unorm / R16Unorm) and chroma at half size (RG8Unorm /
// RG16Unorm). They are the same "R8 + R8G8 / R16 + R16G16" pair plan/05 spells
// out for D3D11, just not views of one resource.
//
// Shared storage: on Apple Silicon that is unified memory (no copy), and it lets
// the software fallback fill a slot with replaceRegion. The hardware path fills
// it with a blit on the decode thread. Intel Macs have no unified memory and
// reject Shared textures, so they get Managed (same fill paths).
#import <Metal/Metal.h>

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {

expected frame_ring::create_slot(video_frame& slot) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!device) return err(status::invalid_arg);

  const MTLPixelFormat luma_format = ten_bit_ ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
  const MTLPixelFormat chroma_format = ten_bit_ ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;

  auto make = [&](MTLPixelFormat format, std::uint32_t w, std::uint32_t h) -> void* {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                            width:w
                                                           height:h
                                                        mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    return (__bridge_retained void*)texture;  // nil -> nullptr
  };

  slot.luma = make(luma_format, texture_w_, texture_h_);
  slot.chroma = make(chroma_format, texture_w_ / 2, texture_h_ / 2);
  if (!slot.luma || !slot.chroma) {
    release_slot(slot);
    return err(status::out_of_memory);
  }
  slot.ten_bit = ten_bit_;
  return {};
}

void frame_ring::release_slot(video_frame& slot) noexcept {
  if (slot.luma) (void)(__bridge_transfer id<MTLTexture>)slot.luma;
  if (slot.chroma) (void)(__bridge_transfer id<MTLTexture>)slot.chroma;
  slot.luma = nullptr;
  slot.chroma = nullptr;
  slot.texture = nullptr;
}

}  // namespace mv::player
