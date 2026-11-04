// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's sound look as the player hears it
// (docs/plans/video-editor-look.md, slice 3): a gain on every decoded sample,
// from the edit's kept ranges, a volume (0..2, 0 = mute) and fades on the
// program's start and end. A sample's gain is found from its source time:
// in a kept range its program time decides the fades; in a cut (the host
// jumps over those) it is the volume alone. The export does the same on the
// program's own sound (edit/clip_look_run.cpp).
//
// Portable, no I/O (D9).
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "core/keep_ranges.h"

namespace mv {

class sound_envelope {
 public:
  // `pairs_ns`: the kept (in, out) source ranges, in order. false (and off)
  // for ranges keep_ranges refuses. An empty list with volume 1 and no fades
  // is off: every gain is 1.
  bool set(std::span<const std::int64_t> pairs_ns, float volume, std::int64_t fade_in_ns,
           std::int64_t fade_out_ns) {
    volume_ = std::clamp(volume, 0.0f, 2.0f);
    fade_in_ = std::max<std::int64_t>(0, fade_in_ns);
    fade_out_ = std::max<std::int64_t>(0, fade_out_ns);
    if (!keep_.set(pairs_ns)) {
      active_ = false;
      return false;
    }
    length_ = keep_.kept_ns();
    active_ = volume_ != 1.0f || ((fade_in_ > 0 || fade_out_ > 0) && keep_.active());
    return true;
  }
  void clear() noexcept { active_ = false; }
  [[nodiscard]] bool active() const noexcept { return active_; }

  [[nodiscard]] float gain_at(std::int64_t source_ns) const noexcept {
    if (!active_) return 1.0f;
    float g = volume_;
    const auto prog = keep_.program_of(source_ns);
    if (!prog) return g;
    if (fade_in_ > 0 && *prog < fade_in_) g *= static_cast<float>(*prog) / static_cast<float>(fade_in_);
    if (fade_out_ > 0 && length_ - *prog < fade_out_) {
      g *= static_cast<float>(std::max<std::int64_t>(0, length_ - *prog)) / static_cast<float>(fade_out_);
    }
    return g;
  }

  // Interleaved float frames; frame k is at source time first_ns + k * ns_per_frame.
  // The gain is evaluated every kStep frames and held between (well under a
  // millisecond at 48 kHz: no audible step).
  void apply(float* pcm, std::uint32_t frames, std::uint32_t channels, std::int64_t first_ns,
             double ns_per_frame) const noexcept {
    if (!active_ || pcm == nullptr || channels == 0) return;
    constexpr std::uint32_t kStep = 32;
    for (std::uint32_t f = 0; f < frames; f += kStep) {
      const float g = gain_at(first_ns + static_cast<std::int64_t>(static_cast<double>(f) * ns_per_frame));
      const std::uint32_t end = std::min(frames, f + kStep);
      for (std::uint32_t i = f * channels; i < end * channels; ++i) pcm[i] *= g;
    }
  }

 private:
  keep_ranges keep_;
  float volume_ = 1.0f;
  std::int64_t fade_in_ = 0;
  std::int64_t fade_out_ = 0;
  std::int64_t length_ = 0;
  bool active_ = false;
};

}  // namespace mv
