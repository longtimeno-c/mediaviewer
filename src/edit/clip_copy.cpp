// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Stream copy: Path 1 keyframe trim, lossless rotate, split, remove-middle,
// remux and audio copy. No decoder is ever opened here, so the output is the
// source's own packets, re-timed (plan/08 "Keyframe trim").
#include <algorithm>
#include <cstring>

#include "edit/clip_internal.h"

namespace mv::edit::clip::detail {
namespace {

// Half a millisecond: keyframe times are read back from the same timestamps,
// so this only absorbs ns <-> timebase rounding. A 240 fps frame is 4 ms.
constexpr time_ns kTol = 500'000;

struct out_stream {
  int index = -1;
  std::int64_t last_dts = INT64_MIN;
};

}  // namespace

expected copy_segments(std::string_view src_path, const std::string& out_path,
                       const copy_spec& spec, const control& ctl, double progress_base,
                       double progress_span) {
  if (spec.segments.empty() || spec.muxer == nullptr) return err(status::invalid_arg);
  MV_TRY(source s, open_source(src_path, ctl.cancel));
  AVFormatContext* in = s.format.get();
  MV_TRY(output_ptr out, open_output(spec.muxer, out_path, ctl.cancel));

  const bool same_family =
      std::strcmp(spec.muxer, muxer_for_family(family_of(s, src_path))) == 0;

  std::vector<out_stream> map(in->nb_streams);
  int picked = 0;
  bool video_picked = false;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    AVStream* ist = in->streams[i];
    const AVMediaType type = ist->codecpar->codec_type;
    const int idx = static_cast<int>(i);
    bool want = false;
    if (spec.pick == stream_pick::audio_only) {
      want = idx == s.audio;
    } else {
      // The main picture and every audio track. Timecode, chapter-text and
      // maker data tracks do not survive a change of container, and a second
      // video track (a phone's depth map) is not what "trim" means.
      want = idx == s.video || type == AVMEDIA_TYPE_AUDIO;
    }
    if (!want) {
      ist->discard = AVDISCARD_ALL;
      continue;
    }
    const int ok = avformat_query_codec(out->oformat, ist->codecpar->codec_id, FF_COMPLIANCE_NORMAL);
    if (ok == 0) {
      // The target cannot hold this codec: fatal for the picture or the one
      // track asked for, otherwise that track is left out.
      if (idx == s.video || spec.pick == stream_pick::audio_only) return err(status::unsupported_format);
      ist->discard = AVDISCARD_ALL;
      continue;
    }
    AVStream* ost = avformat_new_stream(out.get(), nullptr);
    if (ost == nullptr) return err(status::out_of_memory);
    if (avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) return err(status::out_of_memory);
    // A codec tag belongs to its container (hvc1 in MP4 means nothing to
    // Matroska); keep it only when the container family is unchanged, which
    // is what keeps an iPhone HEVC's hvc1 playable in QuickTime.
    if (!same_family) ost->codecpar->codec_tag = 0;
    ost->time_base = ist->time_base;
    ost->disposition = ist->disposition;
    av_dict_copy(&ost->metadata, ist->metadata, 0);
    if (idx == s.video) {
      video_picked = true;
      if (spec.rotation >= 0 && !set_rotation(ost, spec.rotation)) return err(status::out_of_memory);
    }
    map[i].index = ost->index;
    ++picked;
  }
  if (picked == 0) return err(status::unsupported_format);
  copy_metadata(in, out.get());

  if (avformat_write_header(out.get(), nullptr) < 0) {
    return err(cancelled(ctl.cancel) ? status::cancelled : status::unsupported_format);
  }

  const time_ns duration = s.duration_ns;
  time_ns total = 0;
  for (const segment& g : spec.segments) {
    const time_ns end = g.end_ns < 0 ? duration : std::min(g.end_ns, duration);
    total += std::max<time_ns>(0, end - g.start_ns);
  }
  if (total <= 0) return err(status::invalid_arg);

