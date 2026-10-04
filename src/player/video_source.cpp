// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5a - video_source implementation: opens the container, starts the demux
// and decode threads, and hands frames to the render thread.
//
// OWNER: mediaviewer-48 (5a).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/trace.h"
#include "player/audio_card.h"
#include "player/video_internal.h"

namespace mv::player {

int stream_rotation_degrees(const AVStream* stream) noexcept {
  if (!stream || !stream->codecpar) return 0;
  const AVCodecParameters* p = stream->codecpar;
  const AVPacketSideData* sd = av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data,
                                                       AV_PKT_DATA_DISPLAYMATRIX);
  if (sd == nullptr || sd->size < 9 * sizeof(std::int32_t)) return 0;
  // av_display_rotation_get is counter-clockwise; a player turns the other way
  // (the same convention as edit/clip_common.cpp's stream_rotation).
  const double ccw = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(sd->data));
  if (ccw != ccw) return 0;  // NaN: a degenerate matrix
  int cw = static_cast<int>(std::lround(-ccw / 90.0)) * 90;
  cw %= 360;
  if (cw < 0) cw += 360;
  return cw;
}

gfx::colour_desc colour_from_stream(int avcol_space, int avcol_primaries, int avcol_trc,
                                    int avcol_range, int bit_depth) noexcept {
  gfx::colour_desc out;

  switch (avcol_space) {
    case AVCOL_SPC_BT709:      out.matrix = gfx::colour_matrix::bt709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:  out.matrix = gfx::colour_matrix::bt601; break;
    case AVCOL_SPC_SMPTE240M:  out.matrix = gfx::colour_matrix::smpte240m; break;
    case AVCOL_SPC_BT2020_NCL: out.matrix = gfx::colour_matrix::bt2020_ncl; break;
    default:                   out.matrix = gfx::colour_matrix::unspecified; break;
  }

  switch (avcol_primaries) {
    case AVCOL_PRI_BT709:     out.primaries = gfx::colour_primaries::bt709; break;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M: out.primaries = gfx::colour_primaries::bt601_525; break;
    case AVCOL_PRI_BT470BG:   out.primaries = gfx::colour_primaries::bt601_625; break;
    case AVCOL_PRI_BT2020:    out.primaries = gfx::colour_primaries::bt2020; break;
    default:                  out.primaries = gfx::colour_primaries::unspecified; break;
  }

  switch (avcol_trc) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:   out.transfer = gfx::colour_transfer::bt709; break;
    case AVCOL_TRC_IEC61966_2_1:out.transfer = gfx::colour_transfer::srgb; break;
    case AVCOL_TRC_SMPTE2084:   out.transfer = gfx::colour_transfer::smpte2084; break;
    case AVCOL_TRC_ARIB_STD_B67:out.transfer = gfx::colour_transfer::arib_std_b67; break;
    default:                    out.transfer = gfx::colour_transfer::unspecified; break;
  }

  switch (avcol_range) {
    case AVCOL_RANGE_MPEG: out.range = gfx::colour_range::limited; break;
    case AVCOL_RANGE_JPEG: out.range = gfx::colour_range::full; break;
    default:               out.range = gfx::colour_range::unspecified; break;
  }

  out.bit_depth = bit_depth > 8 ? std::uint8_t{10} : std::uint8_t{8};
  return out;
}

