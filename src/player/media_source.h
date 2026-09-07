// SPDX-License-Identifier: GPL-2.0-or-later
// One open clip: demux + video decode + audio + clock + transport.
//
// This is the type the ABI and the shell talk to. plan/05 keeps everything
// behind it so the two rejected options (libmpv, Media Foundation) stay
// droppable — specifically, D2's escape hatch is implementing this interface
// with IMFMediaEngine if PR 5b's clock overruns.
#pragma once

#include <cstdint>

#include "core/result.h"
#include "player/av_clock.h"
#include "player/audio_sink.h"
#include "player/video_source.h"

namespace mv::player {

enum class play_state : std::uint8_t { stopped, playing, paused, ended };

struct media_info {
  video_stream_info video{};
  time_ns       duration_ns  = 0;
  std::uint32_t audio_tracks = 0;
  std::uint32_t video_tracks = 0;
  bool          has_audio    = false;
};

// All methods are [any-thread][no-block] unless marked otherwise: they post
// intent to the demux/decode threads and return. Nothing here decodes inline.
class media_source {
 public:
  virtual ~media_source() = default;

  [[nodiscard]] virtual media_info  info()  const noexcept = 0;
  [[nodiscard]] virtual play_state  state() const noexcept = 0;
  [[nodiscard]] virtual bool needs_present() const noexcept { return state() == play_state::playing; }
  [[nodiscard]] virtual time_ns position_ns() const noexcept = 0;

  virtual void play()  noexcept = 0;
  virtual void pause() noexcept = 0;

  // plan/05 two-mode seek. exact=false -> nearest keyframe, AVSEEK_FLAG_BACKWARD,
  // no full decode: what the scrubber drags on. exact=true -> decode forward from
  // that keyframe to the frame asked for: what a release or a step lands on.
  // Both flush the decoders and bump the generation so in-flight pre-seek frames
  // are discarded.
  virtual void seek(time_ns pts_ns, bool exact) noexcept = 0;

  // Paused only. +1 / -1. Back-step is a seek to the prior keyframe then decode
  // forward to n-1; it is not a cheap operation and the UI should not pretend
  // otherwise.
  virtual void step(int frames) noexcept = 0;

  virtual void set_rate(double rate) noexcept = 0;   // 0.25 .. 4.0, pitch-corrected
  virtual void set_volume(float volume) noexcept = 0;
  virtual void set_muted(bool muted) noexcept = 0;
  virtual void select_audio_track(std::uint32_t index) noexcept = 0;

  // A-B loop. b_ns < 0 clears.
  virtual void set_loop(time_ns a_ns, time_ns b_ns) noexcept = 0;

  // [render-thread] Wait-free. Null when nothing new is due — the caller keeps
  // showing what it has. Frames at a stale generation are never returned.
  [[nodiscard]] virtual video_frame* acquire_frame(std::uint32_t generation,
                                                   time_ns vblank_ns) noexcept = 0;
  virtual void release_frame(video_frame* frame) noexcept = 0;

  [[nodiscard]] virtual clock_stats stats() const noexcept = 0;
};

// Opens a clip with FFmpeg. `device` is our ID3D11Device, passed as void* so
// this header stays free of d3d11.h (D9 / check-hostable-core.ps1); the
// implementation is player/hwdecode_win.cpp and it QIs what it needs.
[[nodiscard]] result<media_source*> open_media(const char* utf8_path, void* device);
void close_media(media_source* source) noexcept;

}  // namespace mv::player
