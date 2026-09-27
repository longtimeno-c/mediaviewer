// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The paths that decode: Path 2 frame-accurate re-encode on a hardware
// encoder, frame -> PNG/JPEG, audio -> WAV/FLAC, and clip -> GIF/WebP.
//
// Decode here is software on the job's own thread, never the player's
// D3D11VA / VideoToolbox decoder on the render device: a clip job must not
// take a surface from the pool the canvas is presenting from (plan/05
// "Surface ownership"), and a job runs whether or not the clip is on screen.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "codec/cicp.h"
#include "codec/raster.h"
#include "edit/clip_internal.h"
#include "edit/clip_pixels.h"
#include "edit/encode.h"

namespace mv::edit::clip::detail {
namespace {

constexpr time_ns kTol = 500'000;

// ---- small RAII -----------------------------------------------------------------

struct swr_deleter {
  void operator()(SwrContext* c) const noexcept { swr_free(&c); }
};
using swr_ptr = std::unique_ptr<SwrContext, swr_deleter>;

struct fifo_deleter {
  void operator()(AVAudioFifo* f) const noexcept { av_audio_fifo_free(f); }
};
using fifo_ptr = std::unique_ptr<AVAudioFifo, fifo_deleter>;

struct graph_deleter {
  void operator()(AVFilterGraph* g) const noexcept { avfilter_graph_free(&g); }
};
using graph_ptr = std::unique_ptr<AVFilterGraph, graph_deleter>;

// Decodes `stream` forward from the keyframe at or before `from`, calling
// `on_frame(frame, t)` for every frame. `on_frame` returns false to stop.
// `on_other` sees every packet of another stream (Path 2 copies audio).
template <class OnFrame, class OnOther>
[[nodiscard]] expected decode_forward(source& s, int stream, AVCodecContext* dec, time_ns from,
                                      const control& ctl, OnFrame&& on_frame, OnOther&& on_other) {
  seek_before(s, from);
  avcodec_flush_buffers(dec);
  AVStream* st = s.format->streams[stream];
  packet_ptr pkt(av_packet_alloc());
  frame_ptr frame(av_frame_alloc());
  if (!pkt || !frame) return err(status::out_of_memory);
  bool stop = false;
  const auto drain = [&]() -> expected {
    while (!stop) {
      const int rc = avcodec_receive_frame(dec, frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return {};  // a damaged frame: skip it, as the player does
      const std::int64_t ts = frame_ts(frame.get());
      if (ts != AV_NOPTS_VALUE) {
        if (!on_frame(frame.get(), to_timeline(s, st, ts))) stop = true;
      }
      av_frame_unref(frame.get());
    }
    return {};
  };
  while (!stop) {
    if (cancelled(ctl.cancel)) return err(status::cancelled);
    const int rc = av_read_frame(s.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      break;
    }
    if (pkt->stream_index == stream) {
      (void)avcodec_send_packet(dec, pkt.get());  // a bad packet is skipped, not fatal
      av_packet_unref(pkt.get());
      MV_TRY_VOID(drain());
    } else {
      if (!on_other(pkt.get())) stop = true;
      av_packet_unref(pkt.get());
    }
  }
  if (!stop) {
    (void)avcodec_send_packet(dec, nullptr);
    MV_TRY_VOID(drain());
  }
  if (cancelled(ctl.cancel)) return err(status::cancelled);
  return {};
}

// Supported software pixel formats of an encoder, in its own order.
[[nodiscard]] std::vector<AVPixelFormat> encoder_formats(const AVCodec* enc) {
  std::vector<AVPixelFormat> out;
  const AVPixelFormat* list = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* cfg = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg) {
    const auto* fmts = static_cast<const AVPixelFormat*>(cfg);
    for (int i = 0; i < n; ++i) {
      const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmts[i]);
      if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(fmts[i]);
    }
    return out;
  }
#else
  list = enc->pix_fmts;
#endif
  for (; list != nullptr && *list != AV_PIX_FMT_NONE; ++list) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*list);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(*list);
  }
  return out;
}

[[nodiscard]] AVPixelFormat pick_format(const std::vector<AVPixelFormat>& fmts, bool ten_bit) {
  const auto has = [&](AVPixelFormat f) { return std::find(fmts.begin(), fmts.end(), f) != fmts.end(); };
  if (ten_bit && has(AV_PIX_FMT_P010LE)) return AV_PIX_FMT_P010LE;
  if (ten_bit && has(AV_PIX_FMT_YUV420P10LE)) return AV_PIX_FMT_YUV420P10LE;
  if (has(AV_PIX_FMT_NV12)) return AV_PIX_FMT_NV12;
  if (has(AV_PIX_FMT_YUV420P)) return AV_PIX_FMT_YUV420P;
  // An encoder that lists nothing (some wrappers) is offered the common case.
  return fmts.empty() ? AV_PIX_FMT_YUV420P : fmts.front();
}

