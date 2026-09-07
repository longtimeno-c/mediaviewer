// SPDX-License-Identifier: GPL-2.0-or-later
// Player-private plumbing shared by the six 5a translation units: the packet
// queue, the frame ring, the decoder context, and the two entry points
// hwdecode_win.cpp exports.
//
// OWNER: mediaviewer-48 (5a). Approved as a header because the alternative —
// repeating extern declarations in each consumer — is an ODR trap.
//
// This header is player-PRIVATE and must stay that way: nothing above player/
// may include it. It may name FFmpeg types (player/ is where they are allowed);
// the contract headers next to it may not.
//
// It must NOT include <libavutil/hwcontext_d3d11va.h> or any other header that
// drags in d3d11.h. That one lives in hwdecode_win.cpp, which is the sole D9
// port boundary. tools/check-hostable-core.ps1 now enforces that specifically.
//
// ID3D11Device / ID3D11Texture2D below arrive TRANSITIVELY through
// player/video_source.h -> gfx/device.h, which is the same allowance
// image/gpu_image.h already uses. No direct d3d11.h include appears here.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "core/result.h"
#include "core/status.h"
#include "gfx/colour_desc.h"
#include "player/video_source.h"
#include "player/av_clock.h"

namespace mv::player {

// ---------------------------------------------------------------------------
// FFmpeg RAII. No exceptions on the hot path, so every one of these is a
// unique_ptr with a noexcept deleter rather than a try/finally.
// ---------------------------------------------------------------------------

struct packet_deleter {
  void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct frame_deleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct codec_ctx_deleter {
  void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct format_ctx_deleter {
  void operator()(AVFormatContext* f) const noexcept { avformat_close_input(&f); }
};
struct buffer_ref_deleter {
  void operator()(AVBufferRef* b) const noexcept { av_buffer_unref(&b); }
};
struct sws_deleter {
  void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};

using packet_ptr     = std::unique_ptr<AVPacket, packet_deleter>;
using frame_ptr      = std::unique_ptr<AVFrame, frame_deleter>;
using codec_ctx_ptr  = std::unique_ptr<AVCodecContext, codec_ctx_deleter>;
using format_ctx_ptr = std::unique_ptr<AVFormatContext, format_ctx_deleter>;
using buffer_ref_ptr = std::unique_ptr<AVBufferRef, buffer_ref_deleter>;
using sws_ptr        = std::unique_ptr<SwsContext, sws_deleter>;

// ---------------------------------------------------------------------------
// Bounded packet queue. plan/05: "packet queue (bounded, ~2 s)".
//
// This is a hand-rolled SPSC ring rather than core/spsc_ring.h for one reason:
// the demux thread must BLOCK when the queue is full, or it reads a 10-minute
// file into memory as fast as the disk allows. core/spsc_ring.h is wait-free by
// contract and its producer drops instead — correct for render-thread
// snapshots, wrong for a packet queue where dropping a packet corrupts the
// stream. Blocking is safe here because neither end is the UI or render thread
// (CLAUDE.md rule 1).
// ---------------------------------------------------------------------------

// 512 packets is >2 s of 4K video at any bitrate we meet, and the memory is the
// packets' own, not the queue's — the ring holds pointers.
inline constexpr std::size_t packet_queue_capacity = 512;

class packet_queue {
 public:
  ~packet_queue() { clear(); }

  // [demux-thread] Blocks while full. Returns false once stopped.
  [[nodiscard]] bool push(packet_ptr packet) noexcept;

  // [decode-thread] Blocks while empty. Returns false once stopped and drained.
  [[nodiscard]] bool pop(packet_ptr& out) noexcept;
  [[nodiscard]] bool try_push(packet_ptr& packet, std::uint32_t generation) noexcept;
  [[nodiscard]] bool try_pop(packet_ptr& out, std::uint32_t* generation) noexcept;

  // [decode-thread] Non-blocking; false when empty.
  [[nodiscard]] bool try_pop(packet_ptr& out) noexcept;

  // [any-thread] Wakes both ends so the threads can exit.
  void stop() noexcept;
  void clear() noexcept;

  // [any-thread] Drop everything queued, e.g. on seek, without stopping.
  void flush() noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

 private:
  mutable std::mutex      mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  struct item { packet_ptr packet; std::uint32_t generation = 0; };
  std::deque<item> items_;
  std::atomic<bool>       stopped_{false};
};

// ---------------------------------------------------------------------------
// The presentation ring — textures WE own.
//
// plan/05 "Surface ownership": a D3D11VA output surface is a (pool texture,
// array slice) pair, not a texture of its own. Holding one keeps that slice out
// of the decoder's pool until the decoder stalls, which reads as a periodic
// hitch that looks like a decode performance problem and isn't. We copy out and
// release the AVFrame at once.
//
// Two SPSC index rings, no locks, no allocation after create():
//   free_  : producer = render thread (release), consumer = decode thread
//   ready_ : producer = decode thread (commit),  consumer = render thread
// A slot the decode thread has taken but not committed is held in
// `pending_index_`, which only the decode thread touches — that is why there is
// no abandon() path pushing back onto free_ and breaking single-producer.
//
// Hand-rolled rather than core/spsc_ring.h because peek_next_pts() has to read
// the oldest queued frame's PTS WITHOUT dequeuing it (the clock's drop-vs-hold
// is undecidable without that peek, per the contract), and a ring with no peek
// cannot express it.
// ---------------------------------------------------------------------------

// plan/05: "3-4 is plenty". Four at 4K P010 is ~100 MB of VRAM, which is the
// right trade against a decoder stall.
inline constexpr std::uint32_t frame_ring_slots = 4;

class frame_ring {
 public:
  frame_ring() = default;
  ~frame_ring();

  frame_ring(const frame_ring&) = delete;
  frame_ring& operator=(const frame_ring&) = delete;

  // `texture_w/h` are the DECODER'S allocation, not the visible frame: a
  // D3D11VA surface is padded up to the decoder's alignment. Ours must match
  // exactly, because CopySubresourceRegion with a null box requires identical
  // dimensions. The visible size travels separately in video_frame.
  [[nodiscard]] expected create(ID3D11Device* device, std::uint32_t texture_w,
                                std::uint32_t texture_h, bool ten_bit);
  void destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return initialized_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint32_t texture_width() const noexcept { return texture_w_; }
  [[nodiscard]] std::uint32_t texture_height() const noexcept { return texture_h_; }
  [[nodiscard]] bool ten_bit() const noexcept { return ten_bit_; }

  // [decode-thread] A free slot to write into, or null when the render thread
  // has not released one yet. Never blocks: the decoder holds the AVFrame a
  // moment longer instead, which is bounded by extra_hw_frames.
  [[nodiscard]] video_frame* begin_write() noexcept;
  // [decode-thread] Publish the slot returned by begin_write().
  void commit() noexcept;

  // [render-thread] Oldest frame at or before `deadline_ns` whose generation
  // matches, or null. Stale-generation frames are recycled here, never shown.
  [[nodiscard]] video_frame* acquire(std::uint32_t generation, time_ns deadline_ns) noexcept;
  // [render-thread] Return a frame from acquire() to the free list.
  void release(video_frame* frame) noexcept;
  // [render-thread] Discard the oldest queued frame without presenting it.
  void drop_oldest() noexcept;

  // [any-thread] PTS of the oldest queued frame. False when the queue is empty.
  [[nodiscard]] bool peek_next_pts(time_ns* out_pts_ns) const noexcept;

  [[nodiscard]] std::uint32_t queued() const noexcept;

 private:
  [[nodiscard]] expected create_slot(video_frame& slot);

  std::atomic<bool> initialized_{false};
  ID3D11Device* device_ = nullptr;  // borrowed; the shell outlives the clip
  std::uint32_t texture_w_ = 0;
  std::uint32_t texture_h_ = 0;
  bool          ten_bit_ = false;

  video_frame slots_[frame_ring_slots]{};

  // Index rings. Capacity is slots+1 so full and empty stay distinguishable.
  static constexpr std::uint32_t index_capacity = frame_ring_slots + 1;
  std::uint32_t free_slots_[index_capacity]{};
  std::uint32_t ready_slots_[index_capacity]{};
  alignas(64) std::atomic<std::uint32_t> free_head_{0};   // written by render
  alignas(64) std::atomic<std::uint32_t> free_tail_{0};   // written by decode
  alignas(64) std::atomic<std::uint32_t> ready_head_{0};  // written by decode
  alignas(64) std::atomic<std::uint32_t> ready_tail_{0};  // written by render

  // Decode-thread private: the slot taken by begin_write() and not yet
  // committed. Keeping it here rather than pushing it back to free_ is what
  // keeps free_ single-producer.
  std::uint32_t pending_index_ = index_capacity;
};

// ---------------------------------------------------------------------------
// hwdecode_win.cpp — the D9 port boundary. A Metal host replaces this file and
// nothing else. Everything that needs BOTH an FFmpeg type and a D3D11 type is
// behind these four functions.
// ---------------------------------------------------------------------------

// Builds the FFmpeg D3D11VA hardware device context from OUR ID3D11Device
// (plan/05: "not a device FFmpeg makes"). The device is AddRef'd for the
// lifetime of the returned ref.
[[nodiscard]] result<AVBufferRef*> create_hw_device_ctx(ID3D11Device* device) noexcept;

// The decoder pool's allocation size behind a hardware frame, and its depth.
// The allocation is padded up to the decoder's alignment; the visible size is
// frame->width/height and is NOT the same number.
[[nodiscard]] status describe_hw_surface(const AVFrame* frame, std::uint32_t* out_texture_w,
                                         std::uint32_t* out_texture_h,
                                         bool* out_ten_bit) noexcept;

// [decode-thread] Copies the (pool texture, array slice) pair behind `frame`
// into `dst`, then returns. The caller releases the AVFrame immediately after.
//
// THE ORDERING GUARANTEE THAT MAKES THIS SAFE: the copy goes to the SAME
// immediate context FFmpeg's decode submits through (AVD3D11VADeviceContext
// carries device_context, and video_context is QI'd off it), so D3D11 orders
// the copy ahead of the decoder's next write to that slice. Move this copy to a
// private or deferred context and that guarantee silently disappears — you get
// intermittent wrong-frame corruption that only shows under DPB pressure, i.e.
// on 4K clips and never in a short test. See plan/12, 2026-09-07.
//
// The lock taken is the AVHWDeviceContext's own (hwctx->lock), not
// ID3D10Multithread directly: it defaults to the same lock but stays correct if
// a future FFmpeg changes the default.
//
// Copies only. No Map, no Flush, no ClearState, no query wait, no GPU sync of
// any kind happens on this thread.
[[nodiscard]] status copy_hw_surface(AVBufferRef* hw_device_ctx, const AVFrame* frame,
                                     ID3D11Texture2D* dst) noexcept;

// Software-decode fallback. Creates a NEW texture from CPU planes with
// D3D11_SUBRESOURCE_DATA and no device context at all — which is plan/02's
// blessed worker-thread pattern, and deliberately does not extend the
// immediate-context exception above to a path that does not need it.
[[nodiscard]] status create_texture_from_planes(ID3D11Device* device, std::uint32_t width,
                                                std::uint32_t height, bool ten_bit,
                                                const std::uint8_t* luma, int luma_pitch,
                                                const std::uint8_t* chroma, int chroma_pitch,
                                                video_frame* out) noexcept;

// ---------------------------------------------------------------------------
// The pipeline: demux thread + video decode thread + the ring between them.
// demux.cpp owns run_demux_thread, video_decode.cpp owns run_video_decode_thread,
// video_source.cpp owns the video_source implementation over both.
// ---------------------------------------------------------------------------

struct video_pipeline {
  // Set up by video_source.cpp before the threads start.
  format_ctx_ptr  format;
  codec_ctx_ptr   codec;
  buffer_ref_ptr  hw_device;
  gfx::com_ptr<ID3D11Device> device_owner;
  ID3D11Device*   device = nullptr;
  int             video_stream = -1;
  int             audio_stream = -1;  // demuxed for 5b; dropped while unclaimed
  AVRational      time_base{0, 1};
  time_ns         start_time_ns = 0;

  packet_queue    video_packets;
  packet_queue    audio_packets;
  av_clock        clock;
  std::atomic<double> rate{1.0};
  std::atomic<int> selected_audio{-1};
  std::atomic<time_ns> exact_target_ns{-1};
  std::atomic<time_ns> audio_target_ns{0};
  std::atomic<bool> video_done{false};
  frame_ring      ring;

  video_stream_info info{};
  gfx::colour_desc  colour{};

  // Bumped by flush()/seek. Frames decoded at an older value are discarded at
  // acquire and never presented (plan/02 generation counters).
  std::atomic<std::uint32_t> generation{1};
  std::atomic<bool>          stopping{false};
  std::atomic<bool>          eof{false};

  // Seek intent, consumed by the demux thread. -1 means none pending.
  std::atomic<time_ns>       seek_request_ns{-1};
  std::atomic<bool>          seek_exact{false};

  // Diagnostics the F3 overlay reads.
  //
  // TWO different numbers, and conflating them makes healthy playback read as
  // broken — the same mistake av_clock.h calls out for cadence vs starvation:
  //
  //  ring_backpressure : the decode thread was ready to receive a frame and the
  //      ring was full. NORMAL. It means decode is running ahead of the
  //      presentation clock, which is what we want; the decoder keeps its whole
  //      DPB while we wait, so nothing is starved.
  //  surface_waits : the decode thread was HOLDING a decoded frame and had
  //      nowhere to copy it. THIS is the plan/05 hazard — a held frame is a held
  //      DPB slice. It should be zero by construction, because a ring slot is
  //      reserved BEFORE avcodec_receive_frame is called.
  std::atomic<std::uint64_t> frames_decoded{0};
  std::atomic<std::uint64_t> frames_dropped_stale{0};
  std::atomic<std::uint64_t> ring_backpressure{0};
  std::atomic<std::uint64_t> surface_waits{0};
  std::atomic<std::uint64_t> decode_errors{0};

  // What the decoder ACTUALLY produced, set from the first frame's pixel
  // format. 0 = nothing yet; otherwise a decoder_kind. Asking for hardware and
  // getting it are different things: libdav1d accepts a hw_device_ctx and
  // ignores it, which would otherwise report as D3D11VA while decoding on the
  // CPU — exactly the silent fallback plan/05 forbids.
  std::atomic<std::uint8_t> observed_decoder{0};

  std::thread demux_thread;
  std::thread decode_thread;
  std::thread audio_thread;
};

void run_demux_thread(video_pipeline& pipe) noexcept;
void run_video_decode_thread(video_pipeline& pipe) noexcept;
void run_audio_decode_thread(video_pipeline& pipe) noexcept;

// Opens the decoder for `stream`, preferring D3D11VA on our device and falling
// back to software. Lives in video_decode.cpp because the get_format callback
// that selects the hardware path does. Sets pipe.info.decoder to what was
// actually opened — the F3 overlay reads it, so the fallback is never silent.
[[nodiscard]] expected open_video_codec(video_pipeline& pipe, AVStream* stream);

// video_source.cpp. Separate from open_media so media_source.cpp can own the
// clock and transport without also owning the container.
[[nodiscard]] result<video_source*> open_video_source(const char* utf8_path, void* device);
void close_video_source(video_source* source) noexcept;

// Diagnostics behind a video_source this module created. Null for anything
// else. Used by media_source.cpp to fill clock_stats for the F3 overlay.
[[nodiscard]] video_pipeline* pipeline_of(video_source* source) noexcept;

// Maps FFmpeg's AVCOL_* onto gfx::colour_desc. Pure; tested in
// tests/test_video_colour.cpp without a device or a clip.
[[nodiscard]] gfx::colour_desc colour_from_stream(int avcol_space, int avcol_primaries,
                                                  int avcol_trc, int avcol_range,
                                                  int bit_depth) noexcept;

// Rescales a stream PTS to nanoseconds and makes it stream-relative.
// AV_NOPTS_VALUE maps to `fallback`.
[[nodiscard]] time_ns pts_to_ns(std::int64_t pts, AVRational time_base, time_ns start_time_ns,
                                time_ns fallback) noexcept;

}  // namespace mv::player