namespace {

// The best video stream that moves. Cover art is exposed by FFmpeg as a video
// stream with the attached-picture disposition; it is a still, not a timeline,
// and seeking on it never lands (docs/plans/audio-and-documents.md §2.2).
int find_moving_video(AVFormatContext* format) noexcept {
  const int best = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (best >= 0 && !(format->streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC)) return best;
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    const AVStream* s = format->streams[i];
    if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

frame_ptr rgba_frame(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba,
                     int stride) noexcept {
  frame_ptr frame(av_frame_alloc());
  if (!frame) return {};
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = static_cast<int>(width);
  frame->height = static_cast<int>(height);
  if (av_frame_get_buffer(frame.get(), 0) < 0) return {};
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(frame->data[0] + static_cast<std::size_t>(y) * frame->linesize[0],
                rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                static_cast<std::size_t>(width) * 4u);
  }
  // swscale's RGB -> NV12 default is BT.601 limited range; say so, so the
  // shader decodes it with the matrix it was encoded with.
  frame->colorspace = AVCOL_SPC_BT470BG;
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_IEC61966_2_1;
  return frame;
}

frame_ptr card_frame(audio_card_kind kind) noexcept {
  const audio_card card = make_audio_card(kind);
  return rgba_frame(card.width, card.height, card.rgba.data(), static_cast<int>(card.width) * 4);
}

// The file's cover art as RGBA, at most 2048 px on its long edge (it is shown
// fitted; a 6000 px scan only costs memory). Null when there is none or it
// does not decode — the caller falls back to the music card.
frame_ptr cover_art(AVFormatContext* format) noexcept {
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    AVStream* s = format->streams[i];
    if (!(s->disposition & AV_DISPOSITION_ATTACHED_PIC) || s->attached_pic.size <= 0) continue;
    const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (!codec) continue;
    codec_ctx_ptr dec(avcodec_alloc_context3(codec));
    if (!dec || avcodec_parameters_to_context(dec.get(), s->codecpar) < 0) continue;
    dec->thread_count = 1;
    if (avcodec_open2(dec.get(), codec, nullptr) < 0) continue;
    frame_ptr decoded(av_frame_alloc());
    if (!decoded || avcodec_send_packet(dec.get(), &s->attached_pic) < 0) continue;
    (void)avcodec_send_packet(dec.get(), nullptr);
    if (avcodec_receive_frame(dec.get(), decoded.get()) < 0) continue;
    if (decoded->width < 2 || decoded->height < 2) continue;

    int w = decoded->width, h = decoded->height;
    const int long_edge = std::max(w, h);
    if (long_edge > 2048) {
      w = static_cast<int>(static_cast<std::int64_t>(w) * 2048 / long_edge);
      h = static_cast<int>(static_cast<std::int64_t>(h) * 2048 / long_edge);
    }
    w = std::max(2, w & ~1);
    h = std::max(2, h & ~1);
    sws_ptr sws(sws_getContext(decoded->width, decoded->height,
                               static_cast<AVPixelFormat>(decoded->format), w, h,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) continue;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
    std::uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int stride[4] = {w * 4, 0, 0, 0};
    if (sws_scale(sws.get(), decoded->data, decoded->linesize, 0, decoded->height, dst, stride) <= 0)
      continue;
    return rgba_frame(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), rgba.data(),
                      w * 4);
  }
  return {};
}

class ffmpeg_video_source final : public video_source {
 public:
  ~ffmpeg_video_source() override { stop(); }

