// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Clip metadata through libavformat: container facts, one inspector record per
// stream (video / audio / subtitle / attachment), chapters and tags. FFmpeg is
// already the video pipeline (plan/05), so this reads in-process rather than
// shelling out to ffprobe (plan/06).
//
// Only container and stream headers are read. No packet is decoded here, and
// nothing about the file leaves the machine (rule 6).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/dict.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "meta/internal.h"

namespace mv::meta::detail {
namespace {

struct format_closer {
  void operator()(AVFormatContext* c) const noexcept { avformat_close_input(&c); }
};
using format_ptr = std::unique_ptr<AVFormatContext, format_closer>;

std::string fmt(const char* f, double a) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), f, a);
  return buf;
}

std::string bitrate_text(std::int64_t bps) {
  if (bps <= 0) return {};
  char buf[32];
  if (bps >= 1'000'000) {
    std::snprintf(buf, sizeof(buf), "%.1f Mb/s", static_cast<double>(bps) / 1e6);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f kb/s", static_cast<double>(bps) / 1e3);
  }
  return buf;
}

std::string duration_text(std::int64_t us) {
  if (us <= 0 || us == AV_NOPTS_VALUE) return {};
  const std::int64_t total_ds = us / 100000;  // tenths of a second
  const std::int64_t s = total_ds / 10, h = s / 3600, m = (s / 60) % 60;
  char buf[48];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%lld", static_cast<long long>(h),
                  static_cast<long long>(m), static_cast<long long>(s % 60),
                  static_cast<long long>(total_ds % 10));
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld.%lld", static_cast<long long>(m),
                  static_cast<long long>(s % 60), static_cast<long long>(total_ds % 10));
  }
  return buf;
}

std::string tag(const AVDictionary* d, const char* key) {
  const AVDictionaryEntry* e = av_dict_get(d, key, nullptr, AV_DICT_IGNORE_SUFFIX);
  return e && e->value ? e->value : std::string();
}

// ISO 6709 "+48.8584+002.2945+035.000/" → "48.85840° N, 2.29450° E".
std::string iso6709(const std::string& s) {
  if (s.size() < 4 || (s[0] != '+' && s[0] != '-')) return {};
  char* end = nullptr;
  const double lat = std::strtod(s.c_str(), &end);
  if (!end || (*end != '+' && *end != '-')) return {};
  const double lon = std::strtod(end, &end);
  if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) return {};
  return format_coordinate(lat, lat < 0 ? 'S' : 'N') + ", " + format_coordinate(lon, lon < 0 ? 'W' : 'E');
}

const char* chroma_name(const AVPixFmtDescriptor* d) {
  if (!d || d->nb_components < 3 || (d->flags & AV_PIX_FMT_FLAG_RGB)) return nullptr;
  if (d->log2_chroma_w == 0 && d->log2_chroma_h == 0) return "4:4:4";
  if (d->log2_chroma_w == 1 && d->log2_chroma_h == 0) return "4:2:2";
  if (d->log2_chroma_w == 1 && d->log2_chroma_h == 1) return "4:2:0";
  if (d->log2_chroma_w == 2 && d->log2_chroma_h == 0) return "4:1:1";
  return nullptr;
}

std::string level_text(AVCodecID id, int level) {
  if (level <= 0) return {};
  char buf[24];
  switch (id) {
    case AV_CODEC_ID_H264:
      std::snprintf(buf, sizeof(buf), "%d.%d", level / 10, level % 10);
      return buf;
    case AV_CODEC_ID_HEVC:
      std::snprintf(buf, sizeof(buf), "%d.%d", level / 30, (level % 30) / 3);
      return buf;
    case AV_CODEC_ID_AV1:
      std::snprintf(buf, sizeof(buf), "%d.%d", 2 + (level >> 2), level & 3);
      return buf;
    default:
      return std::to_string(level);
  }
}

