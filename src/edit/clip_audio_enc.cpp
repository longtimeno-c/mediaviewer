// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/clip_audio_enc.h"

#include <algorithm>
#include <cmath>

namespace mv::edit::clip::detail {
namespace {

// The OS's AAC encoders, then FLAC (docs/design/11: never FFmpeg's own AAC).
constexpr const char* kEncoders[] = {"aac_at", "aac_mf", "flac"};

std::vector<AVSampleFormat> sample_formats(const AVCodec* c) {
  std::vector<AVSampleFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* cfg = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n) >= 0 && cfg) {
    const auto* f = static_cast<const AVSampleFormat*>(cfg);
    out.assign(f, f + n);
  }
#else
  for (const AVSampleFormat* f = c->sample_fmts; f && *f != AV_SAMPLE_FMT_NONE; ++f) out.push_back(*f);
#endif
  return out;
}

AVSampleFormat pick(const std::vector<AVSampleFormat>& have) {
  for (const AVSampleFormat f : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32,
                                 AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16}) {
    if (have.empty() || std::find(have.begin(), have.end(), f) != have.end()) return f;
  }
  return AV_SAMPLE_FMT_NONE;
}

}  // namespace

result<audio_encoder> audio_encoder::open(std::uint32_t rate, std::uint32_t channels, bool global) {
  for (const char* name : kEncoders) {
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (!c || c->type != AVMEDIA_TYPE_AUDIO) continue;
    const AVSampleFormat fmt = pick(sample_formats(c));
    if (fmt == AV_SAMPLE_FMT_NONE) continue;
    codec_ptr ctx(avcodec_alloc_context3(c));
    if (!ctx) return err(status::out_of_memory);
    ctx->sample_fmt = fmt;
    ctx->sample_rate = static_cast<int>(rate);
    av_channel_layout_default(&ctx->ch_layout, static_cast<int>(channels));
    const bool flac = c->id == AV_CODEC_ID_FLAC;
    // About 96 kb/s a channel: transparent for speech and music at 48 kHz.
    if (!flac) ctx->bit_rate = std::min<std::int64_t>(96'000 * channels, 512'000);
    if (fmt == AV_SAMPLE_FMT_S32 || fmt == AV_SAMPLE_FMT_S32P) ctx->bits_per_raw_sample = 24;
    ctx->time_base = AVRational{1, static_cast<int>(rate)};
    if (global) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx.get(), c, nullptr) < 0) continue;  // not on this machine, or not at this rate
    audio_encoder e;
    e.lossless_ = flac;
    e.channels_ = channels;
    e.frame_.reset(av_frame_alloc());
    e.pkt_.reset(av_packet_alloc());
    if (!e.frame_ || !e.pkt_) return err(status::out_of_memory);
    e.frame_->format = ctx->sample_fmt;
    e.frame_->sample_rate = ctx->sample_rate;
    if (av_channel_layout_copy(&e.frame_->ch_layout, &ctx->ch_layout) < 0) return err(status::out_of_memory);
    e.frame_->nb_samples = ctx->frame_size > 0 ? ctx->frame_size : 1024;
    if (av_frame_get_buffer(e.frame_.get(), 0) < 0) return err(status::out_of_memory);
    e.ctx_ = std::move(ctx);
    return e;
  }
  return err(status::unsupported_format);
}

expected audio_encoder::send(int samples) {
  AVFrame* f = frame_.get();
  f->nb_samples = samples;
  if (av_frame_make_writable(f) < 0) return err(status::out_of_memory);
  const int ch = static_cast<int>(channels_);
  const auto fmt = static_cast<AVSampleFormat>(f->format);
  const bool planar = av_sample_fmt_is_planar(fmt) != 0;
  for (int i = 0; i < samples; ++i) {
    for (int c = 0; c < ch; ++c) {
      float v = fifo_[static_cast<std::size_t>(i * ch + c)];
      v = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
      const int at = planar ? i : i * ch + c;
      std::uint8_t* base = f->extended_data[planar ? c : 0];
      switch (fmt) {
        case AV_SAMPLE_FMT_FLTP:
        case AV_SAMPLE_FMT_FLT: reinterpret_cast<float*>(base)[at] = v; break;
        case AV_SAMPLE_FMT_S32P:
        case AV_SAMPLE_FMT_S32:
          reinterpret_cast<std::int32_t*>(base)[at] = static_cast<std::int32_t>(std::lrint(v * 8388607.0f)) * 256;
          break;
        default:
          reinterpret_cast<std::int16_t*>(base)[at] = static_cast<std::int16_t>(std::lrint(v * 32767.0f));
          break;
      }
    }
  }
  f->pts = next_pts_;
  next_pts_ += samples;
  fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(samples) * ch);
  return avcodec_send_frame(ctx_.get(), f) < 0 ? expected{err(status::internal)} : expected{};
}

}  // namespace mv::edit::clip::detail
