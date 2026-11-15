// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's conversion (clip_convert.h). Software decode on this thread and
// at most one more, as every clip job; the encoder is the GPU's.
#include "edit/clip_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/json.h"
#include "edit/clip_audio_enc.h"
#include "edit/clip_internal.h"
#include "edit/clip_pixels.h"
#include "edit/hwencode.h"

namespace mv::edit::clip::convert {

using namespace clip::detail;

namespace {

constexpr AVRational kTs{1, 90000};
constexpr time_ns kTol = 500'000;  // half a millisecond: a frame on the boundary belongs to the later segment
constexpr int kDecodeThreads = 2;

struct swr_deleter {
  void operator()(SwrContext* c) const noexcept { swr_free(&c); }
};
using swr_ptr = std::unique_ptr<SwrContext, swr_deleter>;

[[nodiscard]] AVRational frame_rate_of(const AVStream* st) noexcept {
  AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
  if (fr.num <= 0 || fr.den <= 0 || av_q2d(fr) > 240.0) fr = AVRational{30, 1};
  return fr;
}

// About 0.1 bit per pixel per frame: 8 Mbit/s for 1080p30.
[[nodiscard]] std::int64_t bitrate_for(std::uint32_t w, std::uint32_t h, AVRational fr) noexcept {
  const double bits = 0.1 * static_cast<double>(w) * static_cast<double>(h) * av_q2d(fr);
  return std::clamp<std::int64_t>(static_cast<std::int64_t>(bits), 1'500'000, 10'000'000);
}

struct opened_encoder {
  codec_ptr ctx;
  std::string name;
};

[[nodiscard]] result<opened_encoder> open_video_encoder(std::uint32_t w, std::uint32_t h, AVRational fr,
                                                        const detail::encoders& with) {
  for (const std::string& name : with.names) {
    const AVCodec* c = avcodec_find_encoder_by_name(name.c_str());
    if (c == nullptr || c->type != AVMEDIA_TYPE_VIDEO || !licence_ok(c, with.allow_software)) continue;
    codec_ptr ctx(avcodec_alloc_context3(c));
    if (!ctx) return err(status::out_of_memory);
    ctx->width = static_cast<int>(w);
    ctx->height = static_cast<int>(h);
    ctx->sample_aspect_ratio = AVRational{1, 1};
    ctx->pix_fmt = pick_format(encoder_formats(c), false);
    ctx->time_base = kTs;
    if (c->id == AV_CODEC_ID_MPEG4) ctx->time_base = av_inv_q(fr);  // the tests' software one: 16-bit time base
    ctx->framerate = fr;
    ctx->bit_rate = bitrate_for(w, h, fr);
    ctx->rc_max_rate = ctx->bit_rate * 3 / 2;
    ctx->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(ctx->bit_rate * 2, INT32_MAX));
    // One IDR at the segment's start; the next segment brings its own.
    ctx->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(fr) * 10)));
    ctx->max_b_frames = 0;
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (c->id == AV_CODEC_ID_H264) ctx->profile = AV_PROFILE_H264_HIGH;
    if (avcodec_open2(ctx.get(), c, nullptr) < 0) continue;  // not this machine's GPU
    return opened_encoder{std::move(ctx), name};
  }
  return err(status::not_found);
}

[[nodiscard]] detail::encoders hardware_h264() {
  detail::encoders e;
  for (const char* n : hwencode::candidates(hwencode::codec::h264)) e.names.emplace_back(n);
  return e;
}

// A tail shorter than this joins the segment before (a container's duration
// often runs a few milliseconds past the last frame).
constexpr time_ns kShortestTail = 250'000'000;

[[nodiscard]] std::uint32_t segment_count(time_ns duration, std::uint32_t segment_ms) noexcept {
  const time_ns seg = static_cast<time_ns>(segment_ms) * 1'000'000;
  const time_ns whole = duration / seg;
  return static_cast<std::uint32_t>(std::max<time_ns>(1, whole + (duration % seg >= kShortestTail ? 1 : 0)));
}

