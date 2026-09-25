// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic clips for the PR 13 / 14 tests, written in-process with FFmpeg's
// own MPEG-4 Part 2 and MP2 encoders: no corpus, no ffmpeg CLI, no x264, so
// the same cases run on Linux, Windows and macOS CI.
//
// Frame i has a bottom half of constant luma 16 + 4 * (i % 50), so a decoded
// frame says which source frame it is; the top half is seeded noise so the
// packets have a realistic size and output size is meaningfully proportional.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
}

namespace mv::test::clipfx {

struct spec {
  const char* muxer = "mp4";
  int width = 320;
  int height = 240;
  int fps = 30;
  int frames = 120;
  int gop = 15;
  int b_frames = 0;
  bool audio = true;
  int rotation = 0;  // clockwise
};

[[nodiscard]] inline int luma_of_frame(int i) { return 16 + 4 * (i % 50); }

namespace detail {

inline bool drain(AVCodecContext* enc, AVFormatContext* out, AVStream* st, AVPacket* pkt) {
  while (true) {
    const int rc = avcodec_receive_packet(enc, pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
    if (rc < 0) return false;
    // mpeg4 leaves duration 0; without it the MP4 muxer's edit list hides the
    // last frame (the product's drain_encoder does the same).
    if (pkt->duration <= 0 && enc->codec_type == AVMEDIA_TYPE_VIDEO && enc->framerate.num > 0) {
      pkt->duration = av_rescale_q(1, av_inv_q(enc->framerate), enc->time_base);
    }
    av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
    pkt->stream_index = st->index;
    if (av_interleaved_write_frame(out, pkt) < 0) return false;
  }
}

}  // namespace detail

// Writes a clip to `path`. False on any FFmpeg failure.
inline bool make(const std::string& path, const spec& s) {
  AVFormatContext* out = nullptr;
  if (avformat_alloc_output_context2(&out, nullptr, s.muxer, path.c_str()) < 0) return false;
  const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  AVCodecContext* venc = avcodec_alloc_context3(vc);
  venc->width = s.width;
  venc->height = s.height;
  venc->pix_fmt = AV_PIX_FMT_YUV420P;
  venc->time_base = AVRational{1, s.fps};
  venc->framerate = AVRational{s.fps, 1};
  venc->gop_size = s.gop;
  venc->max_b_frames = s.b_frames;
  venc->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_CLOSED_GOP;
  venc->global_quality = 2 * FF_QP2LAMBDA;
  av_opt_set_int(venc->priv_data, "sc_threshold", 1000000000, 0);  // closed GOPs on a fixed grid
  if (out->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (avcodec_open2(venc, vc, nullptr) < 0) return false;
  AVStream* vst = avformat_new_stream(out, nullptr);
  avcodec_parameters_from_context(vst->codecpar, venc);
  vst->time_base = venc->time_base;
  if (s.rotation != 0) {
    AVPacketSideData* sd = av_packet_side_data_new(&vst->codecpar->coded_side_data,
                                                   &vst->codecpar->nb_coded_side_data,
                                                   AV_PKT_DATA_DISPLAYMATRIX, 9 * 4, 0);
    av_display_rotation_set(reinterpret_cast<int32_t*>(sd->data), static_cast<double>(s.rotation));
  }

  AVCodecContext* aenc = nullptr;
  AVStream* ast = nullptr;
  if (s.audio) {
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
    aenc = avcodec_alloc_context3(ac);
    aenc->sample_rate = 48000;
    aenc->sample_fmt = AV_SAMPLE_FMT_S16;
    aenc->bit_rate = 192000;
    av_channel_layout_default(&aenc->ch_layout, 2);
    aenc->time_base = AVRational{1, 48000};
    if (out->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(aenc, ac, nullptr) < 0) return false;
    ast = avformat_new_stream(out, nullptr);
    avcodec_parameters_from_context(ast->codecpar, aenc);
    ast->time_base = aenc->time_base;
  }
  if (avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
  if (avformat_write_header(out, nullptr) < 0) return false;

  AVPacket* pkt = av_packet_alloc();
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_YUV420P;
  f->width = s.width;
  f->height = s.height;
  av_frame_get_buffer(f, 0);
  std::uint32_t seed = 12345;
  std::int64_t audio_samples = 0;
  const std::int64_t total_samples = static_cast<std::int64_t>(s.frames) * 48000 / s.fps;
  AVFrame* af = nullptr;
  if (aenc) {
    af = av_frame_alloc();
    af->format = AV_SAMPLE_FMT_S16;
    af->nb_samples = aenc->frame_size;
    af->sample_rate = 48000;
    av_channel_layout_copy(&af->ch_layout, &aenc->ch_layout);
    av_frame_get_buffer(af, 0);
  }
  bool ok = true;
  for (int i = 0; i < s.frames && ok; ++i) {
    av_frame_make_writable(f);
    for (int y = 0; y < s.height; ++y) {
      std::uint8_t* row = f->data[0] + y * f->linesize[0];
      if (y < s.height / 2) {
        for (int x = 0; x < s.width; ++x) {
          seed = seed * 1664525u + 1013904223u + static_cast<std::uint32_t>(i);
          row[x] = static_cast<std::uint8_t>(64 + (seed >> 26));
        }
      } else {
        std::memset(row, luma_of_frame(i), static_cast<std::size_t>(s.width));
      }
    }
    for (int p = 1; p < 3; ++p) {
      for (int y = 0; y < s.height / 2; ++y) std::memset(f->data[p] + y * f->linesize[p], 128, s.width / 2);
    }
    f->pts = i;
    ok = avcodec_send_frame(venc, f) >= 0 && detail::drain(venc, out, vst, pkt);
    // Audio up to this frame's end.
    const std::int64_t want = static_cast<std::int64_t>(i + 1) * 48000 / s.fps;
    while (ok && aenc && audio_samples + aenc->frame_size <= std::min(want + aenc->frame_size, total_samples)) {
      av_frame_make_writable(af);
      auto* d = reinterpret_cast<std::int16_t*>(af->data[0]);
      for (int n = 0; n < af->nb_samples; ++n) {
        const auto v = static_cast<std::int16_t>(8000 * std::sin(2 * 3.14159265 * 440 * static_cast<double>(audio_samples + n) / 48000.0));
        d[2 * n] = v;
        d[2 * n + 1] = v;
      }
      af->pts = audio_samples;
      audio_samples += af->nb_samples;
      ok = avcodec_send_frame(aenc, af) >= 0 && detail::drain(aenc, out, ast, pkt);
    }
  }
  ok = ok && avcodec_send_frame(venc, nullptr) >= 0 && detail::drain(venc, out, vst, pkt);
  if (aenc) ok = ok && avcodec_send_frame(aenc, nullptr) >= 0 && detail::drain(aenc, out, ast, pkt);
  ok = ok && av_write_trailer(out) >= 0;
  av_frame_free(&f);
  av_frame_free(&af);
  av_packet_free(&pkt);
  avcodec_free_context(&venc);
  avcodec_free_context(&aenc);
  avio_closep(&out->pb);
  avformat_free_context(out);
  return ok;
}

struct decoded {
  std::vector<int> luma;           // mean bottom-half luma per frame, in pts order
  std::vector<std::int64_t> pts_ns;
  int width = 0;
  int height = 0;
  std::int64_t duration_ns = 0;
  std::int64_t audio_packets = 0;
};

// Decodes every video frame of `path` (software) for the assertions.
inline bool decode_all(const std::string& path, decoded& d) {
  AVFormatContext* in = nullptr;
  if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) return false;
  if (avformat_find_stream_info(in, nullptr) < 0) return false;
  const int vs = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (in->duration != AV_NOPTS_VALUE) d.duration_ns = av_rescale(in->duration, 1'000'000'000, AV_TIME_BASE);
  AVCodecContext* dec = nullptr;
  if (vs >= 0) {
    const AVStream* st = in->streams[vs];
    dec = avcodec_alloc_context3(avcodec_find_decoder(st->codecpar->codec_id));
    avcodec_parameters_to_context(dec, st->codecpar);
    dec->pkt_timebase = st->time_base;
    if (avcodec_open2(dec, avcodec_find_decoder(st->codecpar->codec_id), nullptr) < 0) return false;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* f = av_frame_alloc();
  const std::int64_t origin = in->start_time != AV_NOPTS_VALUE ? av_rescale(in->start_time, 1'000'000'000, AV_TIME_BASE) : 0;
  const auto take = [&]() {
    while (avcodec_receive_frame(dec, f) >= 0) {
      std::int64_t sum = 0;
      const int y0 = f->height * 3 / 4;
      for (int y = y0; y < f->height; ++y) {
        const std::uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < f->width; ++x) sum += row[x];
      }
      d.luma.push_back(static_cast<int>(std::lround(static_cast<double>(sum) / ((f->height - y0) * f->width))));
      const std::int64_t ts = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
      d.pts_ns.push_back(av_rescale_q(ts, in->streams[vs]->time_base, AVRational{1, 1'000'000'000}) - origin);
      d.width = f->width;
      d.height = f->height;
    }
  };
  while (av_read_frame(in, pkt) >= 0) {
    if (pkt->stream_index == vs && dec) {
      avcodec_send_packet(dec, pkt);
      take();
    } else if (in->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      ++d.audio_packets;
    }
    av_packet_unref(pkt);
  }
  if (dec) {
    avcodec_send_packet(dec, nullptr);
    take();
  }
  av_frame_free(&f);
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  avformat_close_input(&in);
  return true;
}

// The video packets' payloads, in decode order: equal lists mean no re-encode.
inline std::vector<std::vector<std::uint8_t>> video_packets(const std::string& path) {
  std::vector<std::vector<std::uint8_t>> out;
  AVFormatContext* in = nullptr;
  if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) return out;
  avformat_find_stream_info(in, nullptr);
  const int vs = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  AVPacket* pkt = av_packet_alloc();
  while (av_read_frame(in, pkt) >= 0) {
    if (pkt->stream_index == vs) out.emplace_back(pkt->data, pkt->data + pkt->size);
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avformat_close_input(&in);
  return out;
}

}  // namespace mv::test::clipfx
