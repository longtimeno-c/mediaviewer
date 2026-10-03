// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5a - avcodec video decode, D3D11VA first and software as a visible
// fallback, copied out of the decoder pool into our own presentation ring.
//
// OWNER: mediaviewer-48 (5a).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {
namespace {

// avcodec calls this to let us pick the surface format. Returning AV_PIX_FMT_D3D11
// is what selects the hardware path; anything else and the decoder produces CPU
// frames and we take the software fallback.
// ProRes on the Mac (D5 amended, docs/design/12 2026-09-29): VideoToolbox decodes it
// in hardware on Apple silicon, but to 4:2:2 or 4:4:4, and the ring and the
// shader take 4:2:0. VideoToolbox converts to any layout it is asked for, so
// ask it for P010 (10-bit 4:2:0, the HEVC Main10 layout) and ProRes stays on
// the GPU path. Other codecs keep FFmpeg's default frames (their native 4:2:0).
// False when VideoToolbox would not take the request: the software path then
// decodes the clip, visibly.
[[nodiscard]] bool request_420_frames(AVCodecContext* ctx) noexcept {
  if constexpr (kHwDeviceType != AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
    return true;
  } else {
    if (ctx->codec_id != AV_CODEC_ID_PRORES) return true;
    AVBufferRef* frames = nullptr;
    if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, kHwPixFmt, &frames) < 0 || !frames) {
      return false;
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->sw_format = AV_PIX_FMT_P010;
    if (av_hwframe_ctx_init(frames) < 0) {
      av_buffer_unref(&frames);
      return false;
    }
    av_buffer_unref(&ctx->hw_frames_ctx);
    ctx->hw_frames_ctx = frames;
    MV_LOG_INFO("player: ProRes via %s, as 10-bit 4:2:0", kHwDecoderName);
    return true;
  }
}

AVPixelFormat pick_hw_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == kHwPixFmt) {
      if (request_420_frames(ctx)) return *p;
      break;  // software, as below
    }
  }
  // docs/design/05: "Fall back to software decode (with a visible indicator in the
  // debug overlay) when the GPU lacks a profile. Never silently."
  MV_LOG_WARN("player: no %s surface format offered; falling back to software decode",
              kHwDecoderName);
  return formats[0];
}

// Software fallback scratch: swscale output in the same NV12/P010 layout the
// hardware path produces, so exactly one shader serves both.
struct sw_convert {
  sws_ptr                    scaler;
  std::vector<std::uint8_t>  luma;
  std::vector<std::uint8_t>  chroma;
  int                        width = 0;
  int                        height = 0;
  AVPixelFormat              source = AV_PIX_FMT_NONE;
  bool                       ten_bit = false;

  [[nodiscard]] bool prepare(const AVFrame* frame) noexcept {
    const auto src = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src);
    const bool wants_ten_bit = desc && desc->comp[0].depth > 8;
    if (scaler && width == frame->width && height == frame->height && source == src &&
        ten_bit == wants_ten_bit) {
      return true;
    }
    width = frame->width;
    height = frame->height;
    source = src;
    ten_bit = wants_ten_bit;
    // P010LE stores its 10 bits in the high bits of each 16-bit word, which is
    // exactly DXGI_FORMAT_P010's layout — the two agree, so no shift is needed
    // here and the shader's shift serves both paths.
    const AVPixelFormat dst = ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    scaler.reset(sws_getContext(width, height, source, width, height, dst, SWS_BILINEAR, nullptr,
                                nullptr, nullptr));
    if (!scaler) return false;
    const std::size_t bytes = ten_bit ? 2u : 1u;
    luma.assign(static_cast<std::size_t>(width) * height * bytes, 0);
    chroma.assign(static_cast<std::size_t>((width + 1) / 2) * ((height + 1) / 2) * 2 * bytes, 0);
    return true;
  }

  [[nodiscard]] bool convert(const AVFrame* frame) noexcept {
    if (!prepare(frame)) return false;
    const int pitch = width * (ten_bit ? 2 : 1);
    std::uint8_t* dst[4] = {luma.data(), chroma.data(), nullptr, nullptr};
    int strides[4] = {pitch, ((width + 1) / 2) * 2 * (ten_bit ? 2 : 1), 0, 0};
    return sws_scale(scaler.get(), frame->data, frame->linesize, 0, height, dst, strides) ==
           height;
  }

  // A hardware frame in a layout the ring cannot take (4:2:2 / 4:4:4 decoder
  // output): copied back to memory and converted like a software frame, so
  // the clip plays -- slower, and said so -- rather than every frame being
  // dropped while the audio plays over nothing (docs/design/05 "never silently").
  frame_ptr readback;
  bool readback_logged = false;

  [[nodiscard]] bool convert_hw(const AVFrame* hw) noexcept {
    if (!readback) readback.reset(av_frame_alloc());
    if (!readback) return false;
    av_frame_unref(readback.get());
    if (av_hwframe_transfer_data(readback.get(), hw, 0) < 0) return false;
    if (!readback_logged) {
      readback_logged = true;
      const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(readback->format));
      MV_LOG_WARN("player: %s surface layout %s is not 4:2:0; copying frames back for software conversion",
                  kHwDecoderName, name ? name : "?");
    }
    return convert(readback.get());
  }
};

