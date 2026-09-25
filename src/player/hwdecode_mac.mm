// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 19 -- VideoToolbox hardware decode. Metal host (D9 / plan/15-platforms.md):
// the counterpart of hwdecode_win.cpp and the only player/ file that names
// CoreVideo or Metal.
//
// "FFmpeg + VideoToolbox on *your* MTLDevice" (plan/10 PR 19): VideoToolbox
// owns its own decode sessions, so the device is used where it matters -- the
// CVMetalTextureCache that wraps the decoder's IOSurface-backed pixel buffers
// is built on OUR MTLDevice, and the blit that copies them OUT of the decoder's
// pool into our presentation ring runs on a queue of that device. The decoder
// pool is never presented from (plan/05 "Surface ownership").
//
// Threading: everything here runs on the video decode thread. It waits for its
// own blit to finish (`waitUntilCompleted`): that thread is neither the UI nor
// the render thread, the wait is ~1 ms for a 4K frame, and it means the slot is
// complete before frame_ring::commit() publishes it and before the CVPixelBuffer
// goes back to the decoder -- there is no ordering hazard to reason about.
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {
namespace {

// Lives in AVHWDeviceContext::user_opaque and dies with the last reference to
// the hardware device context (AVHWDeviceContext::free).
struct mac_hw_state {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CVMetalTextureCacheRef cache = nullptr;

  ~mac_hw_state() {
    if (cache) CFRelease(cache);
  }
};

void free_state(AVHWDeviceContext* ctx) {
  delete static_cast<mac_hw_state*>(ctx->user_opaque);
  ctx->user_opaque = nullptr;
}

[[nodiscard]] mac_hw_state* state_of(AVBufferRef* hw_device_ctx) noexcept {
  if (!hw_device_ctx) return nullptr;
  return static_cast<mac_hw_state*>(
      reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data)->user_opaque);
}

[[nodiscard]] CVPixelBufferRef pixel_buffer_of(const AVFrame* frame) noexcept {
  // AV_PIX_FMT_VIDEOTOOLBOX: data[3] is the CVPixelBufferRef.
  return reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
}

[[nodiscard]] bool is_ten_bit(OSType format) noexcept {
  return format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
         format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
}

[[nodiscard]] bool is_supported_420(OSType format) noexcept {
  return format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || is_ten_bit(format);
}

}  // namespace

result<AVBufferRef*> create_hw_device_ctx(gpu_device_ptr device) noexcept {
  if (!device) return err(status::invalid_arg);

  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
  if (!ref) return err(status::out_of_memory);
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(ref->data);

  auto state = std::make_unique<mac_hw_state>();
  state->device = (__bridge id<MTLDevice>)device;
  state->queue = [state->device newCommandQueue];
  if (!state->queue ||
      CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, state->device, nullptr,
                                &state->cache) != kCVReturnSuccess) {
    av_buffer_unref(&ref);
    return err(status::internal);
  }
  hw->user_opaque = state.release();
  hw->free = free_state;

  if (av_hwdevice_ctx_init(ref) < 0) {
    av_buffer_unref(&ref);  // free_state runs via hw->free
    return err(status::unsupported_format);
  }
  MV_LOG_INFO("player: VideoToolbox hardware device context, texture cache on the shell's MTLDevice");
  return ref;
}

status describe_hw_surface(const AVFrame* frame, std::uint32_t* out_texture_w,
                           std::uint32_t* out_texture_h, bool* out_ten_bit) noexcept {
  if (!frame || !out_texture_w || !out_texture_h || !out_ten_bit) return status::invalid_arg;
  if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX) return status::invalid_arg;

  CVPixelBufferRef pb = pixel_buffer_of(frame);
  if (!pb) return status::corrupt;

  const OSType format = CVPixelBufferGetPixelFormatType(pb);
  // Only 4:2:0 biplanar reaches the NV12/P010 shader. 4:2:2 / 4:4:4 hardware
  // output is not in the D5 v1 set.
  if (!is_supported_420(format) || CVPixelBufferGetPlaneCount(pb) != 2) {
    return status::unsupported_format;
  }
  // The visible size is frame->width/height; the buffer may be larger (decoder
  // alignment). The ring matches the PLANE, as the D3D11 ring matches the pool.
  *out_texture_w = static_cast<std::uint32_t>(CVPixelBufferGetWidthOfPlane(pb, 0)) & ~1u;
  *out_texture_h = static_cast<std::uint32_t>(CVPixelBufferGetHeightOfPlane(pb, 0)) & ~1u;
  *out_ten_bit = is_ten_bit(format);
  return status::ok;
}

