// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a — decoded video frames, in textures WE own.
//
// Contract owner: mediaviewer-56. 5a implements, 5b and 5c consume. Do not
// change a field here without saying so centrally; three sessions bind to it.
//
// Why this is not image::gpu_image: a video frame is two SRVs (luma + chroma),
// an NV12/P010 DXGI format, a colour_desc, a PTS and a generation. Forcing it
// into the still path's one-texture-one-SRV shape loses all of that.
#pragma once

#include <cstdint>

#include "core/status.h"
#include "gfx/colour_desc.h"
#include "gfx/device.h"

namespace mv::player {

// TIME IS int64 NANOSECONDS EVERYWHERE in player/, suffixed _ns. Chosen over
// microseconds so audio can account in whole samples without rounding: at
// 48 kHz one sample is 20833.33 ns. int64 ns overflows in ~292 years.
using time_ns = std::int64_t;

// A frame in OUR presentation ring — never a decoder-pool (DPB) surface.
// plan/05 "Surface ownership": a D3D11VA output surface is a (pool texture,
// array slice) pair, and holding one removes it from the pool until the
// decoder stalls. We CopySubresourceRegion out and release the AVFrame at once.
struct video_frame {
  gfx::com_ptr<ID3D11Texture2D>          texture;  // ours, from the ring
  gfx::com_ptr<ID3D11ShaderResourceView> luma;     // R8_UNORM  / R16_UNORM
  gfx::com_ptr<ID3D11ShaderResourceView> chroma;   // R8G8_UNORM / R16G16_UNORM
  time_ns          pts_ns     = 0;   // stream-relative: start_time already subtracted
  std::uint32_t    width      = 0;
  std::uint32_t    height     = 0;
  // The VIEW generation this frame was decoded at. Navigation bumps it; a stale
  // frame is dropped at acquire, never presented (plan/02 generation counters).
  std::uint32_t    generation = 0;
  gfx::colour_desc colour{};
  bool             ten_bit    = false;  // P010 when true, NV12 when false
};

// Where the pixels actually came from. The F3 overlay names this, so a software
// fallback is never silent (plan/05: "Never silently — a silent software-decode
// fallback on a 4K clip reads to the user as 'this app is slow.'").
enum class decoder_kind : std::uint8_t {
  none = 0,
  d3d11va,   // hardware, on OUR ID3D11Device
  software,  // avcodec CPU path
};

struct video_stream_info {
  std::uint32_t width          = 0;
  std::uint32_t height         = 0;
  double        frame_rate     = 0.0;  // nominal only; we present on PTS, so VFR is free
  time_ns       duration_ns    = 0;
  // Container start_time. Non-zero in MPEG-TS and elsewhere; if it is not
  // subtracted it reads as a constant A/V offset that looks like a clock bug.
  time_ns       start_time_ns  = 0;
  decoder_kind  decoder        = decoder_kind::none;
  bool          ten_bit        = false;
  char          codec_name[32] = {};  // e.g. "hevc", "av1" — for the overlay
};

// Owns demux + video decode + the presentation ring for one clip.
// Implemented by 5a. All methods [any-thread][no-block] unless marked.
class video_source {
 public:
  virtual ~video_source() = default;

  [[nodiscard]] virtual video_stream_info info() const noexcept = 0;

  // [render-thread] Hand over the frame at or before `deadline_ns`, or null.
  // Never blocks, never waits on a decode lock (CLAUDE.md rule 1). Frames whose
  // generation != `generation` are discarded here rather than presented.
  [[nodiscard]] virtual video_frame* acquire(std::uint32_t generation,
                                             time_ns deadline_ns) noexcept = 0;
  virtual void release(video_frame* frame) noexcept = 0;

  // [any-thread][no-block] PTS of the next queued frame without dequeuing it.
  // Required by the clock: drop-vs-hold is undecidable without the peek.
  // Returns false when the queue is empty (starved -> hold current frame).
  [[nodiscard]] virtual bool peek_next_pts(time_ns* out_pts_ns) const noexcept = 0;

  // [any-thread][no-block] Discard queued frames and flush the decoder
  // (avcodec_flush_buffers) at the new generation. plan/05 transport.
  virtual void flush(std::uint32_t generation) noexcept = 0;
};

}  // namespace mv::player
