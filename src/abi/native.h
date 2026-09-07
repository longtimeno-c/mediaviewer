// SPDX-License-Identifier: GPL-2.0-or-later
// Native-only entry points. Not in the C ABI, not P/Invoked. The language
// boundary does not carry ID3D11* (plan/14); the present lab, being C++, is
// allowed to bind the device that lives on the render thread to the session
// that owns decode jobs.
#pragma once

#include <memory>

#include "core/status.h"
#include "image/gpu_image.h"
#include "mediaviewer/mediaviewer.h"

struct ID3D11Device;

namespace mv::abi {

// Exported from mediaviewer_core.dll. These are not in the C ABI (plan/14
// forbids ID3D11* there); they exist so the native present lab can bind the
// render thread's device to the session that owns decode jobs. Ownership of a
// taken gpu_image returns to the DLL via release_gpu_image — new and delete
// must stay on the same heap.

[[nodiscard]] MV_API status attach_device(mv_session_t session, ID3D11Device* device);
MV_API void detach_device(mv_session_t session);

// Render-thread only. Returns the most recently uploaded image, or null.
// Caller owns it until release_gpu_image. Wait-free: no mutex.
[[nodiscard]] MV_API image::gpu_image* take_ready_image(mv_session_t session);
MV_API void release_gpu_image(image::gpu_image* image);

// Auto-reset event signalled when a new GPU image is published. The render
// thread waits on this alongside its own wake event so an idle still appears
// without polling. Null if the session is null.
[[nodiscard]] MV_API void* image_ready_wait_handle(mv_session_t session);

struct gpu_image_deleter {
  void operator()(image::gpu_image* p) const noexcept { release_gpu_image(p); }
};

using gpu_image_ptr = std::unique_ptr<image::gpu_image, gpu_image_deleter>;

}  // namespace mv::abi
