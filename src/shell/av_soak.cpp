// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/av_soak.h"
#include "gfx/device.h"
#include "player/media_source.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
namespace mv::shell {
int run_av_soak(const av_soak_options& options) {
  if (!options.clip_utf8 || !options.csv_out_utf8 || options.seconds == 0) return 2;
  gfx::device device;
  if (!device.create(nullptr)) return 2;
  auto opened = player::open_media(options.clip_utf8, device.d3d());
  if (!opened) return 2;
  std::unique_ptr<player::media_source, decltype(&player::close_media)> source(opened.value(), player::close_media);
  std::ofstream csv(std::filesystem::path(reinterpret_cast<const char8_t*>(options.csv_out_utf8)));
  if (!csv) return 2;
  csv << "elapsed_s,position_ns,audio_master,error_p50_ms,error_p99_ms,slope_ms_min,presented,dropped,cadence,starved,rebuilds,discontinuities,host_gaps\n";
  source->play();
  const auto start = std::chrono::steady_clock::now();
  auto deadline = start;
  unsigned next_sample = 0;
  std::uint64_t frames = 0;
  player::clock_stats stats;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - start).count();
    if (elapsed >= options.seconds) break;
    if (auto* frame = source->acquire_frame(1, 16'666'667)) { ++frames; source->release_frame(frame); }
    stats = source->stats();
    if (elapsed >= next_sample) {
      ++next_sample;
      csv << elapsed << ',' << source->position_ns() << ',' << stats.audio_master << ','
          << stats.err_ms_p50 << ',' << stats.err_ms_p99 << ',' << stats.drift_slope_ms_per_min << ','
          << frames << ',' << stats.counters.dropped_late << ',' << stats.counters.held_cadence << ','
          << stats.counters.held_starved << ',' << stats.counters.device_rebuilds << ','
          << stats.position_discontinuities << ',' << stats.host_clock_gaps << '\n';
      csv.flush();
    }
    if (source->state() == player::play_state::ended) return 3; // A short clip cannot prove a long soak.
    deadline += std::chrono::nanoseconds(16'666'667);
    std::this_thread::sleep_until(deadline); // Harness worker, never the canvas present loop.
  }
  if (frames == 0 || stats.position_discontinuities || stats.host_clock_gaps) return 1;
  if (source->info().has_audio && !stats.audio_master) return 1;
  // Diagnostic runs are useful, but never label them a 30-minute verification.
  if (options.seconds < 1800) return 4;
  return std::abs(stats.drift_slope_ms_per_min) <= 1.0 ? 0 : 1;
}
} // namespace mv::shell