status copy_hw_surface(AVBufferRef* hw_device_ctx, const AVFrame* frame,
                       video_frame* dst) noexcept {
  mac_hw_state* state = state_of(hw_device_ctx);
  if (!state || !frame || !dst || !dst->luma || !dst->chroma) return status::invalid_arg;
  CVPixelBufferRef pb = pixel_buffer_of(frame);
  if (!pb) return status::corrupt;

  const bool ten_bit = is_ten_bit(CVPixelBufferGetPixelFormatType(pb));
  const MTLPixelFormat plane_format[2] = {
      ten_bit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm,
      ten_bit ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm};
  void* const dst_texture[2] = {dst->luma, dst->chroma};

  status result = status::ok;
  @autoreleasepool {
    id<MTLCommandBuffer> cb = [state->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    CVMetalTextureRef held[2] = {nullptr, nullptr};

    for (size_t plane = 0; plane < 2 && result == status::ok; ++plane) {
      const size_t w = CVPixelBufferGetWidthOfPlane(pb, plane);
      const size_t h = CVPixelBufferGetHeightOfPlane(pb, plane);
      if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, state->cache, pb,
                                                    nullptr, plane_format[plane], w, h, plane,
                                                    &held[plane]) != kCVReturnSuccess ||
          !held[plane]) {
        result = status::internal;
        break;
      }
      id<MTLTexture> src = CVMetalTextureGetTexture(held[plane]);
      id<MTLTexture> to = (__bridge id<MTLTexture>)dst_texture[plane];
      const NSUInteger cw = std::min<NSUInteger>(src.width, to.width);
      const NSUInteger ch = std::min<NSUInteger>(src.height, to.height);
      [blit copyFromTexture:src
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(cw, ch, 1)
                  toTexture:to
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [blit endEncoding];
    if (result == status::ok) {
      [cb commit];
      [cb waitUntilCompleted];
      if (cb.status != MTLCommandBufferStatusCompleted) result = status::internal;
    }
    for (CVMetalTextureRef ref : held) {
      if (ref) CFRelease(ref);
    }
    CVMetalTextureCacheFlush(state->cache, 0);
  }
  return result;
}

status create_texture_from_planes(gpu_device_ptr device, std::uint32_t width, std::uint32_t height,
                                  bool ten_bit, const std::uint8_t* luma, int luma_pitch,
                                  const std::uint8_t* chroma, int chroma_pitch,
                                  video_frame* out) noexcept {
  (void)device;
  if (!out || !out->luma || !out->chroma || !luma || !chroma || width == 0 || height == 0) {
    return status::invalid_arg;
  }
  // The Metal ring's slots already own their textures (frame_ring_mac.mm), so
  // this fills one in place -- there is no new texture to create and hand over.
  // The slot is not visible to the render thread until commit().
  id<MTLTexture> luma_tex = (__bridge id<MTLTexture>)out->luma;
  id<MTLTexture> chroma_tex = (__bridge id<MTLTexture>)out->chroma;
  if (luma_tex.width < width || luma_tex.height < height ||
      chroma_tex.width < width / 2 || chroma_tex.height < height / 2) {
    return status::invalid_arg;
  }
  (void)ten_bit;
  [luma_tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0
                withBytes:luma
              bytesPerRow:static_cast<NSUInteger>(luma_pitch)];
  [chroma_tex replaceRegion:MTLRegionMake2D(0, 0, width / 2, height / 2)
                mipmapLevel:0
                  withBytes:chroma
                bytesPerRow:static_cast<NSUInteger>(chroma_pitch)];
  return status::ok;
}

}  // namespace mv::player
