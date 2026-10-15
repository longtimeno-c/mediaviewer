// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #406: a small Matroska file with chapters and a SubRip track, or an MP4 with
// a mov_text track, written in-process (no ffmpeg CLI, no encoder: text cues
// are their packets), so the cue reader's tests run on every CI.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace mv::test::cuefx {

struct line {
  std::int64_t start_ms;
  std::int64_t end_ms;
  std::string text;  // as muxed (markup kept)
};
struct chapter {
  std::int64_t start_ms;
  std::int64_t end_ms;
  std::string title;
};

// `muxer` "matroska" (SubRip) or "mp4" (mov_text).
inline bool make(const std::string& path, const char* muxer, const std::vector<chapter>& chapters,
                 const std::vector<line>& lines) {
  AVFormatContext* out = nullptr;
  if (avformat_alloc_output_context2(&out, nullptr, muxer, path.c_str()) < 0 || !out) return false;
  bool ok = true;
  const bool mp4 = std::strcmp(muxer, "mp4") == 0;
  AVStream* st = avformat_new_stream(out, nullptr);
  ok = st != nullptr;
  if (ok) {
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = mp4 ? AV_CODEC_ID_MOV_TEXT : AV_CODEC_ID_SUBRIP;
    st->time_base = AVRational{1, 1000};
  }
  for (const chapter& c : chapters) {
    if (!ok) break;
    auto* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
    ok = ch != nullptr;
    if (!ok) break;
    ch->id = static_cast<std::int64_t>(out->nb_chapters) + 1;
    ch->time_base = AVRational{1, 1000};
    ch->start = c.start_ms;
    ch->end = c.end_ms;
    av_dict_set(&ch->metadata, "title", c.title.c_str(), 0);
    ok = av_dynarray_add_nofree(&out->chapters, reinterpret_cast<int*>(&out->nb_chapters), ch) >= 0;
  }
  if (ok && !(out->oformat->flags & AVFMT_NOFILE)) ok = avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
  if (ok) ok = avformat_write_header(out, nullptr) >= 0;
  AVPacket* pkt = av_packet_alloc();
  for (const line& l : lines) {
    if (!ok) break;
    std::string data;
    if (mp4) {
      data.push_back(static_cast<char>((l.text.size() >> 8) & 0xFF));
      data.push_back(static_cast<char>(l.text.size() & 0xFF));
    }
    data += l.text;
    ok = av_new_packet(pkt, static_cast<int>(data.size())) >= 0;
    if (!ok) break;
    std::memcpy(pkt->data, data.data(), data.size());
    pkt->stream_index = st->index;
    pkt->pts = pkt->dts = av_rescale_q(l.start_ms, AVRational{1, 1000}, st->time_base);
    pkt->duration = av_rescale_q(l.end_ms - l.start_ms, AVRational{1, 1000}, st->time_base);
    pkt->flags |= AV_PKT_FLAG_KEY;
    ok = av_interleaved_write_frame(out, pkt) >= 0;
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (ok) ok = av_write_trailer(out) >= 0;
  if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
  avformat_free_context(out);
  return ok;
}

}  // namespace mv::test::cuefx