std::string disposition_text(int d) {
  std::string out;
  const auto add = [&](int flag, const char* name) {
    if (d & flag) out += (out.empty() ? "" : ", ") + std::string(name);
  };
  add(AV_DISPOSITION_DEFAULT, "default");
  add(AV_DISPOSITION_FORCED, "forced");
  add(AV_DISPOSITION_ORIGINAL, "original");
  add(AV_DISPOSITION_COMMENT, "commentary");
  add(AV_DISPOSITION_HEARING_IMPAIRED, "hearing impaired");
  add(AV_DISPOSITION_VISUAL_IMPAIRED, "visual impaired");
  add(AV_DISPOSITION_ATTACHED_PIC, "cover art");
  return out;
}

void add(stream_info& s, const char* label, std::string value) {
  if (!value.empty()) s.fields.push_back({label, std::move(value)});
}

void describe_video(const AVStream& st, stream_info& s) {
  const AVCodecParameters& p = *st.codecpar;
  const auto fmt_id = static_cast<AVPixelFormat>(p.format);
  const AVPixFmtDescriptor* desc = p.format >= 0 ? av_pix_fmt_desc_get(fmt_id) : nullptr;

  add(s, "Profile", [&] {
    const char* n = avcodec_profile_name(p.codec_id, p.profile);
    return n ? std::string(n) : std::string();
  }());
  add(s, "Level", level_text(p.codec_id, p.level));
  if (p.width > 0) add(s, "Resolution", std::to_string(p.width) + " \xC3\x97 " + std::to_string(p.height));
  if (p.format >= 0) {
    const char* n = av_get_pix_fmt_name(fmt_id);
    add(s, "Pixel format", n ? n : "");
  }
  int depth = p.bits_per_raw_sample;
  if (depth <= 0 && desc) depth = desc->comp[0].depth;
  if (depth > 0) add(s, "Bit depth", std::to_string(depth) + "-bit");
  if (const char* c = chroma_name(desc)) add(s, "Chroma subsampling", c);

  const AVRational avg = st.avg_frame_rate, real = st.r_frame_rate;
  if (avg.num > 0 && avg.den > 0) {
    std::string fps = fmt("%.3f", av_q2d(avg));
    while (!fps.empty() && fps.back() == '0') fps.pop_back();
    if (!fps.empty() && fps.back() == '.') fps.pop_back();
    // VFR: the average and the base rate disagree by more than 1 %.
    // Too few frames make the average meaningless (a 10-frame clip averages
    // over its own edge effects), so short tracks are never labelled VFR.
    const bool vfr = (st.nb_frames == 0 || st.nb_frames >= 30) && real.num > 0 && real.den > 0 &&
                     std::fabs(av_q2d(avg) - av_q2d(real)) > 0.01 * av_q2d(real);
    add(s, "Frame rate", fps + " fps" + (vfr ? " (variable)" : ""));
  }
  add(s, "Bit rate", bitrate_text(p.bit_rate));

  if (p.color_primaries != AVCOL_PRI_UNSPECIFIED) add(s, "Colour primaries", av_color_primaries_name(p.color_primaries));
  if (p.color_trc != AVCOL_TRC_UNSPECIFIED) add(s, "Transfer", av_color_transfer_name(p.color_trc));
  if (p.color_space != AVCOL_SPC_UNSPECIFIED) add(s, "Matrix", av_color_space_name(p.color_space));
  if (p.color_range != AVCOL_RANGE_UNSPECIFIED) add(s, "Range", av_color_range_name(p.color_range));

  if (const AVPacketSideData* sd = av_packet_side_data_get(
          p.coded_side_data, p.nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
      sd && sd->size >= sizeof(AVMasteringDisplayMetadata)) {
    const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
    if (m->has_primaries) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "R(%.3f, %.3f) G(%.3f, %.3f) B(%.3f, %.3f) W(%.4f, %.4f)",
                    av_q2d(m->display_primaries[0][0]), av_q2d(m->display_primaries[0][1]),
                    av_q2d(m->display_primaries[1][0]), av_q2d(m->display_primaries[1][1]),
                    av_q2d(m->display_primaries[2][0]), av_q2d(m->display_primaries[2][1]),
                    av_q2d(m->white_point[0]), av_q2d(m->white_point[1]));
      add(s, "Mastering display primaries", buf);
    }
    if (m->has_luminance) {
      add(s, "Mastering display luminance",
          fmt("%.4f", av_q2d(m->min_luminance)) + " \xE2\x80\x93 " + fmt("%.0f", av_q2d(m->max_luminance)) + " cd/m\xC2\xB2");
    }
  }
  if (const AVPacketSideData* sd = av_packet_side_data_get(
          p.coded_side_data, p.nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
      sd && sd->size >= sizeof(AVContentLightMetadata)) {
    const auto* c = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
    add(s, "MaxCLL / MaxFALL", std::to_string(c->MaxCLL) + " / " + std::to_string(c->MaxFALL) + " cd/m\xC2\xB2");
  }
  if (const AVPacketSideData* sd = av_packet_side_data_get(
          p.coded_side_data, p.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
      sd && sd->size >= 9 * sizeof(int32_t)) {
    const double rot = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
    if (!std::isnan(rot) && std::fabs(rot) > 0.5) add(s, "Rotation", fmt("%.0f", -rot) + "\xC2\xB0");
  }
}

void describe_audio(const AVStream& st, stream_info& s) {
  const AVCodecParameters& p = *st.codecpar;
  add(s, "Profile", [&] {
    const char* n = avcodec_profile_name(p.codec_id, p.profile);
    return n ? std::string(n) : std::string();
  }());
  if (p.ch_layout.nb_channels > 0) {
    char layout[64] = {};
    av_channel_layout_describe(&p.ch_layout, layout, sizeof(layout));
    add(s, "Channels", std::to_string(p.ch_layout.nb_channels) + (layout[0] ? " (" + std::string(layout) + ")" : ""));
  }
  if (p.sample_rate > 0) add(s, "Sample rate", std::to_string(p.sample_rate) + " Hz");
  int depth = p.bits_per_raw_sample > 0 ? p.bits_per_raw_sample : p.bits_per_coded_sample;
  if (depth > 0) add(s, "Bit depth", std::to_string(depth) + "-bit");
  add(s, "Bit rate", bitrate_text(p.bit_rate));
}

stream_kind kind_of(AVMediaType t) {
  switch (t) {
    case AVMEDIA_TYPE_VIDEO: return stream_kind::video;
    case AVMEDIA_TYPE_AUDIO: return stream_kind::audio;
    case AVMEDIA_TYPE_SUBTITLE: return stream_kind::subtitle;
    case AVMEDIA_TYPE_ATTACHMENT: return stream_kind::attachment;
    default: return stream_kind::data;
  }
}

format_ptr open(std::string_view utf8_path, bool probe_streams) {
  const std::string path(utf8_path);
  AVFormatContext* raw = nullptr;
  if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) return nullptr;
  format_ptr ctx(raw);
  if (probe_streams) {
    // Bounded: headers only, on a worker, and never an unbounded scan of a
    // hostile file.
    ctx->probesize = 8 * 1024 * 1024;
    ctx->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(ctx.get(), nullptr) < 0) return nullptr;
  }
  return ctx;
}

