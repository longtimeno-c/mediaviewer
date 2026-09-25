// SPDX-License-Identifier: GPL-2.0-or-later
// Clip probe, keyframe index, snapping, and the staged-output publish.
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include "edit/clip_internal.h"
#include "io/collision_name.h"
#include "io/file_port.h"

namespace mv::edit::clip {
namespace detail {
namespace {

int interrupt_cb(void* opaque) noexcept {
  const auto* flag = static_cast<const std::atomic<bool>*>(opaque);
  return flag != nullptr && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

[[nodiscard]] bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
  if (s.size() < suffix.size()) return false;
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    char a = s[s.size() - suffix.size() + i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != suffix[i]) return false;
  }
  return true;
}

}  // namespace

result<source> open_source(std::string_view utf8_path, const std::atomic<bool>* cancel) {
  const std::string path(utf8_path);
  AVFormatContext* raw = avformat_alloc_context();
  if (raw == nullptr) return err(status::out_of_memory);
  raw->interrupt_callback.callback = interrupt_cb;
  raw->interrupt_callback.opaque = const_cast<std::atomic<bool>*>(cancel);
  // avformat_open_input frees `raw` on failure.
  if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
    return err(cancelled(cancel) ? status::cancelled : status::io);
  }
  source s;
  s.format.reset(raw);
  if (avformat_find_stream_info(raw, nullptr) < 0) {
    return err(cancelled(cancel) ? status::cancelled : status::corrupt);
  }
  s.video = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  s.audio = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (s.video >= 0 && (raw->streams[s.video]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
    s.video = -1;  // cover art in an audio file is not a clip
  }
  if (s.video < 0 && s.audio < 0) return err(status::unsupported_format);

  // The player's origin (player/video_source.cpp): container start_time,
  // else the stream's.
  if (raw->start_time != AV_NOPTS_VALUE) {
    s.origin_ns = av_rescale_q(raw->start_time, AVRational{1, AV_TIME_BASE}, kNs);
  } else {
    const AVStream* st = raw->streams[s.video >= 0 ? s.video : s.audio];
    if (st->start_time != AV_NOPTS_VALUE) s.origin_ns = av_rescale_q(st->start_time, st->time_base, kNs);
  }
  if (raw->duration != AV_NOPTS_VALUE && raw->duration > 0) {
    s.duration_ns = av_rescale_q(raw->duration, AVRational{1, AV_TIME_BASE}, kNs);
  } else {
    const AVStream* st = raw->streams[s.video >= 0 ? s.video : s.audio];
    if (st->duration != AV_NOPTS_VALUE) s.duration_ns = av_rescale_q(st->duration, st->time_base, kNs);
  }
  return s;
}

time_ns to_timeline(const source& s, const AVStream* st, std::int64_t ts) noexcept {
  return av_rescale_q(ts, st->time_base, kNs) - s.origin_ns;
}

std::int64_t from_timeline(const source& s, const AVStream* st, time_ns t) noexcept {
  return av_rescale_q(t + s.origin_ns, kNs, st->time_base);
}

void seek_before(source& s, time_ns t) noexcept {
  if (t <= 0) return;
  const int idx = s.video >= 0 ? s.video : s.audio;
  const AVStream* st = s.format->streams[idx];
  const std::int64_t ts = from_timeline(s, st, t);
  if (av_seek_frame(s.format.get(), idx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
    // A demuxer without an index for that stream: seek the file instead.
    (void)avformat_seek_file(s.format.get(), -1, INT64_MIN,
                             av_rescale_q(t + s.origin_ns, kNs, AVRational{1, AV_TIME_BASE}),
                             av_rescale_q(t + s.origin_ns, kNs, AVRational{1, AV_TIME_BASE}), 0);
  }
}

int stream_rotation(const AVStream* st) noexcept {
  const AVCodecParameters* p = st->codecpar;
  const AVPacketSideData* sd =
      av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  if (sd == nullptr || sd->size < 9 * 4) return 0;
  // av_display_rotation_get is counter-clockwise; players turn the other way.
  const double ccw = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(sd->data));
  if (ccw != ccw) return 0;  // NaN: a degenerate matrix
  int cw = static_cast<int>(std::lround(-ccw / 90.0)) * 90;
  cw %= 360;
  if (cw < 0) cw += 360;
  return cw;
}

bool set_rotation(AVStream* out, int clockwise) noexcept {
  clockwise %= 360;
  if (clockwise < 0) clockwise += 360;
  AVCodecParameters* p = out->codecpar;
  // Drop any matrix the copy carried, then add ours (identity at 0).
  av_packet_side_data_remove(p->coded_side_data, &p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  AVPacketSideData* sd = av_packet_side_data_new(&p->coded_side_data, &p->nb_coded_side_data,
                                                 AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
  if (sd == nullptr) return false;
  // _set takes a clockwise angle; _get (stream_rotation) returns counter-clockwise.
  av_display_rotation_set(reinterpret_cast<std::int32_t*>(sd->data), static_cast<double>(clockwise));
  return true;
}

const char* muxer_for_family(std::string_view family) noexcept {
  if (family == "mp4") return "mp4";
  if (family == "mov") return "mov";
  if (family == "webm") return "webm";
  if (family == "avi") return "avi";
  if (family == "mpegts") return "mpegts";
  return "matroska";
}

const char* extension_for_muxer(std::string_view muxer) noexcept {
  if (muxer == "mp4") return ".mp4";
  if (muxer == "mov") return ".mov";
  if (muxer == "webm") return ".webm";
  if (muxer == "avi") return ".avi";
  if (muxer == "mpegts") return ".ts";
  if (muxer == "ipod") return ".m4a";
  if (muxer == "mp3") return ".mp3";
  if (muxer == "ogg") return ".ogg";
  if (muxer == "opus") return ".opus";
  if (muxer == "flac") return ".flac";
  if (muxer == "wav") return ".wav";
  if (muxer == "ac3") return ".ac3";
  if (muxer == "eac3") return ".eac3";
  if (muxer == "gif") return ".gif";
  if (muxer == "webp") return ".webp";
  return ".mkv";
}

std::string family_of(const source& s, std::string_view utf8_path) {
  const char* name = s.format->iformat->name;
  if (std::strstr(name, "mp4") != nullptr || std::strstr(name, "mov") != nullptr) {
    return ends_with_ci(utf8_path, ".mov") || ends_with_ci(utf8_path, ".qt") ? "mov" : "mp4";
  }
  if (std::strstr(name, "matroska") != nullptr) {
    return ends_with_ci(utf8_path, ".webm") ? "webm" : "matroska";
  }
  if (std::strcmp(name, "avi") == 0) return "avi";
  if (std::strcmp(name, "mpegts") == 0) return "mpegts";
  return "matroska";
}

// ---- staged output --------------------------------------------------------------

staged_output::staged_output(std::string dir, std::string final_name)
    : dir_(std::move(dir)), name_(std::move(final_name)) {
  // A dot name with an extension no listing matches (io/dir_*.cpp list by
  // extension and skip dot files), so the half-written file never shows up
  // in the folder the user is browsing.
  temp_ = io::join_path(dir_, "." + name_ + ".mvpart");
  (void)io::remove_file(temp_);  // a leftover from a crash, ours by name
  live_ = true;
}

staged_output::~staged_output() { discard(); }

staged_output::staged_output(staged_output&& o) noexcept
    : dir_(std::move(o.dir_)), name_(std::move(o.name_)), temp_(std::move(o.temp_)), live_(o.live_) {
  o.live_ = false;
}

staged_output& staged_output::operator=(staged_output&& o) noexcept {
  if (this != &o) {
    discard();
    dir_ = std::move(o.dir_);
    name_ = std::move(o.name_);
    temp_ = std::move(o.temp_);
    live_ = o.live_;
    o.live_ = false;
  }
  return *this;
}

void staged_output::discard() noexcept {
  if (!live_) return;
  (void)io::remove_file(temp_);
  live_ = false;
}

result<std::string> staged_output::publish() {
  if (!live_) return err(status::internal);
  for (int n = 1; n <= 9999; ++n) {
    const std::string name = n == 1 ? name_ : io::collision_name(name_, n);
    const std::string dest = io::join_path(dir_, name);
    auto r = io::rename_no_replace(temp_, dest);
    if (!r) return err(r.error());
    if (*r == io::rename_outcome::renamed) {
      live_ = false;
      return dest;
    }
  }
  return err(status::io);
}

result<output_ptr> open_output(const char* muxer, const std::string& path,
                               const std::atomic<bool>* cancel) {
  AVFormatContext* raw = nullptr;
  if (avformat_alloc_output_context2(&raw, nullptr, muxer, path.c_str()) < 0 || raw == nullptr) {
    return err(status::unsupported_format);
  }
  output_ptr out(raw);
  raw->interrupt_callback.callback = interrupt_cb;
  raw->interrupt_callback.opaque = const_cast<std::atomic<bool>*>(cancel);
  if (!(raw->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open2(&raw->pb, path.c_str(), AVIO_FLAG_WRITE, &raw->interrupt_callback, nullptr) < 0) {
      return err(status::io);
    }
  }
  return out;
}

void copy_metadata(const AVFormatContext* in, AVFormatContext* out) noexcept {
  av_dict_copy(&out->metadata, in->metadata, 0);
  // The muxer writes its own encoder tag; a stale one would lie.
  av_dict_set(&out->metadata, "encoder", nullptr, 0);
}

}  // namespace detail

// ---- probe --------------------------------------------------------------------

using namespace detail;

result<clip_info> probe(std::string_view utf8_path, const std::atomic<bool>* cancel) {
  MV_TRY(source s, open_source(utf8_path, cancel));
  clip_info info;
  info.duration_ns = s.duration_ns;
  info.has_audio = s.audio >= 0;
  info.has_video = s.video >= 0;
  info.container = family_of(s, utf8_path);
  if (s.video < 0) return info;

  AVStream* vs = s.format->streams[s.video];
  const AVCodecParameters* p = vs->codecpar;
  info.width = static_cast<std::uint32_t>(std::max(0, p->width));
  info.height = static_cast<std::uint32_t>(std::max(0, p->height));
  info.video_codec = avcodec_get_name(p->codec_id);
  info.rotation = stream_rotation(vs);
  const AVRational fr = vs->avg_frame_rate.num > 0 ? vs->avg_frame_rate : vs->r_frame_rate;
  info.frame_rate = fr.den != 0 ? av_q2d(fr) : 0.0;
  if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p->format))) {
    info.bit_depth = d->comp[0].depth;
  }
  info.hdr = p->color_trc == AVCOL_TRC_SMPTE2084 || p->color_trc == AVCOL_TRC_ARIB_STD_B67;

  // The grid: the keyframe packets' pts. Every other stream is discarded at
  // the demuxer. Not AVDISCARD_NONKEY on the video: MOV skips the non-key
  // samples but not their composition offsets, so with B-frames every
  // keyframe after the first comes back with another frame's pts (found by
  // test_clip's B-frame cases). Packets are read, never decoded.
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.video) s.format->streams[i]->discard = AVDISCARD_ALL;
  }
  packet_ptr pkt(av_packet_alloc());
  if (!pkt) return err(status::out_of_memory);
  while (true) {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(s.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      break;  // EOF or a truncated tail: the index holds what was readable
    }
    if (pkt->stream_index == s.video && (pkt->flags & AV_PKT_FLAG_KEY)) {
      const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
      if (ts != AV_NOPTS_VALUE) info.keyframes_ns.push_back(to_timeline(s, vs, ts));
    }
    av_packet_unref(pkt.get());
  }
  std::sort(info.keyframes_ns.begin(), info.keyframes_ns.end());
  info.keyframes_ns.erase(std::unique(info.keyframes_ns.begin(), info.keyframes_ns.end()),
                          info.keyframes_ns.end());
  return info;
}

