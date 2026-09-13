// SPDX-License-Identifier: GPL-2.0-or-later
// Metal device for the Darwin present lab (PR 16).
//
// Apple Silicon only (D9). The default system device is the one that drives
// the screen; there is no hybrid-GPU adapter walk on this floor.
#pragma once

#include "core/result.h"

namespace mv::gfx {

class metal_device {
 public:
  metal_device() = default;
  ~metal_device();

  metal_device(const metal_device&) = delete;
  metal_device& operator=(const metal_device&) = delete;

  [[nodiscard]] expected create() noexcept;
  void destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return device_ != nullptr; }

  // id<MTLDevice> / id<MTLCommandQueue>, borrowed. Valid until destroy().
  [[nodiscard]] void* native_device() const noexcept { return device_; }
  [[nodiscard]] void* native_queue() const noexcept { return queue_; }

 private:
  void* device_ = nullptr;
  void* queue_ = nullptr;
};

}  // namespace mv::gfx
