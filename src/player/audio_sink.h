// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b — the audio endpoint, behind a portable interface.
//
// D9 / plan/15: the WASAPI implementation is audio_win.cpp and NOTHING else.
// A Core Audio host must be able to replace that one file without touching the
// clock. That is why no <audioclient.h> / <mmdeviceapi.h> appears here, and why
// tools/check-hostable-core.ps1 bans those headers everywhere but *_win.cpp.
#pragma once

#include <cstdint>

#include "core/result.h"
#include "player/video_source.h"  // time_ns

namespace mv::player {

// Why the audio clock stopped being the master, when it did.
enum class clock_fallback_reason : std::uint8_t {
  none = 0,
  no_audio_track,
  device_open_failed,
  device_lost,
};

struct audio_endpoint_info {
  std::uint32_t sample_rate   = 0;
  std::uint32_t channels      = 0;
  std::uint32_t buffer_frames = 0;
  std::uint32_t period_frames = 0;  // shared-mode period
};

// Shared-mode render client. Float32 interleaved is the only format that
// crosses this line; swresample converts to it upstream.
class audio_sink {
 public:
  virtual ~audio_sink() = default;

  [[nodiscard]] virtual expected open(std::uint32_t sample_rate,
                                      std::uint32_t channels) = 0;
  virtual void close() noexcept = 0;

  [[nodiscard]] virtual audio_endpoint_info info() const noexcept = 0;

  // [audio-thread] Returns frames accepted; may be < `frames` when the endpoint
  // buffer is full. Never blocks on decode (CLAUDE.md rule 1).
  [[nodiscard]] virtual std::uint32_t write(const float* interleaved,
                                            std::uint32_t frames) noexcept = 0;

  // [any-thread] Position derived from samples ACTUALLY PLAYED — the render
  // client's own position, minus frames still queued but unplayed. NOT a wall
  // clock: a wall clock produces a slow drift ramp that reads as a decode bug.
  //
  // `out_discontinuity` increments whenever the underlying position jumps
  // (device change, stream reset). A single jump poisons a regression slope, so
  // the overlay reports it rather than averaging over it — same honesty as
  // gfx::pacer's statistics_discontinuities.
  [[nodiscard]] virtual time_ns played_ns(std::uint64_t* out_discontinuity) const noexcept = 0;

  // [any-thread] Set by the device-change notification. The clock owner rebuilds
  // the client and re-seeds WITHOUT interrupting video (verify line).
  [[nodiscard]] virtual bool device_changed() const noexcept = 0;

  virtual void set_volume(float volume) noexcept = 0;  // 0.0 .. 1.0
  virtual void set_muted(bool muted) noexcept = 0;
};

// Created by audio_win.cpp on Windows. The only place a host port is selected.
[[nodiscard]] result<audio_sink*> create_audio_sink();
void destroy_audio_sink(audio_sink* sink) noexcept;

}  // namespace mv::player
