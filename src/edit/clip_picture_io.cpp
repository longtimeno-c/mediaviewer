// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// edit/picture_io.h: a clip's frames out, a new clip in.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>

#include "edit/clip_audio_enc.h"
#include "edit/clip_internal.h"
#include "edit/clip_pixels.h"
#include "edit/hwencode.h"
#include "edit/picture_io.h"
#include "io/file_port.h"

extern "C" {
#include <libavutil/imgutils.h>
}

namespace mv::edit::clip {

using namespace detail;

namespace {

[[nodiscard]] std::string stem_of(std::string_view path) {
  const std::string_view name = io::file_name_of(path);
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return std::string(name);
  return std::string(name.substr(0, dot));
}

bool suffix_ok(std::string_view s) noexcept {
  if (s.empty() || s.size() > 32) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

// docs/design/11: as Path 2 (clip_encode.cpp), never x264 / x265 / a software
// HEVC encoder; a software one only for the tests that ask.
bool licence_ok(const AVCodec* enc, bool allow_software) noexcept {
  if (std::strncmp(enc->name, "libx26", 6) == 0 || std::strcmp(enc->name, "libkvazaar") == 0) return false;
  if (enc->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) return true;
  return allow_software && enc->id != AV_CODEC_ID_HEVC;
}

std::vector<AVPixelFormat> encoder_formats(const AVCodec* enc) {
  std::vector<AVPixelFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* cfg = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg) {
    const auto* fmts = static_cast<const AVPixelFormat*>(cfg);
    for (int i = 0; i < n; ++i) {
      const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmts[i]);
      if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(fmts[i]);
    }
  }
#else
  for (const AVPixelFormat* f = enc->pix_fmts; f && *f != AV_PIX_FMT_NONE; ++f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*f);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(*f);
  }
#endif
  return out;
}

AVPixelFormat pick_format(const std::vector<AVPixelFormat>& fmts) {
  const auto has = [&](AVPixelFormat f) { return std::find(fmts.begin(), fmts.end(), f) != fmts.end(); };
  if (has(AV_PIX_FMT_NV12)) return AV_PIX_FMT_NV12;
  if (has(AV_PIX_FMT_YUV420P)) return AV_PIX_FMT_YUV420P;
  return fmts.empty() ? AV_PIX_FMT_YUV420P : fmts.front();
}

}  // namespace

// ---- frames out --------------------------------------------------------------------

struct frame_reader::impl {
  std::atomic<bool> never{false};
  const std::atomic<bool>* cancel = nullptr;
  source src;
  AVStream* st = nullptr;
  codec_ptr dec;
  packet_ptr pkt;
  frame_ptr frame;
  std::deque<frame_ptr> ready;  // decoded, not yet handed out
  rgba_converter conv;
  std::vector<std::uint8_t> rgba;
  frames_facts facts;
  bool eof = false;