// plan/11: never x264 / x265, never a software HEVC encoder. FFmpeg marks the
// hardware wrappers (NVENC, AMF, VideoToolbox) HARDWARE and the MFT / QSV ones
// HYBRID.
[[nodiscard]] bool licence_ok(const AVCodec* enc, bool allow_software) noexcept {
  if (std::strncmp(enc->name, "libx26", 6) == 0 || std::strcmp(enc->name, "libkvazaar") == 0) return false;
  if (enc->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) return true;
  return allow_software && enc->id != AV_CODEC_ID_HEVC;
}

// Receives every packet an encoder has ready and writes it.
[[nodiscard]] expected drain_encoder(AVCodecContext* enc, AVFormatContext* out, AVStream* ost,
                                     AVPacket* pkt) {
  while (true) {
    const int rc = avcodec_receive_packet(enc, pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
    if (rc < 0) return err(status::internal);
    // Some encoders (mpeg4, several hardware wrappers) leave duration 0. The
    // MP4 / MOV muxer then ends the edit list at the last frame's start and
    // every player hides that frame.
    if (pkt->duration <= 0 && enc->codec_type == AVMEDIA_TYPE_VIDEO && enc->framerate.num > 0) {
      pkt->duration = av_rescale_q(1, av_inv_q(enc->framerate), enc->time_base);
    }
    av_packet_rescale_ts(pkt, enc->time_base, ost->time_base);
    pkt->stream_index = ost->index;
    if (av_interleaved_write_frame(out, pkt) < 0) return err(status::io);
  }
}

}  // namespace

// ---- Path 2: frame-accurate re-encode --------------------------------------------------

