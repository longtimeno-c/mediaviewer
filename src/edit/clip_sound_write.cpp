// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// edit/sound_writer.h: new sound in a new file, the source's picture copied.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "edit/clip_internal.h"
#include "edit/sound_writer.h"
#include "io/file_port.h"

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

}  // namespace

struct sound_writer::impl {
  std::atomic<bool> never{false};
  const std::atomic<bool>* cancel = nullptr;
  sound_out_options opt;
  source src;  // only its picture stream is read, and only for a clip
  bool picture = false;
  staged_output staged;
  output_ptr out;
  codec_ptr enc;
  int audio_out = -1;
  int video_out = -1;
  frame_ptr frame;
  packet_ptr pkt;
  packet_ptr vpkt;
  std::vector<float> fifo;    // interleaved, not yet a whole encoder frame
  std::int64_t next_pts = 0;  // in samples
  std::int64_t video_shift = 0;
  bool video_eof = false;
  std::int64_t video_last_dts = INT64_MIN;

  // Copies picture packets up to `until_ns` on the output timeline (all of
  // them when < 0), so the muxer never holds more than about one audio frame
  // of either stream.
  expected copy_picture(time_ns until_ns) {
    if (!picture || video_eof) return {};
    AVStream* ist = src.format->streams[src.video];
    AVStream* ost = out->streams[video_out];
    while (true) {
      if (cancelled(cancel)) return err(status::cancelled);
      if (!vpkt->data) {
        const int rc = av_read_frame(src.format.get(), vpkt.get());
        if (rc < 0) {
          if (rc == AVERROR_EXIT) return err(status::cancelled);
          video_eof = true;
          return {};
        }
        if (vpkt->stream_index != src.video) {
          av_packet_unref(vpkt.get());
          continue;
        }
      }
      const std::int64_t ts = vpkt->dts != AV_NOPTS_VALUE ? vpkt->dts : vpkt->pts;
      if (until_ns >= 0 && ts != AV_NOPTS_VALUE && to_timeline(src, ist, ts) > until_ns) return {};
      if (vpkt->pts != AV_NOPTS_VALUE) vpkt->pts -= video_shift;
      if (vpkt->dts != AV_NOPTS_VALUE) vpkt->dts -= video_shift;
      av_packet_rescale_ts(vpkt.get(), ist->time_base, ost->time_base);
      if (vpkt->dts != AV_NOPTS_VALUE && video_last_dts != INT64_MIN && vpkt->dts <= video_last_dts) {
        vpkt->dts = video_last_dts + 1;
        if (vpkt->pts != AV_NOPTS_VALUE && vpkt->pts < vpkt->dts) vpkt->pts = vpkt->dts;
      }
      if (vpkt->dts != AV_NOPTS_VALUE) video_last_dts = vpkt->dts;
      vpkt->stream_index = video_out;
      vpkt->pos = -1;
      const int wr = av_interleaved_write_frame(out.get(), vpkt.get());  // takes the packet's data
      av_packet_unref(vpkt.get());
      if (wr < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
    }
  }

  expected drain_encoder() {
    while (true) {
      const int rc = avcodec_receive_packet(enc.get(), pkt.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return err(status::internal);
      AVStream* ost = out->streams[audio_out];
      av_packet_rescale_ts(pkt.get(), enc->time_base, ost->time_base);
      pkt->stream_index = audio_out;
      if (picture && pkt->pts != AV_NOPTS_VALUE) {
        MV_TRY_VOID(copy_picture(av_rescale_q(pkt->pts, ost->time_base, kNs)));
      }
      const int wr = av_interleaved_write_frame(out.get(), pkt.get());
      av_packet_unref(pkt.get());
      if (wr < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
    }
  }

  // One encoder frame from the front of the fifo (planar float, as FFmpeg's
  // AAC encoder takes it); a short last frame at the end.
  expected encode_front(int samples) {
    frame->nb_samples = samples;
    if (av_frame_make_writable(frame.get()) < 0) return err(status::out_of_memory);
    const int ch = static_cast<int>(opt.channels);
    for (int c = 0; c < ch; ++c) {
      auto* plane = reinterpret_cast<float*>(frame->extended_data[c]);
      for (int i = 0; i < samples; ++i) {
        const float v = fifo[static_cast<std::size_t>(i * ch + c)];
        plane[i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
      }
    }
    frame->pts = next_pts;
    next_pts += samples;
    fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(samples) * ch);
    if (avcodec_send_frame(enc.get(), frame.get()) < 0) return err(status::internal);
    return drain_encoder();
  }
};

sound_writer::sound_writer(std::unique_ptr<impl> p) : p_(std::move(p)) {}
sound_writer::~sound_writer() = default;  // the staged output discards itself
bool sound_writer::with_picture() const noexcept { return p_->picture; }

result<std::unique_ptr<sound_writer>> sound_writer::open(std::string_view source_utf8,
                                                         const sound_out_options& options,
                                                         const std::atomic<bool>* cancel) {
  if (options.channels < 1 || options.channels > 8 || options.sample_rate < 8000 ||
      options.sample_rate > 192000 || !suffix_ok(options.suffix)) {
    return err(status::invalid_arg);
  }
  auto p = std::make_unique<impl>();
  p->cancel = cancel ? cancel : &p->never;
  p->opt = options;
  MV_TRY(source s, open_source(source_utf8, p->cancel));
  p->src = std::move(s);
  p->picture = p->src.video >= 0;
  AVFormatContext* in = p->src.format.get();
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (static_cast<int>(i) != p->src.video) in->streams[i]->discard = AVDISCARD_ALL;
  }

  const char* muxer = "ipod";  // MP4 with the M4A brand
  std::string ext = ".m4a";
  if (p->picture) {
    const std::string family = family_of(p->src, source_utf8);
    muxer = family == "mp4" ? "mp4" : family == "mov" ? "mov" : "matroska";
    ext = extension_for_muxer(muxer);
    if (std::strcmp(muxer, "matroska") == 0) ext = ".mkv";
  }
  p->staged = staged_output(std::string(io::parent_of(source_utf8)),
                            stem_of(source_utf8) + "-" + options.suffix + ext);
  MV_TRY(output_ptr out, open_output(muxer, p->staged.temp_path(), p->cancel));
  p->out = std::move(out);

  if (p->picture) {
    AVStream* ist = in->streams[p->src.video];
    if (avformat_query_codec(p->out->oformat, ist->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0) {
      return err(status::unsupported_format);
    }
    AVStream* ost = avformat_new_stream(p->out.get(), nullptr);
    if (!ost || avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) return err(status::out_of_memory);
    // The tag belongs to its container: kept within the family (an iPhone
    // HEVC's hvc1 stays playable in QuickTime), dropped across.
    if (std::strcmp(muxer, "matroska") == 0) ost->codecpar->codec_tag = 0;
    ost->time_base = ist->time_base;
    ost->disposition = ist->disposition;
    av_dict_copy(&ost->metadata, ist->metadata, 0);
    p->video_out = ost->index;
    // The picture starts where the sound does: at 0 on the output.
    p->video_shift = from_timeline(p->src, ist, 0);
  }

  const AVCodec* codec = avcodec_find_encoder_by_name("aac");
  if (!codec) return err(status::unsupported_format);
  codec_ptr enc(avcodec_alloc_context3(codec));
  if (!enc) return err(status::out_of_memory);
  enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
  enc->sample_rate = static_cast<int>(options.sample_rate);
  av_channel_layout_default(&enc->ch_layout, static_cast<int>(options.channels));
  // About 96 kb/s a channel: transparent for speech and music at 48 kHz.
  enc->bit_rate = std::min<std::int64_t>(96'000 * options.channels, 512'000);
  enc->time_base = AVRational{1, static_cast<int>(options.sample_rate)};
  if (p->out->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (avcodec_open2(enc.get(), codec, nullptr) < 0) return err(status::unsupported_format);
  AVStream* ast = avformat_new_stream(p->out.get(), nullptr);
  if (!ast || avcodec_parameters_from_context(ast->codecpar, enc.get()) < 0) return err(status::out_of_memory);
  ast->time_base = enc->time_base;
  p->audio_out = ast->index;
  p->enc = std::move(enc);
  copy_metadata(in, p->out.get());
  if (avformat_write_header(p->out.get(), nullptr) < 0) {
    return err(cancelled(p->cancel) ? status::cancelled : status::unsupported_format);
  }

  p->frame.reset(av_frame_alloc());
  p->pkt.reset(av_packet_alloc());
  p->vpkt.reset(av_packet_alloc());
  if (!p->frame || !p->pkt || !p->vpkt) return err(status::out_of_memory);
  p->frame->format = AV_SAMPLE_FMT_FLTP;
  p->frame->sample_rate = p->enc->sample_rate;
  if (av_channel_layout_copy(&p->frame->ch_layout, &p->enc->ch_layout) < 0) return err(status::out_of_memory);
  p->frame->nb_samples = p->enc->frame_size > 0 ? p->enc->frame_size : 1024;
  if (av_frame_get_buffer(p->frame.get(), 0) < 0) return err(status::out_of_memory);
  return std::unique_ptr<sound_writer>(new sound_writer(std::move(p)));
}

expected sound_writer::write(std::span<const float> interleaved) {
  impl& p = *p_;
  if (!p.out) return err(status::invalid_arg);
  if (interleaved.size() % p.opt.channels != 0) return err(status::invalid_arg);
  if (cancelled(p.cancel)) return err(status::cancelled);
  p.fifo.insert(p.fifo.end(), interleaved.begin(), interleaved.end());
  const int frame = p.enc->frame_size > 0 ? p.enc->frame_size : 1024;
  const std::size_t whole = static_cast<std::size_t>(frame) * p.opt.channels;
  while (p.fifo.size() >= whole) MV_TRY_VOID(p.encode_front(frame));
  return {};
}

result<std::string> sound_writer::finish() {
  impl& p = *p_;
  if (!p.out) return err(status::invalid_arg);
  if (!p.fifo.empty()) MV_TRY_VOID(p.encode_front(static_cast<int>(p.fifo.size() / p.opt.channels)));
  if (avcodec_send_frame(p.enc.get(), nullptr) < 0) return err(status::internal);
  MV_TRY_VOID(p.drain_encoder());
  MV_TRY_VOID(p.copy_picture(-1));
  if (av_write_trailer(p.out.get()) < 0) return err(cancelled(p.cancel) ? status::cancelled : status::io);
  p.out.reset();  // closes the file before it is renamed
  return p.staged.publish();
}

}  // namespace mv::edit::clip