  expected receive() {
    while (true) {
      const int rc = avcodec_receive_frame(dec.get(), frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return err(status::corrupt);
      frame_ptr held(av_frame_alloc());
      if (!held) return err(status::out_of_memory);
      av_frame_move_ref(held.get(), frame.get());
      ready.push_back(std::move(held));
    }
  }

  expected step() {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(src.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      (void)avcodec_send_packet(dec.get(), nullptr);
      eof = true;
      return receive();
    }
    if (pkt->stream_index == src.video) (void)avcodec_send_packet(dec.get(), pkt.get());
    av_packet_unref(pkt.get());
    return receive();
  }
};

frame_reader::frame_reader(std::unique_ptr<impl> p) : p_(std::move(p)) {}
frame_reader::~frame_reader() = default;
const frames_facts& frame_reader::facts() const noexcept { return p_->facts; }

result<std::unique_ptr<frame_reader>> frame_reader::open(std::string_view path, const std::atomic<bool>* cancel) {
  auto p = std::make_unique<impl>();
  p->cancel = cancel ? cancel : &p->never;
  MV_TRY(source s, open_source(path, p->cancel));
  if (s.video < 0) return err(status::unsupported_format);
  p->src = std::move(s);
  p->st = p->src.format->streams[p->src.video];
  for (unsigned i = 0; i < p->src.format->nb_streams; ++i) {
    if (static_cast<int>(i) != p->src.video) p->src.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(p->st, 0));
  p->dec = std::move(dec);
  p->pkt.reset(av_packet_alloc());
  p->frame.reset(av_frame_alloc());
  if (!p->pkt || !p->frame) return err(status::out_of_memory);
  const AVCodecParameters* cp = p->st->codecpar;
  frames_facts& f = p->facts;
  f.width = static_cast<std::uint32_t>(std::max(0, cp->width));
  f.height = static_cast<std::uint32_t>(std::max(0, cp->height));
  f.rotation = static_cast<std::uint32_t>(stream_rotation(p->st));
  f.hdr = cp->color_trc == AVCOL_TRC_SMPTE2084 || cp->color_trc == AVCOL_TRC_ARIB_STD_B67;
  const AVRational fr = p->st->avg_frame_rate.num > 0 ? p->st->avg_frame_rate : p->st->r_frame_rate;
  f.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
  f.duration_ms = p->src.duration_ns / 1'000'000;
  f.frame_count = p->st->nb_frames > 0 ? p->st->nb_frames
                                       : static_cast<std::int64_t>(std::llround(f.fps * static_cast<double>(f.duration_ms) / 1000.0));
  if (f.width == 0 || f.height == 0) return err(status::unsupported_format);
  return std::unique_ptr<frame_reader>(new frame_reader(std::move(p)));
}

result<bool> frame_reader::next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us) {
  impl& p = *p_;
  while (p.ready.empty() && !p.eof) MV_TRY_VOID(p.step());
  if (p.ready.empty()) return false;
  frame_ptr f = std::move(p.ready.front());
  p.ready.pop_front();
  const int w = static_cast<int>(p.facts.width), h = static_cast<int>(p.facts.height);
  if (!p.conv.convert(f.get(), p.st->codecpar, w, h, p.rgba)) return err(status::internal);
  const std::size_t px = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  rgb.resize(px * 3);
  for (std::size_t i = 0; i < px; ++i) {
    rgb[i * 3] = p.rgba[i * 4];
    rgb[i * 3 + 1] = p.rgba[i * 4 + 1];
    rgb[i * 3 + 2] = p.rgba[i * 4 + 2];
  }
  const std::int64_t ts = frame_ts(f.get());
  pts_us = ts == AV_NOPTS_VALUE ? 0 : to_timeline(p.src, p.st, ts) / 1000;
  return true;
}

// ---- a new clip in ------------------------------------------------------------------

struct picture_writer::impl {
  std::atomic<bool> never{false};
  const std::atomic<bool>* cancel = nullptr;
  picture_out_options opt;
  source src;
  staged_output staged;
  output_ptr out;
  codec_ptr enc;
  std::string encoder_name;
  int video_out = -1;
  std::vector<int> audio_map;  // source stream -> output stream, when its sound is copied
  bool copy_sound = false;
  bool src_eof = false;
  std::optional<audio_encoder> sound;
  int sound_out = -1;
  sws_ptr sws;
  frame_ptr yuv;
  packet_ptr pkt;
  packet_ptr apkt;
  std::int64_t last_pts = INT64_MIN;  // in the encoder's time base
  std::vector<std::int64_t> audio_last;