result<std::string> reencode(std::string_view src_path, const std::string& out_path,
                             const reencode_spec& spec, const control& ctl) {
  MV_TRY(source s, open_source(src_path, ctl.cancel));
  if (s.video < 0) return err(status::unsupported_format);
  AVStream* ist = s.format->streams[s.video];
  const time_ns in_ns = std::max<time_ns>(0, spec.in_ns);
  const time_ns out_ns = spec.out_ns < 0 ? s.duration_ns : std::min(spec.out_ns, s.duration_ns);
  if (out_ns <= in_ns) return err(status::invalid_arg);
  MV_TRY(codec_ptr dec, open_decoder(ist));

  MV_TRY(output_ptr out, open_output(spec.muxer, out_path, ctl.cancel));
  const AVPixFmtDescriptor* sd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(ist->codecpar->format));
  const bool ten_bit = sd != nullptr && sd->comp[0].depth > 8;
  AVRational fr = ist->avg_frame_rate.num > 0 ? ist->avg_frame_rate : ist->r_frame_rate;
  if (fr.num <= 0 || fr.den <= 0) fr = AVRational{30, 1};

  // Bitrate: the source's video rate (or its share of the file), so the
  // re-encode is not visibly worse than what it came from.
  std::int64_t bitrate = ist->codecpar->bit_rate;
  if (bitrate <= 0 && s.format->bit_rate > 0) bitrate = s.format->bit_rate * 9 / 10;
  if (bitrate <= 0) bitrate = 8'000'000;
  bitrate = std::clamp<std::int64_t>(bitrate * 11 / 10, 1'000'000, 200'000'000);

  codec_ptr enc;
  std::string used;
  for (const std::string& name : spec.encoders) {
    const AVCodec* c = avcodec_find_encoder_by_name(name.c_str());
    if (c == nullptr || c->type != AVMEDIA_TYPE_VIDEO || !licence_ok(c, spec.allow_software)) continue;
    codec_ptr ctx(avcodec_alloc_context3(c));
    if (!ctx) return err(status::out_of_memory);
    ctx->width = ist->codecpar->width;
    ctx->height = ist->codecpar->height;
    ctx->sample_aspect_ratio = ist->codecpar->sample_aspect_ratio;
    ctx->pix_fmt = pick_format(encoder_formats(c), ten_bit && c->id == AV_CODEC_ID_HEVC);
    ctx->time_base = ist->time_base;
    if (ctx->time_base.den > 65535 && c->id == AV_CODEC_ID_MPEG4) ctx->time_base = av_inv_q(fr);
    ctx->framerate = fr;
    ctx->bit_rate = bitrate;
    ctx->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(fr) * 2)));
    ctx->color_primaries = ist->codecpar->color_primaries;
    ctx->color_trc = ist->codecpar->color_trc;
    ctx->colorspace = ist->codecpar->color_space;
    ctx->color_range = ist->codecpar->color_range;
    if (out->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx.get(), c, nullptr) < 0) continue;  // not this machine's GPU
    enc = std::move(ctx);
    used = name;
    break;
  }
  if (!enc) return err(status::unsupported_format);

  AVStream* vost = avformat_new_stream(out.get(), nullptr);
  if (vost == nullptr || avcodec_parameters_from_context(vost->codecpar, enc.get()) < 0) {
    return err(status::out_of_memory);
  }
  vost->time_base = enc->time_base;
  if (const int rot = stream_rotation(ist); rot != 0 && !set_rotation(vost, rot)) return err(status::out_of_memory);
  av_dict_copy(&vost->metadata, ist->metadata, 0);
  av_dict_set(&vost->metadata, "encoder", nullptr, 0);

  // Audio is copied, packet-accurate at the edges: no software AAC encoder
  // anywhere (plan/11).
  std::vector<int> audio_map(s.format->nb_streams, -1);
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    AVStream* ast = s.format->streams[i];
    if (ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      if (static_cast<int>(i) != s.video) ast->discard = AVDISCARD_ALL;
      continue;
    }
    if (avformat_query_codec(out->oformat, ast->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0) {
      ast->discard = AVDISCARD_ALL;
      continue;
    }
    AVStream* aost = avformat_new_stream(out.get(), nullptr);
    if (aost == nullptr || avcodec_parameters_copy(aost->codecpar, ast->codecpar) < 0) {
      return err(status::out_of_memory);
    }
    aost->codecpar->codec_tag = 0;
    aost->time_base = ast->time_base;
    aost->disposition = ast->disposition;
    av_dict_copy(&aost->metadata, ast->metadata, 0);
    audio_map[i] = aost->index;
  }
  copy_metadata(s.format.get(), out.get());
  if (avformat_write_header(out.get(), nullptr) < 0) {
    return err(cancelled(ctl.cancel) ? status::cancelled : status::unsupported_format);
  }

  packet_ptr opkt(av_packet_alloc());
  frame_ptr conv(av_frame_alloc());
  if (!opkt || !conv) return err(status::out_of_memory);
  sws_ptr sws;
  bool video_done = false;
  std::vector<std::int64_t> audio_last(s.format->nb_streams, INT64_MIN);
  status failure = status::ok;
  const time_ns span = out_ns - in_ns;

  auto on_frame = [&](AVFrame* f, time_ns t) -> bool {
    if (t < in_ns - kTol) return true;  // decode-up from the keyframe
    if (t >= out_ns - kTol) {
      video_done = true;
      return false;
    }
    AVFrame* send = f;
    if (f->format != enc->pix_fmt || f->width != enc->width || f->height != enc->height) {
      sws = sws_ptr(sws_getCachedContext(sws.release(), f->width, f->height,
                                         static_cast<AVPixelFormat>(f->format), enc->width, enc->height,
                                         enc->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
      if (!sws) {
        failure = status::internal;
        return false;
      }
      av_frame_unref(conv.get());
      conv->format = enc->pix_fmt;
      conv->width = enc->width;
      conv->height = enc->height;
      if (av_frame_get_buffer(conv.get(), 0) < 0 ||
          sws_scale(sws.get(), f->data, f->linesize, 0, f->height, conv->data, conv->linesize) <= 0) {
        failure = status::out_of_memory;
        return false;
      }
      (void)av_frame_copy_props(conv.get(), f);
      send = conv.get();
    }
    send->pts = av_rescale_q(t - in_ns, kNs, enc->time_base);
    send->pict_type = AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(enc.get(), send) < 0) {
      failure = status::internal;
      return false;
    }
    if (auto d = drain_encoder(enc.get(), out.get(), vost, opkt.get()); !d) {
      failure = d.error();
      return false;
    }
    if (ctl.progress != nullptr) {
      ctl.progress(ctl.user, std::clamp(static_cast<double>(t - in_ns) / static_cast<double>(span), 0.0, 1.0));
    }
    return true;
  };
  auto on_other = [&](AVPacket* pkt) -> bool {
    const int idx = pkt->stream_index;
    if (idx < 0 || idx >= static_cast<int>(audio_map.size()) || audio_map[idx] < 0) return true;
    AVStream* ast = s.format->streams[idx];
    const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE) return true;
    const time_ns t = to_timeline(s, ast, ts);
    if (t < in_ns - kTol || t >= out_ns) return !video_done;
    const std::int64_t shift = from_timeline(s, ast, in_ns);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
    AVStream* aost = out->streams[audio_map[idx]];
    av_packet_rescale_ts(pkt, ast->time_base, aost->time_base);
    if (pkt->dts != AV_NOPTS_VALUE) {
      if (audio_last[idx] != INT64_MIN && pkt->dts <= audio_last[idx]) return true;
      audio_last[idx] = pkt->dts;
    }
    pkt->stream_index = audio_map[idx];
    pkt->pos = -1;
    if (av_interleaved_write_frame(out.get(), pkt) < 0) {
      failure = status::io;
      return false;
    }
    return true;
  };
  MV_TRY_VOID(decode_forward(s, s.video, dec.get(), in_ns, ctl, on_frame, on_other));
  if (failure != status::ok) return err(failure);
  // The tail of the audio that interleaves after the last video frame.
  if (video_done) {
    packet_ptr pkt(av_packet_alloc());
    while (pkt && av_read_frame(s.format.get(), pkt.get()) >= 0) {
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      const int idx = pkt->stream_index;
      bool more = true;
      if (idx != s.video && idx >= 0 && idx < static_cast<int>(audio_map.size()) && audio_map[idx] >= 0) {
        AVStream* ast = s.format->streams[idx];
        const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (ts != AV_NOPTS_VALUE && to_timeline(s, ast, ts) >= out_ns) more = false;
        else if (!on_other(pkt.get())) more = false;
      }
      av_packet_unref(pkt.get());
      if (!more || failure != status::ok) break;
    }
    if (failure != status::ok) return err(failure);
  }
  if (avcodec_send_frame(enc.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(drain_encoder(enc.get(), out.get(), vost, opkt.get()));
  if (av_write_trailer(out.get()) < 0) return err(cancelled(ctl.cancel) ? status::cancelled : status::io);
  return used;
}

// ---- frame -> PNG / JPEG ---------------------------------------------------------

result<std::vector<std::uint8_t>> frame_image(std::string_view src_path, time_ns at, frame_format fmt,
                                              int jpeg_quality, const control& ctl) {
  MV_TRY(source s, open_source(src_path, ctl.cancel));
  if (s.video < 0) return err(status::unsupported_format);
  AVStream* ist = s.format->streams[s.video];
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.video) s.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(ist));
  at = std::clamp<time_ns>(at, 0, std::max<time_ns>(0, s.duration_ns));
  // The frame on screen at `at`: the last one whose pts is not after it.
  frame_ptr held(av_frame_alloc());
  if (!held) return err(status::out_of_memory);
  bool have = false;
  auto on_frame = [&](AVFrame* f, time_ns t) -> bool {
    if (t > at + kTol && have) return false;
    av_frame_unref(held.get());
    if (av_frame_ref(held.get(), f) < 0) return false;
    have = true;
    return t <= at + kTol;
  };
  MV_TRY_VOID(decode_forward(s, s.video, dec.get(), at, ctl, on_frame,
                                  [](AVPacket*) { return true; }));
  if (!have) return err(status::corrupt);

  rgba_converter conv;
  codec::raster img;
  int w = held->width, h = held->height;
  if (!conv.convert(held.get(), ist->codecpar, w, h, img.rgba)) return err(status::internal);
  rotate_rgba(img.rgba, w, h, stream_rotation(ist));
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.tagged_srgb = true;
  encode_options opt;
  opt.format = fmt == frame_format::jpeg ? image_format::jpeg : image_format::png;
  opt.quality = std::clamp(jpeg_quality, 1, 100);
  return encode(img, opt, metadata_blobs{});
}

