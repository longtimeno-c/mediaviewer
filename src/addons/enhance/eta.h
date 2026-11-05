// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One-click Enhance (owner 2026-11-05): how long a run will take, said on the
// Edit pane's card before anything runs ("About 2 min").
//
// From the measurements in docs/design/29 (Apple M-series, CPU fp32, 8
// threads; the per-tile seconds of the least loaded runs, so upper bounds on a
// quiet machine): Real-HAT-GAN 5.4 s a 192-px tile, RealWebPhoto 10 s a
// 256-px tile, SCUNet 4 s and NAFNet 1.25 s a 512-px tile. A run's tiles are
// counted as restore_picture counts them (picture_job.h restore_tiles), on the
// picture the model actually sees: the upscalers run on the photo shrunk to
// its detail (plan_shrink), so their time scales with (W·H)/d². Threads scale
// it by the performance level (tier_policy.threads against the 8 measured).
// Restyle: seconds a diffusion step at each quality (16 s for 20 steps at
// Standard, 43 s at High; Best extrapolated). Restore faces: seconds a face.
//
// Portable, no I/O (D9).
#pragma once

#include <cstdint>
#include <string>

#include "addons/enhance/spec.h"

namespace mv::enhance {

// What the card knows of the photo: its size after the edit, and how far it
// shrinks without losing detail (picture_job.h detail_shrink; 1 for none, or
// not measured yet).
struct photo_size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  double detail = 1.0;
};

// Seconds per tile at 8 threads, per model (doc 29 "Enhance image").
[[nodiscard]] double seconds_per_tile(model_kind k) noexcept;
// Seconds per diffusion step at a Restyle quality ("standard", "high", "best").
[[nodiscard]] double seconds_per_step(const std::string& quality) noexcept;
// Seconds per face restored (RestoreFormer++ / GFPGAN, aligned, guarded, pasted).
[[nodiscard]] double seconds_per_face(const std::string& model) noexcept;
// The level's threads against the 8 measured: 8 / threads, held to 0.5..4.
[[nodiscard]] double thread_factor(int threads) noexcept;

struct eta_in {
  model_kind kind = model_kind::still;
  const model_spec* spec = nullptr;  // io.tile / io.overlap / io.scale
  photo_size photo;
  std::uint32_t out_scale = 1;  // Enhance image's Size (1, 2, 4)
  int threads = 8;
  int steps = 40;               // Restyle's steps at the level
  std::string quality;          // Restyle's quality
};

// Seconds the run takes, or -1 when it cannot say (no size yet, a kind it
// has no measurement for).
[[nodiscard]] double estimate_seconds(const eta_in& in);

// "A few seconds", "About 20 seconds", "About 2 min", "About 1 h 10 min" (the
// progress line's time-left words, docs/design/20 "Craft rules").
[[nodiscard]] std::string eta_words(double seconds);

}  // namespace mv::enhance
