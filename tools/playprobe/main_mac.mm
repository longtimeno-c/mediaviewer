// SPDX-License-Identifier: GPL-2.0-or-later
// playprobe -- headless PR 19 pipeline check on a real Mac.
//
// Opens a clip on the system MTLDevice, plays it against a 60 Hz timer (the
// render thread's job in the lab, minus the window), releases every frame it
// acquires, and prints what actually happened: which decoder ran, the presenter
// counters, and the A/V drift slope. It exists because "it plays" proves
// nothing: a silent software fallback, a starving ring and a drifting clock all
// still put pictures on screen.
//
//   playprobe CLIP [--seconds N] [--refresh HZ] [--seek-at S --seek-to S]
//             [--pause-at S --resume-at S] [--rate R] [--expect hw|sw] [--mute] [--trace]
//
// Exit 0 when the run was clean (and matched --expect), 1 otherwise.
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "player/media_source.h"

namespace {

double now_s() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

const char* decoder_name(mv::player::decoder_kind k) {
  switch (k) {
    case mv::player::decoder_kind::none: return "none";
    case mv::player::decoder_kind::d3d11va: return "d3d11va";
    case mv::player::decoder_kind::software: return "software";
    case mv::player::decoder_kind::videotoolbox: return "videotoolbox";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: playprobe CLIP [--seconds N] [--refresh HZ] [--seek-at S --seek-to S] "
                         "[--pause-at S --resume-at S] [--rate R] [--expect hw|sw] [--mute] [--trace]\n");
    return 2;
  }
  const char* path = argv[1];
  double seconds = 5.0, refresh = 60.0, seek_at = -1, seek_to = 0, pause_at = -1, resume_at = -1,
         rate = 1.0;
  std::string expect;
  bool trace = false;
  bool mute = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--trace") || !std::strcmp(argv[i], "--mute")) {
      if (!std::strcmp(argv[i], "--mute")) mute = true; else trace = true;
      // consume the flag by moving the last arg into its place
      for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
      --argc;
      --i;
    }
  }
  for (int i = 2; i + 1 < argc; i += 2) {
    const char* k = argv[i];
    const char* v = argv[i + 1];
    if (!std::strcmp(k, "--seconds")) seconds = std::atof(v);
    else if (!std::strcmp(k, "--refresh")) refresh = std::atof(v);
    else if (!std::strcmp(k, "--seek-at")) seek_at = std::atof(v);
    else if (!std::strcmp(k, "--seek-to")) seek_to = std::atof(v);
    else if (!std::strcmp(k, "--pause-at")) pause_at = std::atof(v);
    else if (!std::strcmp(k, "--resume-at")) resume_at = std::atof(v);
    else if (!std::strcmp(k, "--rate")) rate = std::atof(v);
    else if (!std::strcmp(k, "--expect")) expect = v;
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "playprobe: no MTLDevice\n");
      return 2;
    }
    auto opened = mv::player::open_media(path, (__bridge void*)device);
    if (!opened) {
      std::fprintf(stderr, "playprobe: open_media failed (%s)\n", mv::status_name(opened.error()));
      return 1;
    }
    mv::player::media_source* src = opened.value();
    const auto info = src->info();
    if (rate != 1.0) src->set_rate(rate);
    // Muted output still runs the Core Audio callback (gain 0), so the clock under
    // test is the real one -- it just is not audible on someone's desk.
    if (mute) src->set_muted(true);
    src->play();

    const double interval = 1.0 / refresh;
    const auto vblank_ns = static_cast<mv::player::time_ns>(interval * 1e9);
    std::uint64_t acquired = 0;
    std::uint32_t width = 0, height = 0;
    bool ten_bit = false;
    bool did_seek = false, did_pause = false, did_resume = false;
    const double t0 = now_s();
    double next = t0;
    double next_trace = 0.0;
    while (now_s() - t0 < seconds) {
      const double t = now_s() - t0;
      if (seek_at >= 0 && !did_seek && t >= seek_at) {
        src->seek(static_cast<mv::player::time_ns>(seek_to * 1e9), true);
        did_seek = true;
      }
      if (pause_at >= 0 && !did_pause && t >= pause_at) { src->pause(); did_pause = true; }
      if (resume_at >= 0 && !did_resume && t >= resume_at) { src->play(); did_resume = true; }
      if (trace && t >= next_trace) {
        next_trace += 0.25;
        const auto s = src->stats();
        std::printf("t=%5.2f clock=%7.3f shown_pts=%7.3f err=%+7.1f ms dropped=%llu presented=%llu "
                    "silence=%llu\n",
                    t, src->position_ns() / 1e9, s.video_pts_ns / 1e9,
                    (s.video_pts_ns - src->position_ns()) / 1e6,
                    (unsigned long long)s.counters.dropped_late,
                    (unsigned long long)s.counters.presented,
                    (unsigned long long)s.counters.silence_fills);
      }
      if (src->needs_present()) {
        if (auto* frame = src->acquire_frame(1, vblank_ns)) {
          ++acquired;
          width = frame->width;
          height = frame->height;
          ten_bit = frame->ten_bit;
          src->release_frame(frame);
        }
      }
      next += interval;
      // Absolute deadline via mach_wait_until: tens of microseconds of jitter,
      // like a display link. sleep_for jitters by milliseconds, and the presenter
      // shows a 30 fps frame only if a tick lands in its one-vblank window, so a
      // sloppy timer reads as "dropped late" when the pipeline is fine.
      const double sleep = next - now_s();
      if (sleep > 0) {
        static const mach_timebase_info_data_t tb = [] {
          mach_timebase_info_data_t t{};
          mach_timebase_info(&t);
          return t;
        }();
        const double ns = sleep * 1e9;
        mach_wait_until(mach_absolute_time() + static_cast<std::uint64_t>(ns * tb.denom / tb.numer));
      } else if (sleep < -0.25) {
        next = now_s();  // machine stalled; do not spiral
      }
    }

    const auto st = src->stats();
    const double elapsed = now_s() - t0;
    std::printf("clip        %s\n", path);
    std::printf("stream      %ux%u %s  codec=%s  fps=%.2f  duration=%.2fs  audio_tracks=%u\n",
                info.video.width, info.video.height, info.video.ten_bit ? "10-bit" : "8-bit",
                info.video.codec_name, info.video.frame_rate, info.duration_ns / 1e9, info.audio_tracks);
    std::printf("decoder     %s\n", decoder_name(st.decoder));
    std::printf("frames      acquired=%llu (%.1f/s) last=%ux%u %s\n", (unsigned long long)acquired,
                acquired / elapsed, width, height, ten_bit ? "P010" : "NV12");
    std::printf("clock       audio_master=%d fallback=%d position=%.2fs\n", st.audio_master ? 1 : 0,
                (int)st.fallback, src->position_ns() / 1e9);
    std::printf("presenter   presented=%llu dropped_late=%llu held_cadence=%llu held_starved=%llu "
                "silence_fills=%llu\n",
                (unsigned long long)st.counters.presented, (unsigned long long)st.counters.dropped_late,
                (unsigned long long)st.counters.held_cadence, (unsigned long long)st.counters.held_starved,
                (unsigned long long)st.counters.silence_fills);
    std::printf("err_ms      mean=%.2f p50=%.2f p99=%.2f min=%.2f max=%.2f  slope=%.3f ms/min\n",
                st.err_ms_mean, st.err_ms_p50, st.err_ms_p99, st.err_ms_min, st.err_ms_max,
                st.drift_slope_ms_per_min);
    std::printf("diagnostics decode_errors=%llu surface_waits=%llu ring_backpressure=%llu "
                "pos_discontinuities=%llu host_gaps=%llu\n",
                (unsigned long long)st.decode_errors, (unsigned long long)st.surface_waits,
                (unsigned long long)st.ring_backpressure,
                (unsigned long long)st.position_discontinuities, (unsigned long long)st.host_clock_gaps);

    const bool hw = st.decoder == mv::player::decoder_kind::videotoolbox ||
                    st.decoder == mv::player::decoder_kind::d3d11va;
    bool ok = acquired > 0 && st.decode_errors == 0 && st.surface_waits == 0;
    if (expect == "hw" && !hw) ok = false;
    if (expect == "sw" && st.decoder != mv::player::decoder_kind::software) ok = false;
    std::printf("result      %s\n", ok ? "OK" : "FAIL");
    mv::player::close_media(src);
    return ok ? 0 : 1;
  }
}