// Reserves a ring slot BEFORE a frame is received from the decoder.
//
// The ordering is the whole point. Receiving first and then looking for a slot
// means the decode thread sits on a decoded frame — which IS a held DPB slice —
// every time presentation is behind, and that is precisely the stall docs/design/05
// warns about. Reserving first turns the same situation into ordinary
// back-pressure: the decoder simply keeps its pool and produces nothing until
// we can take delivery.
//
// So waiting here is NORMAL and is counted as ring_backpressure, not as a
// fault. The fault counter is surface_waits, incremented only where a frame is
// already in hand with nowhere to go.
[[nodiscard]] video_frame* reserve_slot(video_pipeline& pipe, std::uint32_t generation) noexcept {
  if (video_frame* slot = pipe.ring.begin_write()) return slot;
  pipe.ring_backpressure.fetch_add(1, std::memory_order_relaxed);
  while (!pipe.stopping.load(std::memory_order_acquire)) {
    if (video_frame* slot = pipe.ring.begin_write()) return slot;
    // A seek while we are parked here must not be made to wait for a render
    // thread that may never call again — the caller re-enters at the new
    // generation and flushes the codec.
    if (pipe.generation.load(std::memory_order_acquire) != generation) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

// Prefers a decoder that actually advertises D3D11VA over whichever decoder
// FFmpeg happens to register first for this codec id.
//
// This is not hypothetical: for AV1 the vcpkg build registers libdav1d, which
// accepts a hw_device_ctx and quietly ignores it. Taking the default would
// decode 4K AV1 on the CPU while every diagnostic said "D3D11VA" — the silent
// software fallback docs/design/05 forbids, wearing a hardware label.
[[nodiscard]] const AVCodec* pick_decoder(AVCodecID id) noexcept {
  void* iter = nullptr;
  while (const AVCodec* candidate = av_codec_iterate(&iter)) {
    if (candidate->id != id || !av_codec_is_decoder(candidate)) continue;
    for (int i = 0;; ++i) {
      const AVCodecHWConfig* config = avcodec_get_hw_config(candidate, i);
      if (!config) break;
      if (config->device_type == kHwDeviceType &&
          (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
        return candidate;
      }
    }
  }
  return avcodec_find_decoder(id);
}

// Publishes one decoded frame into the ring. Returns false when the frame could
// not be placed and should be dropped.
[[nodiscard]] bool publish_frame(video_pipeline& pipe, AVFrame* frame, sw_convert& sw,
                                 std::uint32_t generation, video_frame* slot) noexcept {
  bool hardware = frame->format == kHwPixFmt;

  std::uint32_t texture_w = 0;
  std::uint32_t texture_h = 0;
  bool ten_bit = false;

  if (hardware && describe_hw_surface(frame, &texture_w, &texture_h, &ten_bit) != status::ok) {
    if (!sw.convert_hw(frame)) return false;
    hardware = false;  // the software upload below, and the overlay says so
    texture_w = static_cast<std::uint32_t>(sw.width) & ~1u;
    texture_h = static_cast<std::uint32_t>(sw.height) & ~1u;
    ten_bit = sw.ten_bit;
  } else if (!hardware) {
    if (!sw.convert(frame)) return false;
    texture_w = static_cast<std::uint32_t>(frame->width);
    texture_h = static_cast<std::uint32_t>(frame->height);
    ten_bit = sw.ten_bit;
    // 4:2:0 needs even dimensions; an odd-sized software frame cannot become an
    // NV12 texture at all.
    texture_w &= ~1u;
    texture_h &= ~1u;
  }

  if (!pipe.ring.valid()) {
    // The ring is sized from the DECODER'S allocation, which is why it is built
    // on the first frame rather than at open: the padding is not knowable from
    // the container.
    if (auto r = pipe.ring.create(pipe.device, texture_w, texture_h, ten_bit); !r) {
      MV_LOG_WARN("player: presentation ring create failed (%s)", status_name(r.error()));
      return false;
    }
  } else if (pipe.ring.texture_width() != texture_w || pipe.ring.texture_height() != texture_h ||
             pipe.ring.ten_bit() != ten_bit) {
    // Mid-stream resolution or bit-depth change. Rebuilding the ring here would
    // free textures the render thread may still hold, so the frame is dropped
    // instead. Not in the D5 v1 set; logged rather than handled silently.
    MV_LOG_WARN("player: frame geometry changed mid-stream; dropping frame");
    return false;
  }

  if (!slot) slot = pipe.ring.begin_write(); // First frame creates the ring above.
  if (!slot) {
    // A frame in hand with nowhere to copy it. This is the docs/design/05 hazard and
    // should be unreachable, because the slot was reserved before the frame was
    // received. Counted rather than ignored so it cannot hide.
    pipe.surface_waits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  if (hardware) {
#if defined(MV_DARWIN)
    if (copy_hw_surface(pipe.hw_device.get(), frame, slot) != status::ok) return false;
#else
    if (copy_hw_surface(pipe.hw_device.get(), frame, slot->texture.Get()) != status::ok) {
      return false;
    }
#endif
  } else {
    const int pitch = sw.width * (ten_bit ? 2 : 1);
    if (create_texture_from_planes(pipe.device, texture_w, texture_h, ten_bit, sw.luma.data(),
                                   pitch, sw.chroma.data(), ((sw.width + 1) / 2) * 2 * (ten_bit ? 2 : 1), slot) != status::ok) {
      return false;
    }
  }

  // What the decoder ACTUALLY produced, not what we asked for.
  pipe.observed_decoder.store(
      static_cast<std::uint8_t>(hardware ? kHwDecoderKind : decoder_kind::software),
      std::memory_order_release);

  slot->pts_ns = pts_to_ns(frame->best_effort_timestamp, pipe.time_base, pipe.start_time_ns,
                           pts_to_ns(frame->pts, pipe.time_base, pipe.start_time_ns, 0));
  slot->width = static_cast<std::uint32_t>(frame->width);
  slot->height = static_cast<std::uint32_t>(frame->height);
  slot->generation = generation;
  slot->ten_bit = ten_bit;
  slot->colour = gfx::resolve_unspecified(
      colour_from_stream(frame->colorspace, frame->color_primaries, frame->color_trc,
                         frame->color_range, ten_bit ? 10 : 8), slot->width, slot->height);
  pipe.ring.commit();
  pipe.frames_decoded.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace

expected open_video_codec(video_pipeline& pipe, AVStream* stream) {
  if (!stream || !pipe.device) return err(status::invalid_arg);

  const AVCodec* codec = pick_decoder(stream->codecpar->codec_id);
  if (!codec) return err(status::unsupported_format);

  pipe.codec.reset(avcodec_alloc_context3(codec));
  if (!pipe.codec) return err(status::out_of_memory);
  if (avcodec_parameters_to_context(pipe.codec.get(), stream->codecpar) < 0) {
    return err(status::corrupt);
  }
  pipe.codec->pkt_timebase = stream->time_base;

  // Try hardware first. A failure to build the D3D11VA context is not fatal —
  // it routes to software, which the overlay names.
  if (auto hw = create_hw_device_ctx(pipe.device)) {
    pipe.hw_device.reset(hw.value());
    pipe.codec->hw_device_ctx = av_buffer_ref(pipe.hw_device.get());
    pipe.codec->get_format = pick_hw_format;
    // docs/design/12 (2026-09-07): belt-and-braces for "the decoder never stalls
    // waiting for a surface over a 10-minute play". The copy is what keeps the
    // pool free; this covers a scheduling delay between decode and copy so a
    // late render thread can never starve the DPB.
    pipe.codec->extra_hw_frames = static_cast<int>(frame_ring_slots) + 4;
  } else {
    MV_LOG_WARN("player: %s unavailable (%s); software decode", kHwDecoderName,
                status_name(hw.error()));
  }

  // Threaded software decode still matters: it is the fallback path's only
  // chance of keeping up, and it is ignored on the hardware path.
  pipe.codec->thread_count = 0;

  if (avcodec_open2(pipe.codec.get(), codec, nullptr) < 0) {
    return err(status::unsupported_format);
  }

  // What was ACTUALLY opened. hw_device_ctx surviving avcodec_open2 is the
  // honest test: asking for hardware and getting it are different things.
  pipe.info.decoder =
      pipe.codec->hw_device_ctx ? kHwDecoderKind : decoder_kind::software;

  const char* name = codec->name ? codec->name : "?";
  std::snprintf(pipe.info.codec_name, sizeof(pipe.info.codec_name), "%s", name);

  MV_LOG_INFO("player: %s decode via %s", name,
              pipe.info.decoder == kHwDecoderKind ? kHwDecoderName : "software");
  return {};
}

void run_video_decode_thread(video_pipeline& pipe) noexcept {
  frame_ptr frame(av_frame_alloc());
  if (!frame) {
    pipe.decode_errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  sw_convert sw;
  std::uint32_t local_generation = pipe.generation.load(std::memory_order_acquire);

  while (!pipe.stopping.load(std::memory_order_acquire)) {
    // A generation bump means a seek or a navigation happened. Flush the codec
    // so no pre-seek frame is decoded into the post-seek generation
    // (docs/design/05: "Flush decoders on every seek").
    const std::uint32_t generation = pipe.generation.load(std::memory_order_acquire);
    if (generation != local_generation) {
      avcodec_flush_buffers(pipe.codec.get());
      local_generation = generation;
    }

    packet_ptr packet;
    std::uint32_t packet_generation = 0;
    if (!pipe.video_packets.try_pop(packet, &packet_generation)) {
      if (pipe.stopping.load(std::memory_order_acquire)) break;
      // Stopped-and-drained, or EOF with nothing queued. Idle rather than spin;
      // a seek can still restart the demuxer.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (packet_generation != local_generation || pipe.generation.load() != local_generation) continue;
    int rc = avcodec_send_packet(pipe.codec.get(), packet.get());
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
      pipe.decode_errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    for (;;) {
      video_frame* slot = pipe.ring.valid() ? reserve_slot(pipe, local_generation) : nullptr;
      if (pipe.stopping.load() || pipe.generation.load() != local_generation) break;
      rc = avcodec_receive_frame(pipe.codec.get(), frame.get());
      if (rc == AVERROR_EOF) {
        // A seek bumps generation and flushes; do not stamp the new generation
        // done with the previous play-out's drain.
        if (pipe.generation.load() == local_generation) pipe.video_done.store(true);
        break;
      }
      if (rc == AVERROR(EAGAIN)) break;
      if (rc < 0) {
        pipe.decode_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      if (pipe.generation.load(std::memory_order_acquire) != local_generation) {
        // Seek landed while this frame was in flight. Discarding it here is
        // cheaper than presenting it and letting the presenter drop it.
        pipe.frames_dropped_stale.fetch_add(1, std::memory_order_relaxed);
        av_frame_unref(frame.get());
        break;
      }
      const auto pts = pts_to_ns(frame->best_effort_timestamp, pipe.time_base, pipe.start_time_ns, 0);
      const auto target = pipe.exact_target_ns.load();
      if (target >= 0 && pts < target) { av_frame_unref(frame.get()); continue; }
      if (!publish_frame(pipe, frame.get(), sw, local_generation, slot)) {
        pipe.frames_dropped_stale.fetch_add(1, std::memory_order_relaxed);
      }
      // Released the moment the copy is submitted — the whole point of copying
      // out of the pool (docs/design/05 "Surface ownership").
      av_frame_unref(frame.get());
    }
  }
}

void run_still_thread(video_pipeline& pipe) noexcept {
  sw_convert sw;
  std::uint32_t published = 0;  // the generation the still last went out at
  while (!pipe.stopping.load(std::memory_order_acquire)) {
    const std::uint32_t generation = pipe.generation.load(std::memory_order_acquire);
    // Wait for the seek that bumped the generation to finish writing its
    // intent (seek_request_ns is the last thing media_source::seek stores, and
    // the demux thread clears it), or the video_done below would be undone by
    // the seek's own reset and the file would never end.
    if (generation == published || pipe.seek_request_ns.load(std::memory_order_acquire) >= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    video_frame* slot = pipe.ring.valid() ? reserve_slot(pipe, generation) : nullptr;
    if (pipe.stopping.load() || pipe.generation.load() != generation) continue;
    // Stamped with the position the seek asked for, so the seek's preview
    // takes it at once and the presenter holds it from there.
    const time_ns at = std::max<time_ns>(0, pipe.audio_target_ns.load());
    const std::int64_t pts =
        av_rescale_q(at + pipe.start_time_ns, AVRational{1, 1'000'000'000}, pipe.time_base);
    pipe.still->pts = pts;
    pipe.still->best_effort_timestamp = pts;
    if (!publish_frame(pipe, pipe.still.get(), sw, generation, slot)) {
      pipe.frames_dropped_stale.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    published = generation;
    if (pipe.generation.load() == generation) pipe.video_done.store(true);
  }
}

}  // namespace mv::player