// ---- audio -> WAV / FLAC -----------------------------------------------------------

expected audio_transcode(std::string_view src_path, const std::string& out_path, audio_format fmt,
                         time_ns in_ns, time_ns out_ns, const control& ctl) {
  MV_TRY(source s, open_source(src_path, ctl.cancel));
  if (s.audio < 0) return err(status::unsupported_format);
  AVStream* ist = s.format->streams[s.audio];
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.audio) s.format->streams[i]->discard = AVDISCARD_ALL;
  }
  in_ns = std::max<time_ns>(0, in_ns);
  if (out_ns < 0 || out_ns > s.duration_ns) out_ns = s.duration_ns;
  if (out_ns <= in_ns) return err(status::invalid_arg);
  MV_TRY(codec_ptr dec, open_decoder(ist));

  const bool flac = fmt == audio_format::flac;
  const AVCodec* c = avcodec_find_encoder(flac ? AV_CODEC_ID_FLAC : AV_CODEC_ID_PCM_S16LE);
  if (c == nullptr) return err(status::unsupported_format);
  codec_ptr enc(avcodec_alloc_context3(c));
  if (!enc) return err(status::out_of_memory);
  const bool deep = flac && (dec->bits_per_raw_sample > 16 || av_get_bytes_per_sample(dec->sample_fmt) > 2);
  enc->sample_fmt = deep ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
  if (deep) enc->bits_per_raw_sample = 24;
  enc->sample_rate = dec->sample_rate;
  if (av_channel_layout_copy(&enc->ch_layout, &dec->ch_layout) < 0) return err(status::out_of_memory);
  if (enc->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    const int n = enc->ch_layout.nb_channels;
    av_channel_layout_uninit(&enc->ch_layout);
    av_channel_layout_default(&enc->ch_layout, n);
  }
  enc->time_base = AVRational{1, enc->sample_rate};
  MV_TRY(output_ptr out, open_output(flac ? "flac" : "wav", out_path, ctl.cancel));
  if (out->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (avcodec_open2(enc.get(), c, nullptr) < 0) return err(status::unsupported_format);
  AVStream* ost = avformat_new_stream(out.get(), nullptr);
  if (ost == nullptr || avcodec_parameters_from_context(ost->codecpar, enc.get()) < 0) {
    return err(status::out_of_memory);
  }
  ost->time_base = enc->time_base;
  copy_metadata(s.format.get(), out.get());
  AVDictionary* mux_opts = nullptr;
  if (!flac) av_dict_set(&mux_opts, "rf64", "auto", 0);  // past 4 GB, RF64 not a broken WAV
  const int hdr = avformat_write_header(out.get(), &mux_opts);
  av_dict_free(&mux_opts);
  if (hdr < 0) return err(status::unsupported_format);

  SwrContext* raw_swr = nullptr;
  if (swr_alloc_set_opts2(&raw_swr, &enc->ch_layout, enc->sample_fmt, enc->sample_rate, &dec->ch_layout,
                          dec->sample_fmt, dec->sample_rate, 0, nullptr) < 0) {
    return err(status::out_of_memory);
  }
  swr_ptr swr(raw_swr);
  if (swr_init(swr.get()) < 0) return err(status::unsupported_format);
  const int channels = enc->ch_layout.nb_channels;
  fifo_ptr fifo(av_audio_fifo_alloc(enc->sample_fmt, channels, 8192));
  frame_ptr ef(av_frame_alloc());
  packet_ptr pkt(av_packet_alloc());
  if (!fifo || !ef || !pkt) return err(status::out_of_memory);
  const int frame_size = enc->frame_size > 0 ? enc->frame_size : 4096;
  std::int64_t samples_out = 0;
  status failure = status::ok;
  std::vector<std::uint8_t> conv_buf;

  const auto send_from_fifo = [&](bool final) -> bool {
    while (av_audio_fifo_size(fifo.get()) >= frame_size || (final && av_audio_fifo_size(fifo.get()) > 0)) {
      const int n = std::min(frame_size, av_audio_fifo_size(fifo.get()));
      av_frame_unref(ef.get());
      ef->nb_samples = n;
      ef->format = enc->sample_fmt;
      ef->sample_rate = enc->sample_rate;
      if (av_channel_layout_copy(&ef->ch_layout, &enc->ch_layout) < 0 || av_frame_get_buffer(ef.get(), 0) < 0 ||
          av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(ef->data), n) != n) {
        failure = status::out_of_memory;
        return false;
      }
      ef->pts = samples_out;
      samples_out += n;
      if (avcodec_send_frame(enc.get(), ef.get()) < 0) {
        failure = status::internal;
        return false;
      }
      if (auto d = drain_encoder(enc.get(), out.get(), ost, pkt.get()); !d) {
        failure = d.error();
        return false;
      }
    }
    return true;
  };

  const double rate = dec->sample_rate;
  auto on_frame = [&](AVFrame* f, time_ns t) -> bool {
    const time_ns dur = static_cast<time_ns>(f->nb_samples * 1e9 / rate);
    if (t + dur <= in_ns) return true;
    if (t >= out_ns) return false;
    // Sample-accurate edges: drop the samples before `in` and after `out`.
    int skip = t < in_ns ? static_cast<int>(static_cast<double>(in_ns - t) * rate / 1e9) : 0;
    int keep = f->nb_samples - skip;
    if (t + dur > out_ns) keep = std::min(keep, static_cast<int>(static_cast<double>(out_ns - std::max(t, in_ns)) * rate / 1e9));
    if (keep <= 0) return true;
    const int max_out = swr_get_out_samples(swr.get(), f->nb_samples);
    std::uint8_t** planes = nullptr;
    int linesize = 0;
    if (av_samples_alloc_array_and_samples(&planes, &linesize, channels, max_out, enc->sample_fmt, 0) < 0) {
      failure = status::out_of_memory;
      return false;
    }
    const int got = swr_convert(swr.get(), planes, max_out, const_cast<const std::uint8_t**>(f->extended_data),
                                f->nb_samples);
    bool ok = got >= 0;
    if (ok) {
      // Resampling is 1:1 here (same rate), so skip/keep map directly.
      const int from = std::min(skip, got);
      const int n = std::min(keep, got - from);
      if (n > 0) {
        const int bps = av_get_bytes_per_sample(enc->sample_fmt) * channels;  // interleaved
        std::uint8_t* start = planes[0] + static_cast<std::ptrdiff_t>(from) * bps;
        ok = av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(&start), n) == n;
      }
    }
    av_freep(&planes[0]);
    av_freep(&planes);
    if (!ok) {
      failure = status::internal;
      return false;
    }
    if (ctl.progress != nullptr) {
      ctl.progress(ctl.user, std::clamp(static_cast<double>(t - in_ns) / static_cast<double>(out_ns - in_ns), 0.0, 1.0));
    }
    return send_from_fifo(false);
  };
  MV_TRY_VOID(decode_forward(s, s.audio, dec.get(), in_ns, ctl, on_frame, [](AVPacket*) { return true; }));
  if (failure != status::ok) return err(failure);
  if (!send_from_fifo(true)) return err(failure);
  if (avcodec_send_frame(enc.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(drain_encoder(enc.get(), out.get(), ost, pkt.get()));
  if (av_write_trailer(out.get()) < 0) return err(status::io);
  return {};
}

// ---- clip -> GIF / WebP ----------------------------------------------------------------

namespace {

// Walks [in, out) at `fps`, handing each tick's frame to `emit` as RGBA8 sRGB
// at the output size, rotated upright.
template <class Emit>
[[nodiscard]] expected sample_frames(std::string_view src_path, time_ns in_ns, time_ns out_ns,
                                     std::uint32_t fps, std::uint32_t long_edge, const control& ctl,
                                     double pbase, double pspan, int& out_w, int& out_h, Emit&& emit) {
  MV_TRY(source s, open_source(src_path, ctl.cancel));
  if (s.video < 0) return err(status::unsupported_format);
  AVStream* ist = s.format->streams[s.video];
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.video) s.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(ist));
  const int rot = stream_rotation(ist);
  int sw = ist->codecpar->width, sh = ist->codecpar->height;
  if (sw <= 0 || sh <= 0) return err(status::corrupt);
  const int disp_w = (rot == 90 || rot == 270) ? sh : sw;
  const int disp_h = (rot == 90 || rot == 270) ? sw : sh;
  const double scale = std::min(1.0, static_cast<double>(long_edge) / std::max(disp_w, disp_h));
  const int dw = std::max(2, static_cast<int>(std::lround(disp_w * scale / 2.0)) * 2);
  const int dh = std::max(2, static_cast<int>(std::lround(disp_h * scale / 2.0)) * 2);
  const int cw = (rot == 90 || rot == 270) ? dh : dw;  // decode-space size
  const int ch = (rot == 90 || rot == 270) ? dw : dh;
  out_w = dw;
  out_h = dh;

  const time_ns step = 1'000'000'000 / fps;
  time_ns tick = in_ns;
  std::int64_t index = 0;
  const time_ns span = out_ns - in_ns;
  rgba_converter conv;
  frame_ptr prev(av_frame_alloc());
  if (!prev) return err(status::out_of_memory);
  bool have = false;
  status failure = status::ok;
  std::vector<std::uint8_t> px;

  const auto emit_prev = [&]() -> bool {
    int w = cw, h = ch;
    if (!conv.convert(prev.get(), ist->codecpar, cw, ch, px)) {
      failure = status::internal;
      return false;
    }
    rotate_rgba(px, w, h, rot);
    if (!emit(px, index)) {
      failure = status::internal;
      return false;
    }
    ++index;
    tick += step;
    if (ctl.progress != nullptr) {
      ctl.progress(ctl.user, pbase + pspan * std::clamp(static_cast<double>(tick - in_ns) / static_cast<double>(span), 0.0, 1.0));
    }
    return true;
  };
  auto on_frame = [&](AVFrame* f, time_ns t) -> bool {
    // Emit the held frame for every tick it covers (t is where the next begins).
    while (have && tick < out_ns && t > tick + kTol) {
      if (!emit_prev()) return false;
    }
    if (tick >= out_ns) return false;
    av_frame_unref(prev.get());
    if (av_frame_ref(prev.get(), f) < 0) return false;
    have = true;
    return true;
  };
  MV_TRY_VOID(decode_forward(s, s.video, dec.get(), in_ns, ctl, on_frame, [](AVPacket*) { return true; }));
  if (failure != status::ok) return err(failure);
  while (have && tick < out_ns) {
    if (cancelled(ctl.cancel)) return err(status::cancelled);
    if (!emit_prev()) return err(failure);
  }
  if (index == 0) return err(status::corrupt);
  return {};
}