// Receives every packet ready and writes it, timestamps moved by kTimestampOffsetMs.
[[nodiscard]] expected drain_to(AVCodecContext* enc, AVFormatContext* out, AVStream* ost, AVPacket* pkt,
                                std::int64_t offset_in_enc_tb) {
  while (true) {
    const int rc = avcodec_receive_packet(enc, pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
    if (rc < 0) return err(status::internal);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += offset_in_enc_tb;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += offset_in_enc_tb;
    if (pkt->duration <= 0 && enc->codec_type == AVMEDIA_TYPE_VIDEO && enc->framerate.num > 0) {
      pkt->duration = av_rescale_q(1, av_inv_q(enc->framerate), enc->time_base);
    }
    av_packet_rescale_ts(pkt, enc->time_base, ost->time_base);
    pkt->stream_index = ost->index;
    if (av_interleaved_write_frame(out, pkt) < 0) return err(status::io);
  }
}

}  // namespace

namespace detail {

void output_size(int width, int height, int sar_num, int sar_den, std::uint32_t& out_w, std::uint32_t& out_h) noexcept {
  double w = std::max(2, width), h = std::max(2, height);
  if (sar_num > 0 && sar_den > 0 && sar_num != sar_den) w = w * sar_num / sar_den;
  const double long_side = std::max(w, h);
  if (long_side > kMaxLongSide) {
    const double s = kMaxLongSide / long_side;
    w *= s;
    h *= s;
  }
  const auto even = [](double v) { return static_cast<std::uint32_t>(std::max(2L, std::lround(v / 2.0) * 2)); };
  out_w = even(w);
  out_h = even(h);
}

expected segment_with(std::string_view utf8_path, std::uint32_t k, std::uint32_t segment_ms,
                      const std::string& out_path, const std::atomic<bool>* cancel, const encoders& with) {
  if (segment_ms == 0) return err(status::invalid_arg);
  MV_TRY(source s, open_source(utf8_path, cancel));
  if (s.video < 0 || s.duration_ns <= 0) return err(status::unsupported_format);
  const std::uint32_t count = segment_count(s.duration_ns, segment_ms);
  if (k >= count) return err(status::invalid_arg);
  const time_ns seg = static_cast<time_ns>(segment_ms) * 1'000'000;
  const time_ns start = static_cast<time_ns>(k) * seg;
  // The last runs to the file's end, whatever the container said its duration was.
  const time_ns end = k + 1 == count ? INT64_MAX / 4 : start + seg;
  const time_ns offset_ns = kTimestampOffsetMs * 1'000'000;

  AVStream* vst = s.format->streams[s.video];
  MV_TRY(codec_ptr vdec, open_decoder(vst, kDecodeThreads));
  std::uint32_t ow = 0, oh = 0;
  output_size(vst->codecpar->width, vst->codecpar->height, vst->codecpar->sample_aspect_ratio.num,
              vst->codecpar->sample_aspect_ratio.den, ow, oh);
  const AVRational fr = frame_rate_of(vst);
  MV_TRY(opened_encoder venc, open_video_encoder(ow, oh, fr, with));

  // The sound: the first audio stream, stereo, from the OS's AAC encoder.
  AVStream* ast = s.audio >= 0 ? s.format->streams[s.audio] : nullptr;
  codec_ptr adec;
  std::optional<audio_encoder> aenc;
  swr_ptr swr;
  int out_rate = 48000;
  if (ast != nullptr) {
    if (auto d = open_decoder(ast, 1)) adec = std::move(*d);
    if (ast->codecpar->sample_rate == 44100) out_rate = 44100;
    if (adec) {
      if (auto e = audio_encoder::open(static_cast<std::uint32_t>(out_rate), 2, false); e && !e->lossless()) {
        aenc.emplace(std::move(*e));
      }
    }
    if (!aenc) adec.reset();  // no AAC here: silent segments, never FLAC in a TS
  }
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.video && !(adec && static_cast<int>(i) == s.audio)) {
      s.format->streams[i]->discard = AVDISCARD_ALL;
    }
  }

  MV_TRY(output_ptr out, open_output("mpegts", out_path, cancel));
  AVStream* vost = avformat_new_stream(out.get(), nullptr);
  if (vost == nullptr || avcodec_parameters_from_context(vost->codecpar, venc.ctx.get()) < 0) {
    return err(status::out_of_memory);
  }
  vost->time_base = kTs;
  AVStream* aost = nullptr;
  if (aenc) {
    aost = avformat_new_stream(out.get(), nullptr);
    if (aost == nullptr || avcodec_parameters_from_context(aost->codecpar, aenc->context()) < 0) {
      return err(status::out_of_memory);
    }
    aost->time_base = kTs;
  }
  if (avformat_write_header(out.get(), nullptr) < 0) {
    return err(cancelled(cancel) ? status::cancelled : status::io);
  }

  packet_ptr pkt(av_packet_alloc()), opkt(av_packet_alloc());
  frame_ptr frame(av_frame_alloc()), conv(av_frame_alloc());
  if (!pkt || !opkt || !frame || !conv) return err(status::out_of_memory);
  sws_ptr sws;
  const AVRational vtb = venc.ctx->time_base;
  const std::int64_t voffset = av_rescale_q(offset_ns, kNs, vtb);
  std::int64_t last_vpts = INT64_MIN;
  bool video_done = false, first_video = true;
  status failure = status::ok;

  const auto on_video = [&](AVFrame* f, time_ns t) -> bool {
    if (t < start - kTol) return true;  // decoding up from the keyframe
    if (t >= end - kTol) {
      video_done = true;
      return false;
    }
    const std::int64_t pts = av_rescale_q(t, kNs, vtb);
    if (pts <= last_vpts) return true;  // a repeated timestamp: one frame per tick
    last_vpts = pts;
    sws = sws_ptr(sws_getCachedContext(sws.release(), f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                       venc.ctx->width, venc.ctx->height, venc.ctx->pix_fmt, SWS_BICUBIC, nullptr,
                                       nullptr, nullptr));
    if (!sws) {
      failure = status::internal;
      return false;
    }
    av_frame_unref(conv.get());
    conv->format = venc.ctx->pix_fmt;
    conv->width = venc.ctx->width;
    conv->height = venc.ctx->height;
    if (av_frame_get_buffer(conv.get(), 0) < 0 ||
        sws_scale(sws.get(), f->data, f->linesize, 0, f->height, conv->data, conv->linesize) <= 0) {
      failure = status::out_of_memory;
      return false;
    }
    conv->pts = pts;
    conv->pict_type = first_video ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (first_video) conv->flags |= AV_FRAME_FLAG_KEY;
    first_video = false;
    if (avcodec_send_frame(venc.ctx.get(), conv.get()) < 0) {
      failure = status::internal;
      return false;
    }
    if (auto d = drain_to(venc.ctx.get(), out.get(), vost, opkt.get(), voffset); !d) {
      failure = d.error();
      return false;
    }
    return true;
  };

  // Sound: resampled to interleaved stereo float, cut to [start, end) by sample.
  //
  // An AAC encoder's first packets are its priming (`initial_padding`, 2112
  // samples for AudioToolbox's). After the first segment the encoder is fed
  // from before `start`, from a sample chosen so that its packets fall on a grid
  // with one starting exactly at `start`, and the packets before it are dropped:
  // the segment's sound starts at its start, primed, and does not overlap the
  // segment before (by at most one packet's tail at its end).
  bool audio_done = !aenc.has_value();
  std::int64_t next_sample = -1;   // the output sample position the next resampled sample has
  std::int64_t first_kept = -1;    // where the encoder's sample 0 is on the timeline
  const std::int64_t start_sample = av_rescale(start, out_rate, 1'000'000'000);
  const std::int64_t end_sample = av_rescale(end, out_rate, 1'000'000'000);
  std::int64_t feed_from = start_sample;
  if (aenc && k > 0) {
    const std::int64_t frame = aenc->context()->frame_size > 0 ? aenc->context()->frame_size : 1024;
    const std::int64_t pad = std::max(0, aenc->context()->initial_padding);
    const std::int64_t frames_back = (2 * pad + frame - 1) / frame;
    feed_from = start_sample + pad - frames_back * frame;
  }
  std::vector<float> pcm;
  const auto audio_sink = [&](AVPacket* p) -> expected {
    // The encoder's time base is 1 / out_rate, from sample 0 = first_kept.
    if (k > 0 && p->pts != AV_NOPTS_VALUE && first_kept + p->pts < start_sample) return {};  // the pre-roll
    if (p->pts != AV_NOPTS_VALUE && first_kept + p->pts >= end_sample) return {};
    const std::int64_t shift = first_kept + av_rescale(offset_ns, out_rate, 1'000'000'000);
    if (p->pts != AV_NOPTS_VALUE) p->pts += shift;
    if (p->dts != AV_NOPTS_VALUE) p->dts += shift;
    av_packet_rescale_ts(p, aenc->context()->time_base, aost->time_base);
    p->stream_index = aost->index;
    if (av_interleaved_write_frame(out.get(), p) < 0) return err(status::io);
    return {};
  };
  const auto on_audio = [&](AVFrame* f) -> bool {
    const std::int64_t ts = frame_ts(f);
    if (!swr) {
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      SwrContext* raw = nullptr;
      if (swr_alloc_set_opts2(&raw, &stereo, AV_SAMPLE_FMT_FLT, out_rate, &f->ch_layout,
                              static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
          swr_init(raw) < 0) {
        swr_free(&raw);
        audio_done = true;  // a layout it cannot take: the picture alone
        return true;
      }
      swr.reset(raw);
    }
    if (next_sample < 0) {
      if (ts == AV_NOPTS_VALUE) return true;
      next_sample = av_rescale(to_timeline(s, ast, ts), out_rate, 1'000'000'000);
    }
    const int room = swr_get_out_samples(swr.get(), f->nb_samples);
    if (room <= 0) return true;
    pcm.resize(static_cast<std::size_t>(room) * 2);
    auto* dst = reinterpret_cast<std::uint8_t*>(pcm.data());
    const int got = swr_convert(swr.get(), &dst, room, const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
    if (got <= 0) return true;
    std::int64_t from = next_sample;
    next_sample += got;
    std::int64_t lo = std::max(from, feed_from), hi = std::min(next_sample, end_sample);
    if (from >= end_sample) {
      audio_done = true;
      return true;
    }
    if (hi <= lo) return true;
    if (first_kept < 0) first_kept = lo;
    const std::span<const float> keep(pcm.data() + (lo - from) * 2, static_cast<std::size_t>(hi - lo) * 2);
    if (auto r = aenc->push(keep, audio_sink); !r) {
      failure = r.error();
      return false;
    }
    if (next_sample >= end_sample) audio_done = true;
    return true;
  };

  // A demuxer with no index (MPEG-PS) can land on the keyframe after the
  // target: aim a second early, and further back while the first video packet
  // after the seek is still past `start`.
  int seeks = 0;
  bool seek_checked = start <= 0;
  const auto seek_to = [&]() {
    const time_ns back = std::int64_t{1'000'000'000} << seeks;  // 1 s, 2 s, 4 s
    seek_before(s, std::max<time_ns>(0, start - back));  // the first segment reads from where the file opens
    avcodec_flush_buffers(vdec.get());
    ++seeks;
  };
  seek_to();
  const auto drain_decoder = [&](AVCodecContext* dec, bool video) -> bool {
    while (true) {
      const int rc = avcodec_receive_frame(dec, frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
      if (rc < 0) return true;  // a damaged frame: skipped, as the player does
      bool go = true;
      if (video) {
        const std::int64_t ts = frame_ts(frame.get());
        if (!video_done && ts != AV_NOPTS_VALUE) go = on_video(frame.get(), to_timeline(s, vst, ts));
      } else if (!audio_done) {
        go = on_audio(frame.get());
      }
      av_frame_unref(frame.get());
      if (!go && failure != status::ok) return false;
    }
  };
  while (!(video_done && audio_done)) {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(s.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      break;
    }
    const bool is_video = pkt->stream_index == s.video;
    if (!seek_checked) {
      // Nothing is decoded until the seek is known to be before `start`.
      const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
      if (!is_video || ts == AV_NOPTS_VALUE) {
        av_packet_unref(pkt.get());
        continue;
      }
      if (to_timeline(s, vst, ts) > start + kTol && seeks < 3) {
        av_packet_unref(pkt.get());
        seek_to();
        continue;
      }
      seek_checked = true;  // and this packet is decoded below
    }
    AVCodecContext* dec = is_video ? vdec.get() : (adec && pkt->stream_index == s.audio ? adec.get() : nullptr);
    if (dec != nullptr && !(is_video ? video_done : audio_done)) {
      (void)avcodec_send_packet(dec, pkt.get());  // a bad packet is skipped, not fatal
      if (!drain_decoder(dec, is_video)) {
        av_packet_unref(pkt.get());
        return err(failure);
      }
    }
    av_packet_unref(pkt.get());
  }
  // At the file's end: what the decoders hold.
  if (!video_done) {
    (void)avcodec_send_packet(vdec.get(), nullptr);
    if (!drain_decoder(vdec.get(), true)) return err(failure);
  }
  if (adec && !audio_done) {
    (void)avcodec_send_packet(adec.get(), nullptr);
    if (!drain_decoder(adec.get(), false)) return err(failure);
  }
  if (failure != status::ok) return err(failure);
  if (first_video) return err(status::corrupt);  // not one frame in the range
  if (avcodec_send_frame(venc.ctx.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(drain_to(venc.ctx.get(), out.get(), vost, opkt.get(), voffset));
  if (aenc && first_kept >= 0) MV_TRY_VOID(aenc->flush(audio_sink));
  if (av_write_trailer(out.get()) < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
  return {};
}

}  // namespace detail

result<plan_info> plan(std::string_view utf8_path, std::uint32_t segment_ms) {
  if (segment_ms == 0) return err(status::invalid_arg);
  MV_TRY(source s, open_source(utf8_path, nullptr));
  if (s.video < 0 || s.duration_ns <= 0) return err(status::unsupported_format);
  const AVCodecParameters* p = s.format->streams[s.video]->codecpar;
  if (p->width <= 0 || p->height <= 0) return err(status::unsupported_format);
  plan_info out;
  out.duration_ms = s.duration_ns / 1'000'000;
  out.segment_ms = segment_ms;
  out.segments = segment_count(s.duration_ns, segment_ms);
  detail::output_size(p->width, p->height, p->sample_aspect_ratio.num, p->sample_aspect_ratio.den, out.width,
                      out.height);
  out.video_codec = avcodec_get_name(p->codec_id);
  if (s.audio >= 0) out.audio_codec = avcodec_get_name(s.format->streams[s.audio]->codecpar->codec_id);
  return out;
}

std::string plan_json(const plan_info& p) {
  json::writer w;
  w.begin_object()
      .key("duration_ms")
      .integer(p.duration_ms)
      .key("segment_ms")
      .integer(p.segment_ms)
      .key("segments")
      .integer(p.segments)
      .key("width")
      .integer(p.width)
      .key("height")
      .integer(p.height)
      .end_object();
  return w.take();
}

expected segment(std::string_view utf8_path, std::uint32_t k, std::uint32_t segment_ms, const std::string& out_path,
                 const std::atomic<bool>* cancel) {
  return detail::segment_with(utf8_path, k, segment_ms, out_path, cancel, hardware_h264());
}

void quiet_logs() noexcept { av_log_set_level(AV_LOG_QUIET); }

result<encoder_found> check() {
  MV_TRY(opened_encoder e, open_video_encoder(1280, 720, AVRational{30, 1}, hardware_h264()));
  return encoder_found{e.name, hwencode::family_label(e.name.c_str())};
}

}  // namespace mv::edit::clip::convert
