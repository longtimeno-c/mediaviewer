// SPDX-License-Identifier: GPL-2.0-or-later
// clip::run: one request -> checked range -> staged output(s) -> publish.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "edit/clip_internal.h"
#include "edit/hwencode.h"
#include "io/collision_name.h"
#include "io/file_port.h"
#include "io/replace.h"

namespace mv::edit::clip {
namespace detail {
namespace {

[[nodiscard]] std::string stem_of(std::string_view path) {
  const std::string_view name = io::file_name_of(path);
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return std::string(name);
  return std::string(name.substr(0, dot));
}

// "01-02.345" (m-ss.mmm), "1-01-02.345" past an hour: sortable, and no ':'
// (Windows refuses it in a name).
[[nodiscard]] std::string time_label(time_ns t) {
  const std::int64_t ms = std::max<time_ns>(0, t) / 1'000'000;
  const std::int64_t h = ms / 3'600'000, m = (ms / 60'000) % 60, s = (ms / 1000) % 60, f = ms % 1000;
  char buf[48];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lld-%02lld-%02lld.%03lld", static_cast<long long>(h),
                  static_cast<long long>(m), static_cast<long long>(s), static_cast<long long>(f));
  } else {
    std::snprintf(buf, sizeof(buf), "%02lld-%02lld.%03lld", static_cast<long long>(m),
                  static_cast<long long>(s), static_cast<long long>(f));
  }
  return buf;
}

// The container a copied audio track goes into, by codec.
struct audio_box {
  const char* muxer;
  const char* ext;
};

[[nodiscard]] audio_box audio_container(AVCodecID id) noexcept {
  switch (id) {
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_ALAC: return {"ipod", ".m4a"};
    case AV_CODEC_ID_MP3: return {"mp3", ".mp3"};
    case AV_CODEC_ID_OPUS: return {"opus", ".opus"};
    case AV_CODEC_ID_VORBIS: return {"ogg", ".ogg"};
    case AV_CODEC_ID_FLAC: return {"flac", ".flac"};
    case AV_CODEC_ID_AC3: return {"ac3", ".ac3"};
    case AV_CODEC_ID_EAC3: return {"eac3", ".eac3"};
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_U8: return {"wav", ".wav"};
    default: return {"matroska", ".mka"};
  }
}

[[nodiscard]] result<std::string> copy_to(const std::string& dir, const std::string& name,
                                          std::string_view src, const copy_spec& spec, const control& ctl,
                                          double pbase, double pspan) {
  staged_output out(dir, name);
  MV_TRY_VOID(copy_segments(src, out.temp_path(), spec, ctl, pbase, pspan));
  if (cancelled(ctl.cancel)) return err(status::cancelled);
  return out.publish();
}

// A still has no seekable container, so it is written whole, exclusively,
// under the first free name.
[[nodiscard]] result<std::string> write_new_unique(const std::string& dir, const std::string& name,
                                                   const std::vector<std::uint8_t>& bytes) {
  for (int n = 1; n <= 9999; ++n) {
    const std::string dest = io::join_path(dir, n == 1 ? name : io::collision_name(name, n));
    if (io::stat_path(dest)) continue;
    auto w = io::write_new(dest, bytes);
    if (w) return dest;
    if (io::stat_path(dest)) continue;  // lost a race to another writer
    return err(w.error());
  }
  return err(status::io);
}

}  // namespace

result<outcome> run_with_encoders(const request& req, const control& ctl, std::vector<std::string> encoders,
                                  bool allow_software) {
  if (req.source.empty()) return err(status::invalid_arg);
  const std::string dir = req.out_dir.empty() ? std::string(io::parent_of(req.source)) : req.out_dir;
  const std::string stem = stem_of(req.source);
  outcome done;

  switch (req.kind) {
    case op::trim_keyframe: {
      MV_TRY(clip_info info, probe(req.source, ctl.cancel));
      const range r = keyframe_range(info, req.in_ns, req.out_ns);
      if (r.out_ns <= r.in_ns) return err(status::invalid_arg);
      copy_spec spec;
      spec.muxer = muxer_for_family(info.container);
      spec.segments.push_back({r.in_ns, r.out_ns >= info.duration_ns ? -1 : r.out_ns});
      MV_TRY(std::string path, copy_to(dir, stem + output_stem_suffix(req.kind) + extension_for_muxer(spec.muxer),
                                       req.source, spec, ctl, 0.0, 1.0));
      done.outputs.push_back(std::move(path));
      done.written = r;
      return done;
    }
    case op::trim_reencode: {
      MV_TRY(clip_info info, probe(req.source, ctl.cancel));
      if (!info.has_video) return err(status::unsupported_format);
      const time_ns in_ns = std::clamp<time_ns>(req.in_ns, 0, info.duration_ns);
      const time_ns out_ns = req.out_ns < 0 ? info.duration_ns : std::min(req.out_ns, info.duration_ns);
      if (out_ns <= in_ns) return err(status::invalid_arg);
      reencode_spec spec;
      spec.in_ns = in_ns;
      spec.out_ns = out_ns;
      spec.allow_software = allow_software;
      // MP4 and MOV stay what they were; anything else becomes Matroska,
      // which holds whatever audio the source carried.
      spec.muxer = info.container == "mp4" || info.container == "mov" ? muxer_for_family(info.container)
                                                                        : "matroska";
      spec.encoders = std::move(encoders);
      if (spec.encoders.empty()) return err(status::unsupported_format);
      staged_output out(dir, stem + output_stem_suffix(req.kind) + extension_for_muxer(spec.muxer));
      MV_TRY(std::string used, reencode(req.source, out.temp_path(), spec, ctl));
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      MV_TRY(std::string path, out.publish());
      done.outputs.push_back(std::move(path));
      done.encoder = std::move(used);
      done.written = {in_ns, out_ns};
      return done;
    }
    case op::rotate: {
      if (req.rotate_degrees % 90 != 0) return err(status::invalid_arg);
      MV_TRY(source s, open_source(req.source, ctl.cancel));
      if (s.video < 0) return err(status::unsupported_format);
      const int now = stream_rotation(s.format->streams[s.video]);
      const std::string family = family_of(s, req.source);
      s.format.reset();
      copy_spec spec;
      spec.muxer = muxer_for_family(family);
      spec.segments.push_back({0, -1});
      spec.rotation = ((now + req.rotate_degrees) % 360 + 360) % 360;
      MV_TRY(std::string path, copy_to(dir, stem + output_stem_suffix(req.kind) + extension_for_muxer(spec.muxer),
                                       req.source, spec, ctl, 0.0, 1.0));
      done.outputs.push_back(std::move(path));
      return done;
    }
    case op::split: {
      MV_TRY(clip_info info, probe(req.source, ctl.cancel));
      const time_ns k = keyframe_nearest(info.keyframes_ns, std::clamp<time_ns>(req.in_ns, 0, info.duration_ns));
      if (k <= 0 || k >= info.duration_ns) return err(status::invalid_arg);
      const char* muxer = muxer_for_family(info.container);
      const std::string ext = extension_for_muxer(muxer);
      // Both halves are staged before either is published, so a cancel in
      // the second half leaves neither.
      staged_output a(dir, stem + "_part1" + ext);
      staged_output b(dir, stem + "_part2" + ext);
      copy_spec one{{{0, k}}, muxer, stream_pick::all_av, -1};
      copy_spec two{{{k, -1}}, muxer, stream_pick::all_av, -1};
      MV_TRY_VOID(copy_segments(req.source, a.temp_path(), one, ctl, 0.0, 0.5));
      MV_TRY_VOID(copy_segments(req.source, b.temp_path(), two, ctl, 0.5, 0.5));
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      MV_TRY(std::string pa, a.publish());
      MV_TRY(std::string pb, b.publish());
      done.outputs.push_back(std::move(pa));
      done.outputs.push_back(std::move(pb));
      done.written = {k, k};
      return done;
    }
    case op::remove_middle: {
      MV_TRY(clip_info info, probe(req.source, ctl.cancel));
      const time_ns end = info.duration_ns;
      const time_ns a = keyframe_nearest(info.keyframes_ns, std::clamp<time_ns>(req.in_ns, 0, end));
      const time_ns b = req.out_ns < 0 || req.out_ns >= end
                            ? end
                            : keyframe_nearest(info.keyframes_ns, std::clamp<time_ns>(req.out_ns, 0, end));
      if (b <= a) return err(status::invalid_arg);
      if (a <= 0 && b >= end) return err(status::invalid_arg);  // nothing would be left
      copy_spec spec;
      spec.muxer = muxer_for_family(info.container);
      if (a > 0) spec.segments.push_back({0, a});
      if (b < end) spec.segments.push_back({b, -1});
      MV_TRY(std::string path, copy_to(dir, stem + output_stem_suffix(req.kind) + extension_for_muxer(spec.muxer),
                                       req.source, spec, ctl, 0.0, 1.0));
      done.outputs.push_back(std::move(path));
      done.written = {a, b};
      return done;
    }
    case op::remux: {
      copy_spec spec;
      spec.muxer = req.remux == remux_target::mkv ? "matroska" : "mp4";
      spec.segments.push_back({0, -1});
      MV_TRY(std::string path, copy_to(dir, stem + extension_for_muxer(spec.muxer), req.source, spec, ctl, 0.0, 1.0));
      done.outputs.push_back(std::move(path));
      return done;
    }
    case op::frame: {
      MV_TRY(std::vector<std::uint8_t> bytes, frame_image(req.source, req.in_ns, req.frame, req.jpeg_quality, ctl));
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      const char* ext = req.frame == frame_format::jpeg ? ".jpg" : ".png";
      MV_TRY(std::string path, write_new_unique(dir, stem + "_frame_" + time_label(req.in_ns) + ext, bytes));
      done.outputs.push_back(std::move(path));
      done.written = {req.in_ns, req.in_ns};
      return done;
    }
    case op::audio: {
      if (req.audio == audio_format::copy) {
        MV_TRY(source s, open_source(req.source, ctl.cancel));
        if (s.audio < 0) return err(status::unsupported_format);
        const audio_box box = audio_container(s.format->streams[s.audio]->codecpar->codec_id);
        s.format.reset();
        copy_spec spec;
        spec.muxer = box.muxer;
        spec.pick = stream_pick::audio_only;
        spec.segments.push_back({std::max<time_ns>(0, req.in_ns), req.out_ns});
        MV_TRY(std::string path, copy_to(dir, stem + output_stem_suffix(req.kind) + box.ext, req.source, spec,
                                         ctl, 0.0, 1.0));
        done.outputs.push_back(std::move(path));
        return done;
      }
      const bool flac = req.audio == audio_format::flac;
      staged_output out(dir, stem + output_stem_suffix(req.kind) + (flac ? ".flac" : ".wav"));
      MV_TRY_VOID(audio_transcode(req.source, out.temp_path(), req.audio, req.in_ns, req.out_ns, ctl));
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      MV_TRY(std::string path, out.publish());
      done.outputs.push_back(std::move(path));
      return done;
    }
    case op::animation: {
      const bool gif = req.animation == anim_format::gif;
      staged_output out(dir, stem + output_stem_suffix(req.kind) + (gif ? ".gif" : ".webp"));
      MV_TRY_VOID(animation(req.source, out.temp_path(), req, ctl));
      if (cancelled(ctl.cancel)) return err(status::cancelled);
      MV_TRY(std::string path, out.publish());
      done.outputs.push_back(std::move(path));
      return done;
    }
  }
  return err(status::invalid_arg);
}

}  // namespace detail