  // Copies the source's sound up to `until_ns` (all of it when < 0).
  expected copy_sound_until(time_ns until_ns) {
    if (!copy_sound || src_eof) return {};
    while (true) {
      if (cancelled(cancel)) return err(status::cancelled);
      if (!apkt->data) {
        const int rc = av_read_frame(src.format.get(), apkt.get());
        if (rc < 0) {
          if (rc == AVERROR_EXIT) return err(status::cancelled);
          src_eof = true;
          return {};
        }
        const int idx = apkt->stream_index;
        if (idx < 0 || idx >= static_cast<int>(audio_map.size()) || audio_map[idx] < 0) {
          av_packet_unref(apkt.get());
          continue;
        }
      }
      const int idx = apkt->stream_index;
      AVStream* ist = src.format->streams[idx];
      const std::int64_t ts = apkt->pts != AV_NOPTS_VALUE ? apkt->pts : apkt->dts;
      if (ts == AV_NOPTS_VALUE) {
        av_packet_unref(apkt.get());
        continue;
      }
      const time_ns t = to_timeline(src, ist, ts);
      if (until_ns >= 0 && t > until_ns) return {};
      AVStream* ost = out->streams[audio_map[idx]];
      const std::int64_t shift = from_timeline(src, ist, 0);
      if (apkt->pts != AV_NOPTS_VALUE) apkt->pts -= shift;
      if (apkt->dts != AV_NOPTS_VALUE) apkt->dts -= shift;
      av_packet_rescale_ts(apkt.get(), ist->time_base, ost->time_base);
      std::int64_t& last = audio_last[static_cast<std::size_t>(idx)];
      if (apkt->dts != AV_NOPTS_VALUE && last != INT64_MIN && apkt->dts <= last) {
        apkt->dts = last + 1;
        if (apkt->pts != AV_NOPTS_VALUE && apkt->pts < apkt->dts) apkt->pts = apkt->dts;
      }
      if (apkt->dts != AV_NOPTS_VALUE) last = apkt->dts;
      apkt->stream_index = audio_map[idx];
      apkt->pos = -1;
      const int wr = av_interleaved_write_frame(out.get(), apkt.get());
      av_packet_unref(apkt.get());
      if (wr < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
    }
  }

  expected drain_video() {
    AVStream* ost = out->streams[video_out];
    while (true) {
      const int rc = avcodec_receive_packet(enc.get(), pkt.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return err(status::internal);
      if (pkt->duration <= 0 && enc->framerate.num > 0) {
        pkt->duration = av_rescale_q(1, av_inv_q(enc->framerate), enc->time_base);
      }
      av_packet_rescale_ts(pkt.get(), enc->time_base, ost->time_base);
      pkt->stream_index = video_out;
      if (pkt->dts != AV_NOPTS_VALUE) {
        MV_TRY_VOID(copy_sound_until(av_rescale_q(pkt->dts, ost->time_base, kNs)));
      }
      const int wr = av_interleaved_write_frame(out.get(), pkt.get());
      av_packet_unref(pkt.get());
      if (wr < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
    }
  }

  expected write_sound_packet(AVPacket* p) {
    AVStream* ost = out->streams[sound_out];
    av_packet_rescale_ts(p, sound->context()->time_base, ost->time_base);
    p->stream_index = sound_out;
    const int wr = av_interleaved_write_frame(out.get(), p);
    return wr < 0 ? expected{err(cancelled(cancel) ? status::cancelled : status::io)} : expected{};
  }
};

picture_writer::picture_writer(std::unique_ptr<impl> p) : p_(std::move(p)) {}
picture_writer::~picture_writer() = default;  // the staged output discards itself
const char* picture_writer::encoder() const noexcept { return p_->encoder_name.c_str(); }

result<std::unique_ptr<picture_writer>> picture_writer::open(std::string_view source_utf8,
                                                             const picture_out_options& options,
                                                             const std::atomic<bool>* cancel) {
  if (options.width < 16 || options.height < 16 || options.width > 8192 || options.height > 8192 ||
      (options.width & 1) || (options.height & 1) || !suffix_ok(options.suffix) ||
      (options.sound_rate != 0 && (options.sound_channels < 1 || options.sound_channels > 8 ||
                                   options.sound_rate < 8000 || options.sound_rate > 192000))) {
    return err(status::invalid_arg);
  }
  auto p = std::make_unique<impl>();
  p->cancel = cancel ? cancel : &p->never;
  p->opt = options;
  MV_TRY(source s, open_source(source_utf8, p->cancel));
  if (s.video < 0) return err(status::unsupported_format);
  p->src = std::move(s);
  AVFormatContext* in = p->src.format.get();
  AVStream* ist = in->streams[p->src.video];

  if (options.sound_rate != 0) {
    MV_TRY(audio_encoder snd, audio_encoder::open(options.sound_rate, options.sound_channels, true));
    p->sound = std::move(snd);
  }
  const std::string family = family_of(p->src, source_utf8);
  const bool flac = p->sound && p->sound->lossless();
  const char* muxer = !flac && family == "mp4" ? "mp4" : !flac && family == "mov" ? "mov" : "matroska";
  const std::string ext = std::strcmp(muxer, "matroska") == 0 ? ".mkv" : extension_for_muxer(muxer);
  p->staged = staged_output(std::string(io::parent_of(source_utf8)),
                            stem_of(source_utf8) + "-" + options.suffix + ext);
  MV_TRY(output_ptr out, open_output(muxer, p->staged.temp_path(), p->cancel));
  p->out = std::move(out);

  // The encoder: the test list, else the hardware ones, HEVC first for an
  // HEVC source (as Path 2).
  std::vector<std::string> names = options.encoders;
  if (names.empty()) {
    const bool hevc = ist->codecpar->codec_id == AV_CODEC_ID_HEVC;
    for (const hwencode::codec c : {hwencode::codec::hevc, hwencode::codec::h264}) {
      if (c == hwencode::codec::hevc && !hevc) continue;
      for (const char* n : hwencode::candidates(c)) names.emplace_back(n);
    }
    if (!hevc) {
      for (const char* n : hwencode::candidates(hwencode::codec::hevc)) names.emplace_back(n);
    }
  }
  AVRational fr = ist->avg_frame_rate.num > 0 ? ist->avg_frame_rate : ist->r_frame_rate;
  if (fr.num <= 0 || fr.den <= 0) fr = AVRational{30, 1};
  // The source's rate, scaled with the picture's area (more pixels, more
  // bits), as Path 2 clamps it.
  std::int64_t bitrate = ist->codecpar->bit_rate;
  if (bitrate <= 0 && in->bit_rate > 0) bitrate = in->bit_rate * 9 / 10;
  if (bitrate <= 0) bitrate = 8'000'000;
  const double area = static_cast<double>(options.width) * options.height /
                      std::max(1.0, static_cast<double>(ist->codecpar->width) * ist->codecpar->height);
  bitrate = std::clamp<std::int64_t>(static_cast<std::int64_t>(static_cast<double>(bitrate) * area * 1.1),
                                     1'000'000, 200'000'000);
  for (const std::string& name : names) {
    const AVCodec* c = avcodec_find_encoder_by_name(name.c_str());
    if (!c || c->type != AVMEDIA_TYPE_VIDEO || !licence_ok(c, options.allow_software)) continue;
    if (avformat_query_codec(p->out->oformat, c->id, FF_COMPLIANCE_NORMAL) == 0) continue;
    codec_ptr ctx(avcodec_alloc_context3(c));
    if (!ctx) return err(status::out_of_memory);
    ctx->width = static_cast<int>(options.width);
    ctx->height = static_cast<int>(options.height);
    ctx->sample_aspect_ratio = ist->codecpar->sample_aspect_ratio;
    ctx->pix_fmt = pick_format(encoder_formats(c));
    ctx->time_base = ist->time_base;
    if (ctx->time_base.den > 65535 && c->id == AV_CODEC_ID_MPEG4) ctx->time_base = av_inv_q(fr);
    ctx->framerate = fr;
    ctx->bit_rate = bitrate;
    ctx->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(fr) * 2)));
    // The frames are sRGB-encoded BT.709 (clip_pixels.h): tagged so.
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (p->out->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx.get(), c, nullptr) < 0) continue;  // not this machine's GPU, or not at this size
    p->enc = std::move(ctx);
    p->encoder_name = name;
    break;
  }
  if (!p->enc) return err(status::unsupported_format);

  AVStream* vost = avformat_new_stream(p->out.get(), nullptr);
  if (!vost || avcodec_parameters_from_context(vost->codecpar, p->enc.get()) < 0) return err(status::out_of_memory);
  vost->time_base = p->enc->time_base;
  if (const int rot = stream_rotation(ist); rot != 0 && !set_rotation(vost, rot)) return err(status::out_of_memory);
  av_dict_copy(&vost->metadata, ist->metadata, 0);
  av_dict_set(&vost->metadata, "encoder", nullptr, 0);
  p->video_out = vost->index;

  p->audio_map.assign(in->nb_streams, -1);
  p->audio_last.assign(in->nb_streams, INT64_MIN);
  if (p->sound) {
    AVStream* ast = avformat_new_stream(p->out.get(), nullptr);
    if (!ast || avcodec_parameters_from_context(ast->codecpar, p->sound->context()) < 0) {
      return err(status::out_of_memory);
    }
    ast->time_base = p->sound->context()->time_base;
    p->sound_out = ast->index;
  }
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    AVStream* st = in->streams[i];
    const bool keep = !p->sound && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                      avformat_query_codec(p->out->oformat, st->codecpar->codec_id, FF_COMPLIANCE_NORMAL) != 0;
    if (!keep) {
      st->discard = AVDISCARD_ALL;
      continue;
    }
    AVStream* aost = avformat_new_stream(p->out.get(), nullptr);
    if (!aost || avcodec_parameters_copy(aost->codecpar, st->codecpar) < 0) return err(status::out_of_memory);
    if (std::strcmp(muxer, "matroska") == 0) aost->codecpar->codec_tag = 0;
    aost->time_base = st->time_base;
    aost->disposition = st->disposition;
    av_dict_copy(&aost->metadata, st->metadata, 0);
    p->audio_map[i] = aost->index;
    p->copy_sound = true;
  }
  copy_metadata(in, p->out.get());
  if (avformat_write_header(p->out.get(), nullptr) < 0) {
    return err(cancelled(p->cancel) ? status::cancelled : status::unsupported_format);
  }

  p->yuv.reset(av_frame_alloc());
  p->pkt.reset(av_packet_alloc());
  p->apkt.reset(av_packet_alloc());
  if (!p->yuv || !p->pkt || !p->apkt) return err(status::out_of_memory);
  p->yuv->format = p->enc->pix_fmt;
  p->yuv->width = p->enc->width;
  p->yuv->height = p->enc->height;
  if (av_frame_get_buffer(p->yuv.get(), 0) < 0) return err(status::out_of_memory);
  p->sws = sws_ptr(sws_getContext(p->enc->width, p->enc->height, AV_PIX_FMT_RGB24, p->enc->width, p->enc->height,
                                  p->enc->pix_fmt, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr));
  if (!p->sws) return err(status::internal);
  // RGB in, BT.709 limited range out (as tagged above).
  (void)sws_setColorspaceDetails(p->sws.get(), sws_getCoefficients(SWS_CS_ITU709), 1,
                                 sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
  return std::unique_ptr<picture_writer>(new picture_writer(std::move(p)));
}

