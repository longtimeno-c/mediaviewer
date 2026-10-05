// SPDX-License-Identifier: GPL-3.0-or-later
// encprobe: spike S1 for the video/audio editor add-on (docs/design/21-video-editor.md).
//
// What can THIS machine's build encode and filter, and how fast? Prints a
// Markdown report on stdout:
//
//   1. Video encoders: the encode port's candidates (edit/hwencode.h) plus the
//      editor's extra candidates (AV1, ProRes), each opened at 1080p30 8-bit,
//      2160p30 8-bit and 2160p60 10-bit, fed synthetic frames, timed. The same
//      licence gate as Path 2 (edit/clip_encode.cpp): software HEVC and
//      x264 / x265 are refused, never probed.
//   2. Audio encoders the export path could use.
//   3. The filters the LGPL FFmpeg build carries (audio clean-up, loudness,
//      waveform, LUT / colour), and the GPL-only ones it must NOT carry.
//   4. Hardware decode device types (preview).
//
// Synthetic frames only: it opens no user file and prints no path (rule 6).
// Throughput is an upper bound (no decode, no grade, no mux); real-time
// preview is spike S2.
//
// Build: an opt-in tool, not part of the app. See tools/encprobe/CMakeLists.txt.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "edit/hwencode.h"

namespace {

namespace hw = mv::edit::hwencode;

struct video_case {
  const char* label;
  int width;
  int height;
  int fps;
  bool ten_bit;
};

constexpr video_case kCases[] = {
    {"1080p30 8-bit", 1920, 1080, 30, false},
    {"2160p30 8-bit", 3840, 2160, 30, false},
    {"2160p60 10-bit", 3840, 2160, 60, true},
};

constexpr int kFrames = 90;

// Encoders the editor may add beyond Path 2's port. Hardware only; a name the
// build lacks is reported as "not in build".
#if defined(_WIN32)
constexpr const char* kExtraVideo[] = {"av1_nvenc", "av1_qsv", "av1_amf"};
#elif defined(__APPLE__)
constexpr const char* kExtraVideo[] = {"prores_videotoolbox"};
#else
constexpr const char* kExtraVideo[] = {"h264_vaapi"};
#endif

constexpr const char* kAudioEncoders[] = {"aac_at", "aac_mf", "aac", "alac_at", "alac",
                                          "flac",   "pcm_s16le", "pcm_s24le", "opus", "libopus"};

struct filter_row {
  const char* name;
  const char* use;
  bool must_be_absent;  // GPL-only in FFmpeg: its presence means a GPL build
};

constexpr filter_row kFilters[] = {
    {"volume", "gain", false},
    {"equalizer", "parametric EQ band", false},
    {"highpass", "rumble cut", false},
    {"acompressor", "dynamics", false},
    {"alimiter", "true-peak guard", false},
    {"loudnorm", "EBU R128 normalise", false},
    {"ebur128", "loudness meter", false},
    {"afftdn", "spectral noise reduction (no model)", false},
    {"anlmdn", "non-local-means noise reduction (no model)", false},
    {"arnndn", "RNNoise-style speech denoise (needs a model file)", false},
    {"showwavespic", "waveform image", false},
    {"aresample", "mixing sample-rate conversion", false},
    {"amix", "multi-clip audio mix", false},
    {"lut3d", ".cube LUT (reference only; grading runs in our kernel)", false},
    {"zscale", "HDR / colour conversion", false},
    {"tonemap", "HDR to SDR", false},
    {"colorspace", "matrix conversion", false},
    {"eq", "GPL-only video filter", true},
    {"hqdn3d", "GPL-only video filter", true},
    {"delogo", "GPL-only video filter", true},
};

bool is_hardware(const AVCodec* c) {
  return (c->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) != 0;
}

// The same gate as edit/clip_encode.cpp licence_ok(allow_software = false),
// with an explicit list of software encoders the editor may use for proxies
// and intermediates (none today).
bool licence_ok(const AVCodec* c) {
  if (std::strncmp(c->name, "libx26", 6) == 0 || std::strcmp(c->name, "libkvazaar") == 0) return false;
  return is_hardware(c);
}

std::vector<AVPixelFormat> sw_formats(const AVCodec* c) {
  std::vector<AVPixelFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* cfg = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg) {
    const auto* f = static_cast<const AVPixelFormat*>(cfg);
    for (int i = 0; i < n; ++i) {
      const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f[i]);
      if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(f[i]);
    }
  }
#else
  for (const AVPixelFormat* f = c->pix_fmts; f && *f != AV_PIX_FMT_NONE; ++f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*f);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) out.push_back(*f);
  }
#endif
  return out;
}