// ---- snapping ---------------------------------------------------------------------

time_ns keyframe_at_or_before(const std::vector<time_ns>& kf, time_ns t) noexcept {
  if (kf.empty()) return t;
  auto it = std::upper_bound(kf.begin(), kf.end(), t);
  if (it == kf.begin()) return kf.front() <= 0 ? kf.front() : 0;
  return *(it - 1);
}

time_ns keyframe_at_or_after(const std::vector<time_ns>& kf, time_ns t, time_ns end) noexcept {
  if (kf.empty()) return std::min(t, end);
  auto it = std::lower_bound(kf.begin(), kf.end(), t);
  return it == kf.end() ? end : std::min(*it, end);
}

time_ns keyframe_nearest(const std::vector<time_ns>& kf, time_ns t) noexcept {
  if (kf.empty()) return t;
  auto it = std::lower_bound(kf.begin(), kf.end(), t);
  if (it == kf.end()) return kf.back();
  if (it == kf.begin()) return *it;
  const time_ns after = *it;
  const time_ns before = *(it - 1);
  return (t - before) <= (after - t) ? before : after;
}

range keyframe_range(const clip_info& info, time_ns in_ns, time_ns out_ns) noexcept {
  const time_ns end = info.duration_ns;
  if (out_ns < 0 || out_ns > end) out_ns = end;
  in_ns = std::clamp<time_ns>(in_ns, 0, end);
  range r;
  r.in_ns = std::max<time_ns>(0, keyframe_at_or_before(info.keyframes_ns, in_ns));
  r.out_ns = out_ns >= end ? end : keyframe_at_or_after(info.keyframes_ns, out_ns, end);
  if (r.out_ns <= r.in_ns) r.out_ns = keyframe_at_or_after(info.keyframes_ns, r.in_ns + 1, end);
  return r;
}

}  // namespace mv::edit::clip
