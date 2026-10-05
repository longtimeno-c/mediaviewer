// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/poster.h"

#include <algorithm>

#include "codec/card.h"
#include "player/video_internal.h"

namespace mv::player {
namespace {

// Clips very often open on black, a slate, or a fade. Landing 10 % in gives a
// tile that looks like the clip. Capped so a 40-minute file does not pay for a
// long seek, and floored to 0 for anything short enough that 10 % is noise.
constexpr time_ns kPosterOffsetCapNs = 3'000'000'000;

[[nodiscard]] bool cancelled(const job_context* ctx) noexcept {
  return ctx != nullptr && ctx->cancelled();
}

// An audio file's tile when it has no cover art: the same card the player
// shows (docs/plans/audio-and-documents.md §2.2), at the tile's size.
poster_image card_poster(codec::card_kind kind, std::uint32_t max_long_edge) {
  const std::uint32_t w = std::max<std::uint32_t>(2, max_long_edge);
  const codec::card card = codec::make_card(kind, w, std::max<std::uint32_t>(2, w * 9 / 16));
  poster_image out;
  out.width = card.width;
  out.height = card.height;
  out.rgba = card.rgba;
  return out;
}

// Turns a packed RGBA image by `turns` quarter turns clockwise, in a new
// buffer (a 512-px poster: well under a millisecond).
void rotate_rgba(poster_image& img, unsigned turns) {
  turns &= 3u;
  if (turns == 0 || img.width == 0 || img.height == 0) return;
  const std::uint32_t w = img.width, h = img.height;
  const std::uint32_t ow = (turns & 1u) ? h : w;
  const std::uint32_t oh = (turns & 1u) ? w : h;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(ow) * oh * 4u);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* src = img.rgba.data() + static_cast<std::size_t>(y) * w * 4u;
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t dx = 0, dy = 0;
      switch (turns) {
        case 1: dx = h - 1 - y; dy = x; break;          // 90 clockwise
        case 2: dx = w - 1 - x; dy = h - 1 - y; break;  // 180
        default: dx = y; dy = w - 1 - x; break;         // 270 clockwise
      }
      std::uint8_t* dst = out.data() + (static_cast<std::size_t>(dy) * ow + dx) * 4u;
      dst[0] = src[x * 4u]; dst[1] = src[x * 4u + 1]; dst[2] = src[x * 4u + 2]; dst[3] = src[x * 4u + 3];
    }
  }
  img.width = ow;
  img.height = oh;
  img.rgba.swap(out);
}

}  // namespace