  packet_ptr pkt(av_packet_alloc());
  if (!pkt) return err(status::out_of_memory);

  time_ns written_ns = 0;  // output timeline where this segment starts
  for (const segment& g : spec.segments) {
    const time_ns start = g.start_ns;
    const bool to_eof = g.end_ns < 0 || g.end_ns >= duration;
    const time_ns end = to_eof ? duration : g.end_ns;
    seek_before(s, start);

    bool video_started = !video_picked;
    bool video_done = !video_picked;
    std::vector<bool> audio_done(in->nb_streams, false);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      audio_done[i] = map[i].index < 0 || static_cast<int>(i) == s.video;
    }

    while (true) {
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      const int rc = av_read_frame(in, pkt.get());
      if (rc < 0) {
        if (rc == AVERROR_EXIT) return err(status::cancelled);
        break;  // EOF, or a truncated tail: keep what was whole
      }
      const int idx = pkt->stream_index;
      const out_stream* os = idx >= 0 && idx < static_cast<int>(map.size()) ? &map[idx] : nullptr;
      if (os == nullptr || os->index < 0) {
        av_packet_unref(pkt.get());
        continue;
      }
      AVStream* ist = in->streams[idx];
      const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
      if (ts == AV_NOPTS_VALUE) {
        av_packet_unref(pkt.get());
        continue;
      }
      const time_ns t = to_timeline(s, ist, ts);
      bool keep = false;
      if (idx == s.video) {
        const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (!video_started) {
          video_started = key && t >= start - kTol;
          keep = video_started;
        } else if (!video_done) {
          if (!to_eof && key && t >= end - kTol) {
            video_done = true;  // the out keyframe itself belongs to what follows
          } else {
            // Leading pictures of an open GOP (pts before the in keyframe)
            // reference the GOP before it, which is not in the output.
            keep = t >= start - kTol;
          }
        }
      } else {
        if (t >= end - kTol && !to_eof) {
          audio_done[idx] = true;
        } else {
          // From the very start nothing is cut: an audio priming packet with a
          // negative pts (an MP4 edit list) belongs to the first frame.
          keep = start <= 0 || t >= start - kTol;
        }
      }
      if (!keep) {
        av_packet_unref(pkt.get());
        if (video_done && std::all_of(audio_done.begin(), audio_done.end(), [](bool b) { return b; })) break;
        continue;
      }

      // Re-time: the segment's start lands at `written_ns` on the output.
      AVStream* ost = out->streams[os->index];
      const std::int64_t shift =
          from_timeline(s, ist, start) - av_rescale_q(written_ns, kNs, ist->time_base);
      if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
      if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
      av_packet_rescale_ts(pkt.get(), ist->time_base, ost->time_base);
      out_stream& o = map[idx];
      if (pkt->dts != AV_NOPTS_VALUE && o.last_dts != INT64_MIN && pkt->dts <= o.last_dts) {
        // A join (remove-middle) can meet a reordered GOP's early dts; the
        // muxer needs strictly increasing dts per stream.
        pkt->dts = o.last_dts + 1;
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) pkt->pts = pkt->dts;
      }
      if (pkt->dts != AV_NOPTS_VALUE) o.last_dts = pkt->dts;
      pkt->stream_index = os->index;
      pkt->pos = -1;

      if (ctl.progress != nullptr && (idx == s.video || !video_picked)) {
        const double f = static_cast<double>(written_ns + std::max<time_ns>(0, t - start)) /
                         static_cast<double>(total);
        ctl.progress(ctl.user, progress_base + progress_span * std::clamp(f, 0.0, 1.0));
      }
      const int wr = av_interleaved_write_frame(out.get(), pkt.get());
      if (wr < 0) return err(cancelled(ctl.cancel) ? status::cancelled : status::io);
    }
    written_ns += std::max<time_ns>(0, end - start);
  }

  if (av_write_trailer(out.get()) < 0) return err(cancelled(ctl.cancel) ? status::cancelled : status::io);
  return {};
}

}  // namespace mv::edit::clip::detail
