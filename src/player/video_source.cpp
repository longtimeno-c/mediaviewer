// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - video_source implementation: opens the container, starts the demux
// and decode threads, and hands frames to the render thread.
//
// OWNER: mediaviewer-48 (5a).
#include <cstdio>

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {

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

class ffmpeg_video_source final : public video_source {
 public:
  ~ffmpeg_video_source() override { stop(); }

  [[nodiscard]] expected open(const char* utf8_path, void* device) {
    if (!utf8_path || !device) return err(status::invalid_arg);
    pipe_.device = static_cast<ID3D11Device*>(device);
    pipe_.device_owner = pipe_.device;

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, utf8_path, nullptr, nullptr) < 0) return err(status::io);
    pipe_.format.reset(format);
    if (avformat_find_stream_info(pipe_.format.get(), nullptr) < 0) return err(status::corrupt);

    pipe_.video_stream =
        av_find_best_stream(pipe_.format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (pipe_.video_stream < 0) return err(status::unsupported_format);
    // Audio uses the same demux timeline as video.
    pipe_.audio_stream =
        av_find_best_stream(pipe_.format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

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
    // use-after-free plan/05 warns about at shutdown.
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
