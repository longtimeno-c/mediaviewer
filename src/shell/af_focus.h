// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #209 (docs/design/16 View, `Shift+Z`): where "zoom to the AF point" lands.
// Pure maths over what the overlay already holds — the AF quads the host
// pre-formatted from the metadata record (`meta_overlay`, displayed image,
// EXIF orientation already applied by meta::displayed_af_points) and the edit
// map the render thread places the texture with. No I/O, no decode; both hosts'
// render threads call it on the frame the command lands.
#pragma once

#include <optional>

#include "edit/edit_stack.h"
#include "shell/input_state.h"

namespace mv::shell {

// A point normalised 0..1 with a top-left origin.
struct af_focus_point {
  float u = 0.0f;
  float v = 0.0f;
};

// The AF point to check focus on. The in-focus quads when there are any (a
// camera that reports several in-focus points of a zone), else every quad
// (the selected ones); of those, the one whose centre is nearest their common
// centroid, so a cluster lands on a real point and not on the gap between
// two. Ties go to the first. nullopt when there are no quads.
[[nodiscard]] inline std::optional<af_focus_point> pick_af_focus(const meta_overlay& m) noexcept {
  const int n = m.af_count < meta_overlay::kMaxAf ? m.af_count : meta_overlay::kMaxAf;
  if (n <= 0) return std::nullopt;
  bool any_in_focus = false;
  for (int i = 0; i < n; ++i) any_in_focus = any_in_focus || m.af[i][4] > 0.5f;
  const auto counts = [&](int i) { return !any_in_focus || m.af[i][4] > 0.5f; };
  const auto centre = [&](int i) {
    return af_focus_point{m.af[i][0] + m.af[i][2] * 0.5f, m.af[i][1] + m.af[i][3] * 0.5f};
  };
  float sx = 0.0f, sy = 0.0f;
  int k = 0;
  for (int i = 0; i < n; ++i) {
    if (!counts(i)) continue;
    const af_focus_point c = centre(i);
    sx += c.u;
    sy += c.v;
    ++k;
  }
  const float mx = sx / static_cast<float>(k);
  const float my = sy / static_cast<float>(k);
  int best = -1;
  float best_d = 0.0f;
  for (int i = 0; i < n; ++i) {
    if (!counts(i)) continue;
    const af_focus_point c = centre(i);
    const float d = (c.u - mx) * (c.u - mx) + (c.v - my) * (c.v - my);
    if (best < 0 || d < best_d) {
      best = i;
      best_d = d;
    }
  }
  return centre(best);
}

// `p` (in the displayed, unedited picture: the texture the edit map samples)
// carried into the edited output the camera frames, through the inverse of
// `output_to_source` (edit::placement::map). A user rotate / flip / straighten
// moves it with the pixels; nullopt when a crop has cut it away.
[[nodiscard]] inline std::optional<af_focus_point> af_focus_in_output(
    af_focus_point p, const edit::affine& output_to_source) noexcept {
  if (output_to_source.identity()) return p;
  const edit::affine inv = edit::invert(output_to_source);
  const float* m = inv.m;
  const af_focus_point o{m[0] * p.u + m[1] * p.v + m[2], m[3] * p.u + m[4] * p.v + m[5]};
  // A hair of slack for float error at an uncropped edge.
  constexpr float kEps = 1e-4f;
  if (o.u < -kEps || o.v < -kEps || o.u > 1.0f + kEps || o.v > 1.0f + kEps) return std::nullopt;
  return o;
}

}  // namespace mv::shell