void add_tags(metadata& out, const char* group, const AVDictionary* d) {
  const AVDictionaryEntry* e = nullptr;
  while ((e = av_dict_iterate(d, e))) {
    if (out.properties.size() >= 4000) return;
    property p;
    p.space = origin::container;
    p.group = group;
    p.name = e->key;
    p.label = e->key;
    p.value = p.raw = e->value ? e->value : "";
    p.raw_tag = std::string(group) + "." + e->key;
    out.properties.push_back(std::move(p));
  }
}

}  // namespace

bool read_clip(std::string_view utf8_path, metadata& out) noexcept {
  try {
    format_ptr ctx = open(utf8_path, true);
    if (!ctx) return false;
    out.is_clip = true;
    summary& s = out.s;
    s.format = ctx->iformat && ctx->iformat->long_name ? ctx->iformat->long_name : "";
    s.duration = duration_text(ctx->duration);
    std::int64_t bps = ctx->bit_rate;
    if (bps <= 0 && ctx->duration > 0 && s.file_size > 0) {
      bps = static_cast<std::int64_t>(static_cast<double>(s.file_size) * 8.0 * AV_TIME_BASE / static_cast<double>(ctx->duration));
    }
    s.bitrate = bitrate_text(bps);
    if (ctx->duration > 0) s.duration_seconds = static_cast<double>(ctx->duration) / AV_TIME_BASE;
    s.bitrate_bps = std::max<std::int64_t>(0, bps);

    add_tags(out, "Container", ctx->metadata);
    out.properties.push_back({origin::computed, "Container", "Format", "Format", s.format, s.format, "Container.Format"});
    if (!s.duration.empty()) {
      out.properties.push_back({origin::computed, "Container", "Duration", "Duration", s.duration, s.duration, "Container.Duration"});
    }

    bool have_video = false;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
      const AVStream& st = *ctx->streams[i];
      stream_info info;
      info.index = static_cast<int>(i);
      info.kind = kind_of(st.codecpar->codec_type);
      const AVCodecDescriptor* cd = avcodec_descriptor_get(st.codecpar->codec_id);
      info.codec = cd && cd->long_name ? cd->long_name : (cd && cd->name ? cd->name : "unknown");
      switch (info.kind) {
        case stream_kind::video: describe_video(st, info); break;
        case stream_kind::audio:
          describe_audio(st, info);
          if (s.audio_channels == 0) {
            s.audio_channels = std::max(0, st.codecpar->ch_layout.nb_channels);
            s.audio_sample_rate = std::max(0, st.codecpar->sample_rate);
          }
          break;
        default: break;
      }
      if ((info.kind == stream_kind::video && !(st.disposition & AV_DISPOSITION_ATTACHED_PIC)) ||
          info.kind == stream_kind::audio) {
        if (std::find(s.codecs.begin(), s.codecs.end(), info.codec) == s.codecs.end()) {
          s.codecs.push_back(info.codec);
        }
      }
      add(info, "Language", tag(st.metadata, "language"));
      add(info, "Title", tag(st.metadata, "title"));
      add(info, "Disposition", disposition_text(st.disposition));
      if (info.kind == stream_kind::attachment) {
        add(info, "File name", tag(st.metadata, "filename"));
        add(info, "MIME type", tag(st.metadata, "mimetype"));
      }
      // The summary follows the first real video stream (not cover art).
      if (info.kind == stream_kind::video && !have_video && !(st.disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        have_video = true;
        s.width = static_cast<std::uint32_t>(std::max(0, st.codecpar->width));
        s.height = static_cast<std::uint32_t>(std::max(0, st.codecpar->height));
        s.codec = cd && cd->name ? cd->name : "";
      }
      add_tags(out, ("Stream #" + std::to_string(i)).c_str(), st.metadata);
      out.streams.push_back(std::move(info));
    }

    for (unsigned i = 0; i < ctx->nb_chapters; ++i) {
      const AVChapter& c = *ctx->chapters[i];
      const double tb = av_q2d(c.time_base);
      out.chapters.push_back({static_cast<std::int64_t>(static_cast<double>(c.start) * tb * 1000.0),
                              static_cast<std::int64_t>(static_cast<double>(c.end) * tb * 1000.0),
                              tag(c.metadata, "title")});
    }

    for (const char* key : {"creation_time", "com.apple.quicktime.creationdate", "date"}) {
      const std::string v = tag(ctx->metadata, key);
      if (const auto k = parse_date_key(v)) {
        s.date_taken_key = *k;
        s.date_taken = v.substr(0, std::min<std::size_t>(v.size(), 19));
        if (s.date_taken.size() > 10 && s.date_taken[10] == 'T') s.date_taken[10] = ' ';
        break;
      }
    }
    const std::string make = tag(ctx->metadata, "com.apple.quicktime.make");
    const std::string model = tag(ctx->metadata, "com.apple.quicktime.model");
    s.camera = model.empty() ? make : make.empty() ? model : make + " " + model;
    for (const char* key : {"com.apple.quicktime.location.ISO6709", "location"}) {
      s.gps = iso6709(tag(ctx->metadata, key));
      if (!s.gps.empty()) break;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::int64_t> clip_date_key(std::string_view utf8_path) noexcept {
  try {
    format_ptr ctx = open(utf8_path, false);  // header only: no stream probing
    if (!ctx) return std::nullopt;
    for (const char* key : {"creation_time", "com.apple.quicktime.creationdate", "date"}) {
      if (const auto k = parse_date_key(tag(ctx->metadata, key))) return k;
    }
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace mv::meta::detail
