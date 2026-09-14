// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/device_mac.h"

#import <Metal/Metal.h>

#include "core/trace.h"

namespace mv::gfx {

metal_device::~metal_device() { destroy(); }

expected metal_device::create() noexcept {
  destroy();
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    MV_LOG_ERROR("metal_device: MTLCreateSystemDefaultDevice failed");
    return err(status::device_lost);
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (!queue) {
    MV_LOG_ERROR("metal_device: newCommandQueue failed");
    return err(status::device_lost);
  }
  device_ = (__bridge_retained void*)device;
  queue_ = (__bridge_retained void*)queue;
  return {};
}

void metal_device::destroy() noexcept {
  if (queue_) {
    (void)(__bridge_transfer id)queue_;
    queue_ = nullptr;
  }
  if (device_) {
    (void)(__bridge_transfer id)device_;
    device_ = nullptr;
  }
}

}  // namespace mv::gfx
