// SPDX-License-Identifier: GPL-2.0-or-later
// Decode and resample on a worker; only fixed float blocks reach the audio pump.
#include "player/video_internal.h"
#include "player/transport.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/samplefmt.h>
}
namespace mv::player {
namespace {
struct audio_filter {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* input = nullptr;
  AVFilterContext* output = nullptr;
  ~audio_filter() { reset(); }
  void reset() { avfilter_graph_free(&graph); input = output = nullptr; }
  bool create(const AVFrame* frame, AVRational time_base, double rate) {
    reset();
    graph = avfilter_graph_alloc();
    if (!graph) return false;
    char layout[128]{}, args[512]{};
    av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
    std::snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                  time_base.num, time_base.den, frame->sample_rate,
                  av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)), layout);
    if (avfilter_graph_create_filter(&input, avfilter_get_by_name("abuffer"), "in", args, nullptr, graph) < 0 ||
        avfilter_graph_create_filter(&output, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph) < 0) return false;
    AVFilterInOut* in = avfilter_inout_alloc();
    AVFilterInOut* out = avfilter_inout_alloc();
    if (!in || !out) { avfilter_inout_free(&in); avfilter_inout_free(&out); return false; }
    in->name = av_strdup("out"); in->filter_ctx = output; in->pad_idx = 0;
    out->name = av_strdup("in"); out->filter_ctx = input; out->pad_idx = 0;
    // aresample uses libswresample; atempo preserves pitch over the full 0.25-4x range.
    std::string chain = "aresample=48000,aformat=sample_fmts=flt:channel_layouts=stereo";
    const auto tempo = build_atempo_chain(rate);
    if (!tempo.empty()) chain += "," + tempo;
    int rc = avfilter_graph_parse_ptr(graph, chain.c_str(), &in, &out, nullptr);
    avfilter_inout_free(&in); avfilter_inout_free(&out);
    return rc >= 0 && avfilter_graph_config(graph, nullptr) >= 0;
  }
};
}
void run_audio_decode_thread(video_pipeline& pipe) noexcept {
  codec_ctx_ptr codec;
  frame_ptr decoded(av_frame_alloc()), filtered(av_frame_alloc());
  if (!decoded || !filtered) return;
  audio_filter filter;
  std::uint32_t generation = 0;
  int stream_index = -1;
  double rate = 1;
  time_ns next_pts = 0;
  bool seeded = false;
  while (!pipe.stopping.load()) {
    packet_ptr packet;
    std::uint32_t packet_generation = 0;
    if (!pipe.audio_packets.try_pop(packet, &packet_generation)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;
    }
    if (packet_generation != pipe.generation.load()) continue;
    const int selected = pipe.selected_audio.load();
    if (selected < 0) continue;
    AVStream* stream = pipe.format->streams[selected];
    if (selected != stream_index) {
      const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
      codec.reset(decoder ? avcodec_alloc_context3(decoder) : nullptr);
      if (!codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
          avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        pipe.decode_errors.fetch_add(1); continue;
      }
      codec->pkt_timebase = stream->time_base;
      stream_index = selected;
      generation = 0;
    }
    if (generation != packet_generation) {
      avcodec_flush_buffers(codec.get()); filter.reset(); seeded = false;
      generation = packet_generation; rate = pipe.rate.load();
    }
    if (avcodec_send_packet(codec.get(), packet.get()) < 0) continue;
    for (;;) {
      int rc = avcodec_receive_frame(codec.get(), decoded.get());
      if (rc == AVERROR(EAGAIN)) break;
      if (rc < 0 && rc != AVERROR_EOF) { pipe.decode_errors.fetch_add(1); break; }
      if (rc != AVERROR_EOF) {
        if (!filter.graph && !filter.create(decoded.get(), stream->time_base, rate)) {
          pipe.decode_errors.fetch_add(1); break;
        }
        if (!seeded) {
          next_pts = pts_to_ns(decoded->best_effort_timestamp, stream->time_base, pipe.start_time_ns, 0);
          seeded = true;
        }
      }
      if (!filter.graph) break;
      const int fed = av_buffersrc_add_frame_flags(filter.input,
          rc == AVERROR_EOF ? nullptr : decoded.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
      av_frame_unref(decoded.get());
      if (fed < 0) break;
      while (av_buffersink_get_frame(filter.output, filtered.get()) >= 0) {
        const auto* samples = reinterpret_cast<const float*>(filtered->data[0]);
        int offset = 0;
        while (offset < filtered->nb_samples && !pipe.stopping.load() && generation == pipe.generation.load()) {
          audio_block block;
          block.channels = 2; block.generation = generation;
          block.frames = static_cast<std::uint32_t>(std::min(1024, filtered->nb_samples - offset));
          block.pts_ns = next_pts;
          next_pts += static_cast<time_ns>(static_cast<double>(block.frames) * 1'000'000'000.0 * rate / 48000.0);
          std::memcpy(block.samples, samples + offset * 2, block.frames * 2 * sizeof(float));
          offset += static_cast<int>(block.frames);
          const auto target = pipe.audio_target_ns.load();
          if (target >= 0 && next_pts <= target) continue;
          if (target > block.pts_ns) {
            const auto trim = std::min(block.frames, static_cast<std::uint32_t>(
                static_cast<double>(target - block.pts_ns) * 48000.0 / (1'000'000'000.0 * rate)));
            block.frames -= trim; block.pts_ns = target;
            std::memmove(block.samples, block.samples + trim * 2, block.frames * 2 * sizeof(float));
          }
          while (!pipe.clock.submit(block) && !pipe.stopping.load() && generation == pipe.generation.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        av_frame_unref(filtered.get());
      }
      if (rc == AVERROR_EOF) break;
    }
  }
}
}  // namespace mv::player