AVPixelFormat pick(const std::vector<AVPixelFormat>& f, bool ten_bit, bool prores) {
  const auto has = [&](AVPixelFormat p) { return std::find(f.begin(), f.end(), p) != f.end(); };
  if (prores) {
    if (has(AV_PIX_FMT_P210LE)) return AV_PIX_FMT_P210LE;
    if (has(AV_PIX_FMT_YUV422P10LE)) return AV_PIX_FMT_YUV422P10LE;
  }
  if (ten_bit && has(AV_PIX_FMT_P010LE)) return AV_PIX_FMT_P010LE;
  if (ten_bit && has(AV_PIX_FMT_YUV420P10LE)) return AV_PIX_FMT_YUV420P10LE;
  if (ten_bit) return AV_PIX_FMT_NONE;
  if (has(AV_PIX_FMT_NV12)) return AV_PIX_FMT_NV12;
  if (has(AV_PIX_FMT_YUV420P)) return AV_PIX_FMT_YUV420P;
  return f.empty() ? AV_PIX_FMT_YUV420P : f.front();
}

// A moving luma ramp: enough change per frame that an encoder cannot skip it.
void paint(AVFrame* f, int index) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
  const bool wide = d->comp[0].depth > 8;
  for (int p = 0; p < 4 && f->data[p]; ++p) {
    const int h = p == 0 ? f->height : AV_CEIL_RSHIFT(f->height, d->log2_chroma_h);
    for (int y = 0; y < h; ++y) {
      std::uint8_t* row = f->data[p] + static_cast<std::ptrdiff_t>(y) * f->linesize[p];
      if (p == 0) {
        if (wide) {
          auto* r16 = reinterpret_cast<std::uint16_t*>(row);
          for (int x = 0; x < f->width; ++x) r16[x] = static_cast<std::uint16_t>(((x + y + index * 8) & 1023) << 6);
        } else {
          for (int x = 0; x < f->width; ++x) row[x] = static_cast<std::uint8_t>(x + y + index * 4);
        }
      } else {
        std::memset(row, 0x80, static_cast<std::size_t>(f->linesize[p]));
      }
    }
  }
}

struct video_result {
  bool opened = false;
  std::string note;
  double fps = 0.0;
  AVPixelFormat fmt = AV_PIX_FMT_NONE;
};