std::string output_stem_suffix(op kind) noexcept {
  switch (kind) {
    case op::trim_keyframe:
    case op::trim_reencode: return "_trimmed";
    case op::rotate: return "_rotated";
    case op::split: return "_part";
    case op::remove_middle: return "_cut";
    case op::frame: return "_frame";
    case op::audio: return "_audio";
    case op::remux:
    case op::animation: return "";
  }
  return "";
}

result<outcome> run(const request& req, const control& ctl) {
  std::vector<std::string> encoders;
  if (req.kind == op::trim_reencode) {
    // HEVC stays HEVC when this machine has a hardware HEVC encoder; H.264
    // is the fallback for everything, and the only choice for other codecs.
    auto s = detail::open_source(req.source, ctl.cancel);
    if (!s) return err(s.error());
    const bool hevc = s->video >= 0 && s->format->streams[s->video]->codecpar->codec_id == AV_CODEC_ID_HEVC;
    if (hevc) {
      for (const char* n : hwencode::candidates(hwencode::codec::hevc)) encoders.emplace_back(n);
    }
    for (const char* n : hwencode::candidates(hwencode::codec::h264)) encoders.emplace_back(n);
  }
  return detail::run_with_encoders(req, ctl, std::move(encoders), false);
}

}  // namespace mv::edit::clip
