// SPDX-License-Identifier: GPL-2.0-or-later
// Native-only entry points. Not in the C ABI, not P/Invoked. The language
// boundary does not carry ID3D11* (plan/14); the present lab, being C++, is
// allowed to bind the device that lives on the render thread to the session
// that owns decode jobs.
#pragma once

#include <memory>
#include <span>

#include "core/status.h"
#include "image/gpu_image.h"
#include "image/tiles.h"
#include "player/video_source.h"
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

// PR 7 tiled pyramid (plan/04), for an image whose `tiles` is set. Render
// thread only, never blocks: marks and requests what `view` needs, evicts over
// budget, and returns the ready tiles to draw coarse to fine (valid until the
// next call for this image). Empty for an untiled image.
[[nodiscard]] MV_API std::span<const gfx::tile_quad> tiles_frame(const image::gpu_image& image,
                                                                 const image::tile_view& view) noexcept;
[[nodiscard]] MV_API image::tile_stats tiles_stats(const image::gpu_image& image) noexcept;

// PR 6 animated GIF / APNG / WebP (plan/04). Native-only like poll_video: not
// in the C ABI, not P/Invoked (plan/16: no new hot-path ABI). The render thread
// pulls frames from the session's decode ring; none of these wait.
//
// Takes the next decoded frame for `generation`, if one is ready. The caller
// owns `texture` until release_gpu_image.
[[nodiscard]] MV_API bool take_animation_frame(mv_session_t session, std::uint32_t generation,
                                               image::gpu_image*& texture,
                                               std::uint32_t& delay_ms,
                                               std::uint32_t& index) noexcept;
// An animation was published for `generation` (the item on screen is animated).
[[nodiscard]] MV_API bool animation_open(mv_session_t session, std::uint32_t generation) noexcept;
// Its loop count is exhausted (or it turned out to be a still): nothing more comes.
[[nodiscard]] MV_API bool animation_finished(mv_session_t session,
                                             std::uint32_t generation) noexcept;
[[nodiscard]] MV_API bool animation_loops_forever(mv_session_t session) noexcept;
// The view moved to `generation`: older animation work stops, queued frames go.
MV_API void animation_retire(mv_session_t session, std::uint32_t generation) noexcept;
// The next frame produced is `index` (`,` stepping back).
MV_API void animation_seek(mv_session_t session, std::uint32_t index) noexcept;

struct animation_stats {
  std::uint32_t depth = 0;
  std::uint32_t queued = 0;
  std::uint32_t last_upload_us = 0;  // whole make: colour conversion + CreateTexture2D
  std::uint32_t last_icc_us = 0;     // of which, colour conversion
  std::uint64_t frames_made = 0;
};
[[nodiscard]] MV_API animation_stats animation_stats_now(mv_session_t session) noexcept;

// Auto-reset event signalled when a new GPU image is published. The render
// thread waits on this alongside its own wake event so an idle still appears
// without polling. Null if the session is null.
[[nodiscard]] MV_API void* image_ready_wait_handle(mv_session_t session);

[[nodiscard]] MV_API bool poll_video(mv_session_t session, player::time_ns vblank,
                                      player::video_frame& frame, bool& active);

// [any-thread][wait-free] True from the moment a clip is published on this
// session until it is retired — i.e. before the first decoded frame exists.
// The render thread needs the distinction: "no texture" is the empty window,
// "no texture yet, clip open" is a clip loading, and the two must not paint
// the same thing.
[[nodiscard]] MV_API bool video_open(mv_session_t session) noexcept;

// PR 5a video. Same wait-free handoff as take_ready_image, but a video frame is
// its own type: two SRVs, an NV12/P010 format, a colour_desc and a PTS. It is
// deliberately NOT forced into image::gpu_image.
[[nodiscard]] MV_API player::video_frame* take_ready_video_frame(mv_session_t session,
                                                                 std::uint32_t generation);
MV_API void release_video_frame(player::video_frame* frame);

struct video_frame_deleter {
  void operator()(player::video_frame* p) const noexcept { release_video_frame(p); }
};

using video_frame_ptr = std::unique_ptr<player::video_frame, video_frame_deleter>;

struct gpu_image_deleter {
  void operator()(image::gpu_image* p) const noexcept { release_gpu_image(p); }
};

using gpu_image_ptr = std::unique_ptr<image::gpu_image, gpu_image_deleter>;

}  // namespace mv::abi