video_result probe_video(const AVCodec* c, const video_case& vc) {
  video_result r;
  const bool prores = c->id == AV_CODEC_ID_PRORES;
  r.fmt = pick(sw_formats(c), vc.ten_bit && (c->id != AV_CODEC_ID_H264 || prores), prores);
  if (vc.ten_bit && c->id == AV_CODEC_ID_H264) {
    r.note = "n/a (H.264 10-bit not offered)";
    return r;
  }
  if (r.fmt == AV_PIX_FMT_NONE) {
    r.note = "no 10-bit input format";
    return r;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(c);
  ctx->width = vc.width;
  ctx->height = vc.height;
  ctx->pix_fmt = r.fmt;
  ctx->time_base = AVRational{1, vc.fps};
  ctx->framerate = AVRational{vc.fps, 1};
  ctx->bit_rate = vc.height > 1080 ? 40'000'000 : 12'000'000;
  ctx->gop_size = vc.fps * 2;
  ctx->max_b_frames = 0;
  if (avcodec_open2(ctx, c, nullptr) < 0) {
    avcodec_free_context(&ctx);
    r.note = "open failed (no such GPU / size / format here)";
    return r;
  }
  r.opened = true;
  AVFrame* f = av_frame_alloc();
  f->format = r.fmt;
  f->width = vc.width;
  f->height = vc.height;
  av_frame_get_buffer(f, 0);
  AVPacket* pkt = av_packet_alloc();
  int packets = 0;
  const auto t0 = std::chrono::steady_clock::now();
  bool failed = false;
  for (int i = 0; i <= kFrames && !failed; ++i) {
    AVFrame* in = nullptr;
    if (i < kFrames) {
      av_frame_make_writable(f);
      paint(f, i);
      f->pts = i;
      in = f;
    }
    if (avcodec_send_frame(ctx, in) < 0) failed = true;
    while (!failed) {
      const int rc = avcodec_receive_packet(ctx, pkt);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) {
        failed = true;
        break;
      }
      ++packets;
      av_packet_unref(pkt);
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (failed || packets == 0) {
    r.note = "encode failed after open";
  } else {
    r.fps = kFrames / secs;
  }
  av_packet_free(&pkt);
  av_frame_free(&f);
  avcodec_free_context(&ctx);
  return r;
}

void video_section() {
  std::printf("## Video encoders (%d synthetic frames each, no mux)\n\n", kFrames);
  std::printf("| Encoder | Family | Case | Result | Input | fps | x real time |\n|---|---|---|---|---|---|---|\n");
  std::vector<const char*> names;
  for (hw::codec k : {hw::codec::h264, hw::codec::hevc}) {
    for (const char* n : hw::candidates(k)) names.push_back(n);
  }
  for (const char* n : kExtraVideo) names.push_back(n);
  if (names.empty()) std::printf("| (none: the portable build has no encode port) | | | | | | |\n");
  for (const char* name : names) {
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (c == nullptr) {
      std::printf("| `%s` | %s | all | not in build | | | |\n", name, hw::family_label(name));
      continue;
    }
    if (!licence_ok(c)) {
      std::printf("| `%s` | %s | all | refused: software encoder (docs/design/11) | | | |\n", name,
                  hw::family_label(name));
      continue;
    }
    for (const video_case& vc : kCases) {
      const video_result r = probe_video(c, vc);
      if (r.fps > 0.0) {
        std::printf("| `%s` | %s | %s | ok | %s | %.0f | %.1fx |\n", name, hw::family_label(name), vc.label,
                    av_get_pix_fmt_name(r.fmt), r.fps, r.fps / vc.fps);
      } else {
        std::printf("| `%s` | %s | %s | %s | | | |\n", name, hw::family_label(name), vc.label,
                    r.note.c_str());
      }
    }
  }
  std::printf("\n");
}

void audio_section() {
  std::printf("## Audio encoders (48 kHz stereo open)\n\n| Encoder | In build | Opens | Kind |\n|---|---|---|---|\n");
  for (const char* name : kAudioEncoders) {
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (c == nullptr) {
      std::printf("| `%s` | no | | |\n", name);
      continue;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(c);
    ctx->sample_rate = 48000;
    av_channel_layout_default(&ctx->ch_layout, 2);
    ctx->time_base = AVRational{1, 48000};
    ctx->bit_rate = c->id == AV_CODEC_ID_AAC || c->id == AV_CODEC_ID_OPUS ? 192'000 : 0;
    // The encoder's first sample format.
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n) >= 0 && cfg && n > 0) {
      ctx->sample_fmt = static_cast<const AVSampleFormat*>(cfg)[0];
    }
#else
    if (c->sample_fmts) ctx->sample_fmt = c->sample_fmts[0];
#endif
    AVDictionary* opts = nullptr;
    if (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL) av_dict_set(&opts, "strict", "experimental", 0);
    const bool ok = avcodec_open2(ctx, c, &opts) >= 0;
    av_dict_free(&opts);
    // AudioToolbox (_at) and Media Foundation (_mf) are the OS's encoders,
    // whatever FFmpeg's capability flags say.
    const std::size_t len = std::strlen(name);
    const bool os = len > 3 && (std::strcmp(name + len - 3, "_at") == 0 || std::strcmp(name + len - 3, "_mf") == 0);
    const char* kind = os || is_hardware(c) ? "OS encoder" : "bundled software";
    if (std::strcmp(name, "aac") == 0) kind = "bundled software AAC: docs/design/11 says prefer the OS encoder";
    if (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL) kind = "bundled software, experimental";
    std::printf("| `%s` | yes | %s | %s |\n", name, ok ? "yes" : "no", kind);
    avcodec_free_context(&ctx);
  }
  std::printf("\n");
}

void filter_section() {
  std::printf("## Filters in this FFmpeg build\n\n| Filter | Present | Use | Verdict |\n|---|---|---|---|\n");
  for (const filter_row& f : kFilters) {
    const bool present = avfilter_get_by_name(f.name) != nullptr;
    const char* verdict = f.must_be_absent ? (present ? "**FAIL: GPL build**" : "ok (absent, LGPL build)")
                                           : (present ? "ok" : "missing");
    std::printf("| `%s` | %s | %s | %s |\n", f.name, present ? "yes" : "no", f.use, verdict);
  }
  std::printf("\n");
}

void hwdevice_section() {
  std::printf("## Hardware device types (decode for preview)\n\n");
  AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
  bool any = false;
  while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
    AVBufferRef* dev = nullptr;
    const bool ok = av_hwdevice_ctx_create(&dev, t, nullptr, nullptr, 0) >= 0;
    std::printf("- `%s`: %s\n", av_hwdevice_get_type_name(t), ok ? "creates" : "built in, no device here");
    av_buffer_unref(&dev);
    any = true;
  }
  if (!any) std::printf("- none\n");
  std::printf("\n");
}

}  // namespace

int main() {
  av_log_set_level(AV_LOG_QUIET);
  std::printf("# encprobe (docs/design/21 spike S1)\n\n");
  std::printf("libavcodec %s · libavfilter %s\n\n", AV_STRINGIFY(LIBAVCODEC_VERSION),
              AV_STRINGIFY(LIBAVFILTER_VERSION));
  video_section();
  audio_section();
  filter_section();
  hwdevice_section();
  return 0;
}