expected picture_writer::write_frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us) {
  impl& p = *p_;
  if (!p.out || !rgb || stride < p.opt.width * 3) return err(status::invalid_arg);
  if (cancelled(p.cancel)) return err(status::cancelled);
  if (av_frame_make_writable(p.yuv.get()) < 0) return err(status::out_of_memory);
  const std::uint8_t* src[4] = {rgb, nullptr, nullptr, nullptr};
  const int src_stride[4] = {static_cast<int>(stride), 0, 0, 0};
  if (sws_scale(p.sws.get(), src, src_stride, 0, static_cast<int>(p.opt.height), p.yuv->data, p.yuv->linesize) <= 0) {
    return err(status::internal);
  }
  std::int64_t pts = av_rescale_q(pts_us, AVRational{1, 1'000'000}, p.enc->time_base);
  if (p.last_pts != INT64_MIN && pts <= p.last_pts) pts = p.last_pts + 1;  // a repeated time from the source
  p.last_pts = pts;
  p.yuv->pts = pts;
  p.yuv->pict_type = AV_PICTURE_TYPE_NONE;
  if (avcodec_send_frame(p.enc.get(), p.yuv.get()) < 0) return err(status::internal);
  return p.drain_video();
}

expected picture_writer::write_sound(std::span<const float> interleaved) {
  impl& p = *p_;
  if (!p.out || !p.sound || interleaved.size() % p.opt.sound_channels != 0) return err(status::invalid_arg);
  if (cancelled(p.cancel)) return err(status::cancelled);
  return p.sound->push(interleaved, [&p](AVPacket* pkt) { return p.write_sound_packet(pkt); });
}

result<std::string> picture_writer::finish() {
  impl& p = *p_;
  if (!p.out) return err(status::invalid_arg);
  if (avcodec_send_frame(p.enc.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(p.drain_video());
  if (p.sound) MV_TRY_VOID(p.sound->flush([&p](AVPacket* pkt) { return p.write_sound_packet(pkt); }));
  MV_TRY_VOID(p.copy_sound_until(-1));
  if (av_write_trailer(p.out.get()) < 0) return err(cancelled(p.cancel) ? status::cancelled : status::io);
  p.out.reset();  // closes the file before it is renamed
  return p.staged.publish();
}

}  // namespace mv::edit::clip
