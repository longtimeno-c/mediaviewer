// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// edit/sound_writer.h: new sound in a new file, the source's picture copied.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "edit/clip_audio_enc.h"
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
  audio_encoder enc;
  int audio_out = -1;
  int video_out = -1;
  packet_ptr vpkt;
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

  // Each packet the encoder has ready, with the picture up to its time first.
  expected write_sound(AVPacket* pkt) {
    AVStream* ost = out->streams[audio_out];
    av_packet_rescale_ts(pkt, enc.context()->time_base, ost->time_base);
    pkt->stream_index = audio_out;
    if (picture && pkt->pts != AV_NOPTS_VALUE) {
      MV_TRY_VOID(copy_picture(av_rescale_q(pkt->pts, ost->time_base, kNs)));
    }
    const int wr = av_interleaved_write_frame(out.get(), pkt);
    if (wr < 0) return err(cancelled(cancel) ? status::cancelled : status::io);
    return {};
  }
};

sound_writer::sound_writer(std::unique_ptr<impl> p) : p_(std::move(p)) {}
sound_writer::~sound_writer() = default;  // the staged output discards itself
bool sound_writer::with_picture() const noexcept { return p_->picture; }
const char* sound_writer::encoder() const noexcept { return p_->enc.name(); }

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

  // The encoder first: it decides the box. AAC (the OS's) in M4A, or in the
  // clip's own MP4 / MOV; FLAC (where the OS has no AAC encoder) in a .flac,
  // or a clip in Matroska.
  MV_TRY(audio_encoder enc, audio_encoder::open(options.sample_rate, options.channels, true));
  const char* muxer = enc.lossless() ? "flac" : "ipod";  // ipod: MP4 with the M4A brand
  std::string ext = enc.lossless() ? ".flac" : ".m4a";
  if (p->picture) {
    const std::string family = family_of(p->src, source_utf8);
    muxer = enc.lossless() ? "matroska" : family == "mp4" ? "mp4" : family == "mov" ? "mov" : "matroska";
    ext = std::strcmp(muxer, "matroska") == 0 ? ".mkv" : extension_for_muxer(muxer);
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

  AVStream* ast = avformat_new_stream(p->out.get(), nullptr);
  if (!ast || avcodec_parameters_from_context(ast->codecpar, enc.context()) < 0) return err(status::out_of_memory);
  ast->time_base = enc.context()->time_base;
  p->audio_out = ast->index;
  p->enc = std::move(enc);
  copy_metadata(in, p->out.get());
  if (avformat_write_header(p->out.get(), nullptr) < 0) {
    return err(cancelled(p->cancel) ? status::cancelled : status::unsupported_format);
  }
  p->vpkt.reset(av_packet_alloc());
  if (!p->vpkt) return err(status::out_of_memory);
  return std::unique_ptr<sound_writer>(new sound_writer(std::move(p)));
}

expected sound_writer::write(std::span<const float> interleaved) {
  impl& p = *p_;
  if (!p.out) return err(status::invalid_arg);
  if (interleaved.size() % p.opt.channels != 0) return err(status::invalid_arg);
  if (cancelled(p.cancel)) return err(status::cancelled);
  return p.enc.push(interleaved, [&p](AVPacket* pkt) { return p.write_sound(pkt); });
}

result<std::string> sound_writer::finish() {
  impl& p = *p_;
  if (!p.out) return err(status::invalid_arg);
  MV_TRY_VOID(p.enc.flush([&p](AVPacket* pkt) { return p.write_sound(pkt); }));
  MV_TRY_VOID(p.copy_picture(-1));
  if (av_write_trailer(p.out.get()) < 0) return err(cancelled(p.cancel) ? status::cancelled : status::io);
  p.out.reset();  // closes the file before it is renamed
  return p.staged.publish();
}

}  // namespace mv::edit::clip