[[nodiscard]] AVFilterContext* make_filter(AVFilterGraph* g, const char* name, const char* label,
                                           const char* args) {
  AVFilterContext* f = nullptr;
  if (avfilter_graph_create_filter(&f, avfilter_get_by_name(name), label, args, nullptr, g) < 0) return nullptr;
  return f;
}

[[nodiscard]] std::string buffer_args(int w, int h, AVPixelFormat fmt, std::uint32_t fps) {
  char a[160];
  std::snprintf(a, sizeof(a), "video_size=%dx%d:pix_fmt=%d:time_base=1/%u:pixel_aspect=1/1", w, h,
                static_cast<int>(fmt), fps);
  return a;
}

[[nodiscard]] frame_ptr rgba_frame(const std::vector<std::uint8_t>& px, int w, int h, std::int64_t pts) {
  frame_ptr f(av_frame_alloc());
  if (!f) return f;
  f->format = AV_PIX_FMT_RGBA;
  f->width = w;
  f->height = h;
  if (av_frame_get_buffer(f.get(), 0) < 0) return nullptr;
  for (int y = 0; y < h; ++y) {
    std::memcpy(f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0],
                px.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
  }
  f->pts = pts;
  return f;
}

}  // namespace

expected animation(std::string_view src_path, const std::string& out_path, const request& req,
                   const control& ctl) {
  const std::uint32_t fps = std::clamp<std::uint32_t>(req.animation_fps, 1, 50);
  const std::uint32_t edge = std::clamp<std::uint32_t>(req.animation_width, 16, 1920);
  MV_TRY(source probe_src, open_source(src_path, ctl.cancel));
  const time_ns in_ns = std::max<time_ns>(0, req.in_ns);
  const time_ns out_ns = req.out_ns < 0 ? probe_src.duration_ns : std::min(req.out_ns, probe_src.duration_ns);
  probe_src.format.reset();
  if (out_ns <= in_ns) return err(status::invalid_arg);
  if (out_ns - in_ns > kMaxAnimationNs) return err(status::invalid_arg);

  const bool gif = req.animation == anim_format::gif;
  int w = 0, h = 0;
  packet_ptr pkt(av_packet_alloc());
  frame_ptr filtered(av_frame_alloc());
  if (!pkt || !filtered) return err(status::out_of_memory);

  // GIF pass 1: one palette for the whole clip (palettegen over every frame),
  // so the colours do not crawl from frame to frame.
  frame_ptr palette;
  if (gif) {
    graph_ptr g(avfilter_graph_alloc());
    if (!g) return err(status::out_of_memory);
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    bool built = false;
    auto feed = [&](const std::vector<std::uint8_t>& px, std::int64_t index) -> bool {
      if (!built) {
        src = make_filter(g.get(), "buffer", "in", buffer_args(w, h, AV_PIX_FMT_RGBA, fps).c_str());
        AVFilterContext* pg = make_filter(g.get(), "palettegen", "pg", "stats_mode=full");
        sink = make_filter(g.get(), "buffersink", "out", nullptr);
        if (!src || !pg || !sink || avfilter_link(src, 0, pg, 0) < 0 || avfilter_link(pg, 0, sink, 0) < 0 ||
            avfilter_graph_config(g.get(), nullptr) < 0) {
          return false;
        }
        built = true;
      }
      frame_ptr f = rgba_frame(px, w, h, index);
      return f && av_buffersrc_add_frame(src, f.get()) >= 0;
    };
    MV_TRY_VOID(sample_frames(src_path, in_ns, out_ns, fps, edge, ctl, 0.0, 0.5, w, h, feed));
    if (!built || av_buffersrc_add_frame(src, nullptr) < 0) return err(status::internal);
    palette.reset(av_frame_alloc());
    if (!palette || av_buffersink_get_frame(sink, palette.get()) < 0) return err(status::internal);
  }

  // Encoder + muxer.
  const AVCodec* c = gif ? avcodec_find_encoder(AV_CODEC_ID_GIF) : avcodec_find_encoder_by_name("libwebp_anim");
  if (c == nullptr) return err(status::unsupported_format);
  MV_TRY(output_ptr out, open_output(gif ? "gif" : "webp", out_path, ctl.cancel));
  codec_ptr enc;
  AVStream* ost = nullptr;
  graph_ptr g2;
  AVFilterContext* src2 = nullptr;
  AVFilterContext* sink2 = nullptr;
  sws_ptr sws;
  AVPixelFormat webp_fmt = AV_PIX_FMT_NONE;

  const auto open_encoder = [&](AVPixelFormat fmt) -> expected {
    enc.reset(avcodec_alloc_context3(c));
    if (!enc) return err(status::out_of_memory);
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = fmt;
    enc->time_base = AVRational{1, static_cast<int>(fps)};
    enc->framerate = AVRational{static_cast<int>(fps), 1};
    if (!gif) {
      (void)av_opt_set_int(enc->priv_data, "loop", 0, 0);
      enc->global_quality = 75 * FF_QP2LAMBDA;
      enc->flags |= AV_CODEC_FLAG_QSCALE;
    }
    if (avcodec_open2(enc.get(), c, nullptr) < 0) return err(status::unsupported_format);
    ost = avformat_new_stream(out.get(), nullptr);
    if (ost == nullptr || avcodec_parameters_from_context(ost->codecpar, enc.get()) < 0) {
      return err(status::out_of_memory);
    }
    ost->time_base = enc->time_base;
    if (avformat_write_header(out.get(), nullptr) < 0) return err(status::unsupported_format);
    return {};
  };

  status failure = status::ok;
  const auto encode_frame = [&](AVFrame* f) -> bool {
    if (avcodec_send_frame(enc.get(), f) < 0) {
      failure = status::internal;
      return false;
    }
    if (auto d = drain_encoder(enc.get(), out.get(), ost, pkt.get()); !d) {
      failure = d.error();
      return false;
    }
    return true;
  };
  const auto pull_gif = [&]() -> bool {
    while (true) {
      const int rc = av_buffersink_get_frame(sink2, filtered.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
      if (rc < 0) {
        failure = status::internal;
        return false;
      }
      const bool ok = encode_frame(filtered.get());
      av_frame_unref(filtered.get());
      if (!ok) return false;
    }
  };

  auto pass2 = [&](const std::vector<std::uint8_t>& px, std::int64_t index) -> bool {
    if (!enc) {
      if (gif) {
        g2.reset(avfilter_graph_alloc());
        src2 = make_filter(g2.get(), "buffer", "in", buffer_args(w, h, AV_PIX_FMT_RGBA, fps).c_str());
        AVFilterContext* pal = make_filter(
            g2.get(), "buffer", "pal",
            buffer_args(palette->width, palette->height, static_cast<AVPixelFormat>(palette->format), fps).c_str());
        AVFilterContext* use = make_filter(g2.get(), "paletteuse", "pu", "dither=sierra2_4a");
        sink2 = make_filter(g2.get(), "buffersink", "out", nullptr);
        if (!src2 || !pal || !use || !sink2 || avfilter_link(src2, 0, use, 0) < 0 ||
            avfilter_link(pal, 0, use, 1) < 0 || avfilter_link(use, 0, sink2, 0) < 0 ||
            avfilter_graph_config(g2.get(), nullptr) < 0) {
          return false;
        }
        palette->pts = 0;
        if (av_buffersrc_add_frame(pal, palette.get()) < 0 || av_buffersrc_add_frame(pal, nullptr) < 0) return false;
        if (!open_encoder(AV_PIX_FMT_PAL8)) return false;
      } else {
        const std::vector<AVPixelFormat> fmts = encoder_formats(c);
        webp_fmt = std::find(fmts.begin(), fmts.end(), AV_PIX_FMT_YUVA420P) != fmts.end() ? AV_PIX_FMT_YUVA420P
                   : fmts.empty()                                                           ? AV_PIX_FMT_YUV420P
                                                                                            : fmts.front();
        if (!open_encoder(webp_fmt)) return false;
      }
    }
    frame_ptr f = rgba_frame(px, w, h, index);
    if (!f) return false;
    if (gif) {
      if (av_buffersrc_add_frame(src2, f.get()) < 0) return false;
      return pull_gif();
    }
    sws = sws_ptr(sws_getCachedContext(sws.release(), w, h, AV_PIX_FMT_RGBA, w, h, webp_fmt, SWS_BICUBIC,
                                       nullptr, nullptr, nullptr));
    frame_ptr y(av_frame_alloc());
    if (!sws || !y) return false;
    y->format = webp_fmt;
    y->width = w;
    y->height = h;
    if (av_frame_get_buffer(y.get(), 0) < 0 ||
        sws_scale(sws.get(), f->data, f->linesize, 0, h, y->data, y->linesize) <= 0) {
      return false;
    }
    y->pts = index;
    return encode_frame(y.get());
  };
  MV_TRY_VOID(sample_frames(src_path, in_ns, out_ns, fps, edge, ctl, gif ? 0.5 : 0.0, gif ? 0.5 : 1.0,
                                 w, h, pass2));
  if (failure != status::ok) return err(failure);
  if (!enc) return err(status::internal);
  if (gif) {
    if (av_buffersrc_add_frame(src2, nullptr) < 0 || !pull_gif()) return err(status::internal);
  }
  if (avcodec_send_frame(enc.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(drain_encoder(enc.get(), out.get(), ost, pkt.get()));
  if (av_write_trailer(out.get()) < 0) return err(status::io);
  return {};
}

}  // namespace mv::edit::clip::detail