result<poster_image> poster_frame(const char* utf8_path, std::uint32_t max_long_edge,
                                  const job_context* ctx) {
  if (!utf8_path || max_long_edge == 0) return err(status::invalid_arg);

  AVFormatContext* raw_format = nullptr;
  if (avformat_open_input(&raw_format, utf8_path, nullptr, nullptr) < 0) return err(status::io);
  format_ctx_ptr format(raw_format);
  if (avformat_find_stream_info(format.get(), nullptr) < 0) return err(status::corrupt);
  if (cancelled(ctx)) return err(status::cancelled);

  const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    const int audio = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio < 0) return err(status::unsupported_format);
    return card_poster(is_protected_audio(format->streams[audio]) ? codec::card_kind::locked
                                                               : codec::card_kind::music,
                       max_long_edge);
  }
  AVStream* stream = format->streams[stream_index];

  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) return err(status::unsupported_format);
  codec_ctx_ptr decoder(avcodec_alloc_context3(codec));
  if (!decoder) return err(status::out_of_memory);
  if (avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0) return err(status::corrupt);
  // One thread on purpose. Thumb jobs already run one per pool thread; letting
  // each of them spawn a frame-threaded decoder oversubscribes the machine the
  // playing clip is decoding on.
  decoder->thread_count = 1;
  if (avcodec_open2(decoder.get(), codec, nullptr) < 0) return err(status::unsupported_format);

  // Nearest keyframe, backward — a poster frame is not worth decoding forward
  // for. A failed seek is not fatal: frame 0 is a perfectly good fallback.
  const time_ns duration_ns =
      format->duration == AV_NOPTS_VALUE
          ? 0
          : static_cast<time_ns>(av_rescale_q(format->duration, AVRational{1, AV_TIME_BASE},
                                              AVRational{1, 1'000'000'000}));
  const time_ns want_ns = std::min(duration_ns / 10, kPosterOffsetCapNs);
  if (want_ns > 0) {
    const std::int64_t target =
        av_rescale_q(want_ns, AVRational{1, 1'000'000'000}, stream->time_base) +
        (stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time);
    if (av_seek_frame(format.get(), stream_index, target, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(decoder.get());
    }
  }

  packet_ptr packet(av_packet_alloc());
  frame_ptr frame(av_frame_alloc());
  if (!packet || !frame) return err(status::out_of_memory);

  bool have_frame = false;
  bool sent_eof = false;
  while (!have_frame) {
    if (cancelled(ctx)) return err(status::cancelled);
    const int rc = avcodec_receive_frame(decoder.get(), frame.get());
    if (rc == 0) {
      have_frame = true;
      break;
    }
    // EOF here means the decoder drained without ever producing a frame; any
    // other error is a decode failure. Neither gives us a poster.
    if (rc != AVERROR(EAGAIN)) return err(status::corrupt);
    if (sent_eof) return err(status::corrupt);

    int read = 0;
    do {
      av_packet_unref(packet.get());
      read = av_read_frame(format.get(), packet.get());
      if (cancelled(ctx)) return err(status::cancelled);
    } while (read >= 0 && packet->stream_index != stream_index);

    if (read < 0) {
      (void)avcodec_send_packet(decoder.get(), nullptr);
      sent_eof = true;
      continue;
    }
    if (avcodec_send_packet(decoder.get(), packet.get()) < 0) return err(status::corrupt);
    av_packet_unref(packet.get());
  }

  if (frame->width <= 0 || frame->height <= 0) return err(status::corrupt);

  // Anamorphic clips (DV, some phone slow-motion) store square-ish pixels and a
  // sample aspect ratio. Thumbing the coded size makes them look squashed next
  // to the photos they were shot alongside.
  std::uint32_t src_w = static_cast<std::uint32_t>(frame->width);
  std::uint32_t src_h = static_cast<std::uint32_t>(frame->height);
  const AVRational sar = av_guess_sample_aspect_ratio(format.get(), stream, frame.get());
  std::uint32_t display_w = src_w;
  std::uint32_t display_h = src_h;
  if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
    if (sar.num > sar.den) {
      display_w = static_cast<std::uint32_t>(
          av_rescale(static_cast<std::int64_t>(src_w), sar.num, sar.den));
    } else {
      display_h = static_cast<std::uint32_t>(
          av_rescale(static_cast<std::int64_t>(src_h), sar.den, sar.num));
    }
  }
  if (display_w == 0) display_w = 1;
  if (display_h == 0) display_h = 1;

  const std::uint32_t long_edge = display_w > display_h ? display_w : display_h;
  std::uint32_t out_w = display_w;
  std::uint32_t out_h = display_h;
  if (long_edge > max_long_edge) {
    out_w = display_w * max_long_edge / long_edge;
    out_h = display_h * max_long_edge / long_edge;
  }
  if (out_w == 0) out_w = 1;
  if (out_h == 0) out_h = 1;

  // BT.709 / BT.601 handling is sws_scale's own: it reads the frame's colour
  // metadata. A thumbnail is display-referred by definition, so this is the one
  // place a straight YUV -> sRGB conversion is the right answer and the linear
  // FP16 working space (D6) does not apply.
  sws_ptr sws(sws_getContext(static_cast<int>(src_w), static_cast<int>(src_h),
                             static_cast<AVPixelFormat>(frame->format),
                             static_cast<int>(out_w), static_cast<int>(out_h),
                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) return err(status::unsupported_format);

  poster_image out;
  out.width = out_w;
  out.height = out_h;
  out.rgba.resize(static_cast<std::size_t>(out_w) * out_h * 4u);
  std::uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
  int dst_stride[4] = {static_cast<int>(out_w) * 4, 0, 0, 0};
  if (sws_scale(sws.get(), frame->data, frame->linesize, 0, static_cast<int>(src_h), dst,
                dst_stride) <= 0) {
    return err(status::corrupt);
  }
  if (cancelled(ctx)) return err(status::cancelled);
  // A portrait phone clip is coded landscape with a 90 degree display matrix:
  // its tile is turned the way the player shows it (and the way Photos does).
  const int rotation = stream_rotation_degrees(stream);
  if (rotation != 0) rotate_rgba(out, static_cast<unsigned>(rotation / 90));
  return out;
}

}  // namespace mv::player
