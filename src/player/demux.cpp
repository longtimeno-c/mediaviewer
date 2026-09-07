// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - libavformat demux into bounded packet queues (~2 s).
//
// OWNER: mediaviewer-48 (5a).
#include <chrono>

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {

time_ns pts_to_ns(std::int64_t pts, AVRational time_base, time_ns start_time_ns,
                  time_ns fallback) noexcept {
  if (pts == AV_NOPTS_VALUE || time_base.den == 0) return fallback;
  const AVRational ns{1, 1'000'000'000};
  // plan/05 / the PR 5 contract: PTS is stream-relative. A container start_time
  // that is not subtracted here reads downstream as a constant A/V offset and
  // gets blamed on the clock, which is a long way from where the bug is.
  return static_cast<time_ns>(av_rescale_q(pts, time_base, ns)) - start_time_ns;
}

void run_demux_thread(video_pipeline& pipe) noexcept {
  packet_ptr packet(av_packet_alloc());
  if (!packet) return;
  std::uint32_t generation = pipe.generation.load();
  bool drained = false;
  auto enqueue = [&](packet_queue& queue, packet_ptr& item) {
    while (!pipe.stopping.load()) {
      if (pipe.seek_request_ns.load() >= 0) return false;
      if (queue.try_push(item, generation)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  while (!pipe.stopping.load()) {
    const time_ns want = pipe.seek_request_ns.exchange(-1);
    if (want >= 0) {
      const auto next_generation = pipe.generation.load();
      const auto target = av_rescale_q(want + pipe.start_time_ns,
                                      AVRational{1, 1'000'000'000}, pipe.time_base);
      if (av_seek_frame(pipe.format.get(), pipe.video_stream, target, AVSEEK_FLAG_BACKWARD) < 0)
        pipe.decode_errors.fetch_add(1);
      pipe.video_packets.flush();
      pipe.audio_packets.flush();
      generation = next_generation;
      pipe.eof.store(false);
      pipe.video_done.store(false);
      drained = false;
    }
    if (drained) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const int rc = av_read_frame(pipe.format.get(), packet.get());
    if (rc < 0) {
      if (rc == AVERROR(EAGAIN)) continue;
      if (rc != AVERROR_EOF) pipe.decode_errors.fetch_add(1);
      packet_ptr end_video, end_audio;
      if (!enqueue(pipe.video_packets, end_video)) continue;
      if (pipe.audio_stream >= 0 && !enqueue(pipe.audio_packets, end_audio)) continue;
      pipe.eof.store(true);
      drained = true;
      continue;
    }
    packet_queue* queue = nullptr;
    if (packet->stream_index == pipe.video_stream) queue = &pipe.video_packets;
    else if (packet->stream_index == pipe.selected_audio.load()) queue = &pipe.audio_packets;
    if (queue) {
      packet_ptr item(av_packet_alloc());
      if (!item) break;
      av_packet_move_ref(item.get(), packet.get());
      (void)enqueue(*queue, item);
    }
    av_packet_unref(packet.get());
  }
  pipe.video_packets.stop();
  pipe.audio_packets.stop();
}

}  // namespace mv::player