  [[nodiscard]] expected open(const char* utf8_path, void* device) {
    if (!utf8_path || !device) return err(status::invalid_arg);
#if defined(MV_DARWIN)
    pipe_.device = device;  // id<MTLDevice>, borrowed
#else
    pipe_.device = static_cast<ID3D11Device*>(device);
    pipe_.device_owner = pipe_.device;
#endif

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, utf8_path, nullptr, nullptr) < 0) return err(status::io);
    pipe_.format.reset(format);
    if (avformat_find_stream_info(pipe_.format.get(), nullptr) < 0) return err(status::corrupt);

    pipe_.video_stream = find_moving_video(pipe_.format.get());
    // Audio uses the same demux timeline as video.
    pipe_.audio_stream =
        av_find_best_stream(pipe_.format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (pipe_.video_stream < 0) {
      if (pipe_.audio_stream < 0) return err(status::unsupported_format);
      return open_audio_only();
    }
    pipe_.seek_stream = pipe_.video_stream;

    AVStream* stream = pipe_.format->streams[pipe_.video_stream];
    pipe_.time_base = stream->time_base;

    // Container start_time, subtracted from every PTS before anything else sees
    // it. MPEG-TS routinely carries a large one; a surviving offset reads as a
    // constant A/V error and gets blamed on 5b's clock.
    const AVRational ns{1, 1'000'000'000};
    pipe_.start_time_ns = 0;
    if (pipe_.format->start_time != AV_NOPTS_VALUE) {
      pipe_.start_time_ns = av_rescale_q(pipe_.format->start_time, AVRational{1, AV_TIME_BASE}, ns);
    } else if (stream->start_time != AV_NOPTS_VALUE) {
      pipe_.start_time_ns = av_rescale_q(stream->start_time, stream->time_base, ns);
    }

    MV_TRY_VOID(open_video_codec(pipe_, stream));

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pipe_.codec->pix_fmt);
    const int bit_depth = desc ? desc->comp[0].depth : 8;

    pipe_.colour = gfx::resolve_unspecified(
        colour_from_stream(pipe_.codec->colorspace, pipe_.codec->color_primaries,
                           pipe_.codec->color_trc, pipe_.codec->color_range, bit_depth),
        static_cast<std::uint32_t>(pipe_.codec->width),
        static_cast<std::uint32_t>(pipe_.codec->height));

    pipe_.info.width = static_cast<std::uint32_t>(pipe_.codec->width);
    pipe_.info.height = static_cast<std::uint32_t>(pipe_.codec->height);
    pipe_.info.ten_bit = bit_depth > 8;
    // A portrait phone clip: coded landscape, shown turned (video_source.h).
    pipe_.info.rotation = static_cast<std::uint32_t>(stream_rotation_degrees(stream));
    pipe_.info.start_time_ns = pipe_.start_time_ns;
    const AVRational fps = av_guess_frame_rate(pipe_.format.get(), stream, nullptr);
    pipe_.info.frame_rate = fps.den > 0 ? av_q2d(fps) : 0.0;
    pipe_.info.duration_ns =
        stream->duration != AV_NOPTS_VALUE
            ? av_rescale_q(stream->duration, stream->time_base, ns)
            : (pipe_.format->duration != AV_NOPTS_VALUE
                   ? av_rescale_q(pipe_.format->duration, AVRational{1, AV_TIME_BASE}, ns)
                   : 0);

    pipe_.selected_audio.store(pipe_.audio_stream);
    if (pipe_.audio_stream >= 0) {
      MV_TRY_VOID(pipe_.clock.start(48000, 2));
    } else pipe_.clock.start_host_only();
    pipe_.clock.set_paused(true);
    pipe_.clock.seeked(0, pipe_.generation.load());
    if (pipe_.audio_stream >= 0)
      pipe_.audio_thread = std::thread([this] { run_audio_decode_thread(pipe_); });
    pipe_.demux_thread = std::thread([this] { run_demux_thread(pipe_); });
    pipe_.decode_thread = std::thread([this] { run_video_decode_thread(pipe_); });
    return {};
  }

  // An MP3/M4A/M4P: the audio is the timeline, the picture a still
  // (docs/plans/audio-and-documents.md §2.2).
  [[nodiscard]] expected open_audio_only() {
    pipe_.audio_only = true;
    AVStream* audio = pipe_.format->streams[pipe_.audio_stream];
    pipe_.seek_stream = pipe_.audio_stream;
    pipe_.time_base = audio->time_base;
    const AVRational ns{1, 1'000'000'000};
    pipe_.start_time_ns = 0;
    if (pipe_.format->start_time != AV_NOPTS_VALUE) {
      pipe_.start_time_ns = av_rescale_q(pipe_.format->start_time, AVRational{1, AV_TIME_BASE}, ns);
    } else if (audio->start_time != AV_NOPTS_VALUE) {
      pipe_.start_time_ns = av_rescale_q(audio->start_time, audio->time_base, ns);
    }

    if (is_protected_audio(audio)) {
      // Shown, never played: no audio thread, a host clock, no duration, so
      // play ends at once instead of running a silent timer.
      pipe_.drm_protected = true;
      pipe_.audio_stream = -1;
      pipe_.still = card_frame(audio_card_kind::protected_);
    } else {
      pipe_.still = cover_art(pipe_.format.get());
      if (!pipe_.still) pipe_.still = card_frame(audio_card_kind::music);
    }
    if (!pipe_.still) return err(status::out_of_memory);

    pipe_.info.width = static_cast<std::uint32_t>(pipe_.still->width);
    pipe_.info.height = static_cast<std::uint32_t>(pipe_.still->height);
    pipe_.info.ten_bit = false;
    pipe_.info.start_time_ns = pipe_.start_time_ns;
    pipe_.info.frame_rate = 0.0;
    pipe_.info.decoder = decoder_kind::software;
    if (!pipe_.drm_protected) {
      pipe_.info.duration_ns =
          audio->duration != AV_NOPTS_VALUE
              ? av_rescale_q(audio->duration, audio->time_base, ns)
              : (pipe_.format->duration != AV_NOPTS_VALUE
                     ? av_rescale_q(pipe_.format->duration, AVRational{1, AV_TIME_BASE}, ns)
                     : 0);
    }
    const AVCodecDescriptor* desc = avcodec_descriptor_get(audio->codecpar->codec_id);
    std::snprintf(pipe_.info.codec_name, sizeof(pipe_.info.codec_name), "%s",
                  pipe_.drm_protected ? "fairplay" : (desc ? desc->name : "audio"));
    pipe_.colour = gfx::resolve_unspecified(
        colour_from_stream(pipe_.still->colorspace, pipe_.still->color_primaries,
                           pipe_.still->color_trc, pipe_.still->color_range, 8),
        pipe_.info.width, pipe_.info.height);

    pipe_.selected_audio.store(pipe_.audio_stream);
    if (pipe_.audio_stream >= 0) {
      MV_TRY_VOID(pipe_.clock.start(48000, 2));
    } else pipe_.clock.start_host_only();
    pipe_.clock.set_paused(true);
    pipe_.clock.seeked(0, pipe_.generation.load());
    if (pipe_.audio_stream >= 0)
      pipe_.audio_thread = std::thread([this] { run_audio_decode_thread(pipe_); });
    pipe_.demux_thread = std::thread([this] { run_demux_thread(pipe_); });
    pipe_.decode_thread = std::thread([this] { run_still_thread(pipe_); });
    return {};
  }

  void stop() noexcept {
    pipe_.stopping.store(true, std::memory_order_release);
    pipe_.video_packets.stop();
    pipe_.audio_packets.stop();
    if (pipe_.demux_thread.joinable()) pipe_.demux_thread.join();
    if (pipe_.decode_thread.joinable()) pipe_.decode_thread.join();
    if (pipe_.audio_thread.joinable()) pipe_.audio_thread.join();
    pipe_.clock.stop();
    // Only now is it safe to tear the ring down: both producers are joined, and
    // the render thread is the caller. Doing it in the other order is the
    // use-after-free docs/design/05 warns about at shutdown.
    pipe_.ring.destroy();
    pipe_.video_packets.clear();
  }

  [[nodiscard]] video_stream_info info() const noexcept override { return pipe_.info; }

  [[nodiscard]] video_frame* acquire(std::uint32_t generation,
                                     time_ns deadline_ns) noexcept override {
    return pipe_.ring.acquire(generation, deadline_ns);
  }

  void release(video_frame* frame) noexcept override { pipe_.ring.release(frame); }

  [[nodiscard]] bool peek_next_pts(time_ns* out_pts_ns) const noexcept override {
    return pipe_.ring.peek_next_pts(out_pts_ns);
  }

  void flush(std::uint32_t generation) noexcept override {
    // The bump is the whole mechanism: the decode thread sees it and calls
    // avcodec_flush_buffers, and every frame already queued fails its
    // generation test at acquire. No cross-thread queue surgery, which is what
    // keeps both rings single-producer.
    pipe_.generation.store(generation, std::memory_order_release);

  }

  [[nodiscard]] video_pipeline& pipeline() noexcept { return pipe_; }

 private:
  video_pipeline pipe_;
};

}  // namespace

result<video_source*> open_video_source(const char* utf8_path, void* device) {
  auto source = std::make_unique<ffmpeg_video_source>();
  MV_TRY_VOID(source->open(utf8_path, device));
  return static_cast<video_source*>(source.release());
}

void close_video_source(video_source* source) noexcept {
  delete static_cast<ffmpeg_video_source*>(source);
}

video_pipeline* pipeline_of(video_source* source) noexcept {
  return source ? &static_cast<ffmpeg_video_source*>(source)->pipeline() : nullptr;
}

}  // namespace mv::player
