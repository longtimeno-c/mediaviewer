// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S3: the canvas tools drawn over the photo, in the
// same present as the picture (ImGui's foreground list, like crop mode's
// draft). One body for both labs (present_lab.cpp, present_lab_mac.mm); each
// passes its camera's image → screen mapping. Replaces slice 7's
// brush_overlay.h.
//
//   * S4a: the brush's committed strokes, one tint texture a worker built
//     (shell/brush_mask.h), drawn as one ImGui image over the picture;
//   * the dabs that texture lacks yet (the stroke in progress);
//   * the ring under the pointer, sized in screen pixels: a 1.5 px dark
//     ring outside a 1 px light one and a 2 px centre dot, dashed while the
//     brush erases (two-tone so it reads on any photo);
//   * the size label ("48 px") for kToolLabelMs after a change;
//   * the red-eye fixes (a ring with a check; a cross on the one a click
//     would remove) and a kToolPulseMs pulse where a fix just landed;
//   * S8: the Faces tool's boxes, one per face (corner brackets, a numbered
//     badge the panel's list shares, a wash and the accent when chosen, the
//     person's name below, a dashed edge while a fix runs on it).
//   * one-click Enhance (docs/design/29 "One click"): the working glow, light
//     bleeding in from the photo's edges in drifting lilac, pink, periwinkle,
//     coral and peach (after Apple Intelligence's edge light), while a run
//     works on the photo; it recedes in kWorkGlowFadeMs when the run ends.
//     With Reduce Motion it is still (no drift, no swell, no fade). The labs present every
//     frame only while work_glow_live() (0 presents once it has gone).
//   * S6: Extend's frame past the photo's edges: the new area hatched (a
//     side past 25 % in the caution tint), the part of the photo it crops
//     dimmed as crop mode dims it, and the frame with its handles. The
//     readout "+18 % · 6048×4032" is the context bar's (owner call A:
//     nothing floats over the photo).
//
// Colours come from the theme (input_snapshot::home.accent) or are the ring's
// black and white. Nothing here presents on its own: the lab keeps drawing
// while tool_hud_until is ahead, then once more to clear (0 presents on a
// still with a tool armed and the pointer still).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "shell/edit_workspace.h"
#include "shell/input_state.h"

namespace mv::shell {

// The latest deadline the overlay animates to (0: none).
[[nodiscard]] inline std::uint64_t tool_hud_until(const edit_view& v) noexcept {
  return std::max(v.tool_label_until_ms, v.tool_pulse_until_ms);
}

// Whether the overlay has anything to draw.
[[nodiscard]] inline bool tool_overlay_wanted(const edit_view& v) noexcept {
  return (v.brush_on || v.brush_count > 0 || v.brush_mask != 0 || v.tool != 0 || v.tool_ring_px > 0.0f ||
          v.work_glow_on_ms != 0 || v.work_glow_off_ms != 0) &&
         !v.crop_overlay;
}

// One-click Enhance: the glow's fade once a run ends (shell/enhance_run.h's
// kWorkGlowFadeMs; this header stays free of it).
inline constexpr std::uint64_t kWorkGlowFade = 300;

// Whether the working glow animates now: on (unless Reduce Motion holds it
// still), or fading out. The labs present each frame while this holds, then
// once more to clear, then idle.
[[nodiscard]] inline bool work_glow_live(const edit_view& v, std::uint64_t now_ms, bool reduce_motion) noexcept {
  if (reduce_motion) return false;
  if (v.work_glow_on_ms != 0) return true;
  return v.work_glow_off_ms != 0 && now_ms < v.work_glow_off_ms + kWorkGlowFade;
}

// canvas_tool::extend's wire value (canvas_tool.h asserts it; this header
// does not include canvas_tool.h, which the labs need not).
inline constexpr std::uint8_t kToolExtendWire = 5;
// The caution tint of a side past Extend's 25 % warning (fills may smear
// there). The theme has no caution token; this is the chromes' picture amber
// (MVTheme.warm, IslandHost TrackWarm), which their bars' readout uses too.
inline constexpr std::uint32_t kExtendCautionRgba = 0xFAB84DFFu;

namespace detail {

[[nodiscard]] inline ImU32 rgba(std::uint32_t c, float alpha_scale = 1.0f) noexcept {
  const auto a = static_cast<int>(std::clamp(static_cast<float>(c & 0xFFu) * alpha_scale, 0.0f, 255.0f));
  return IM_COL32((c >> 24) & 0xFFu, (c >> 16) & 0xFFu, (c >> 8) & 0xFFu, a);
}

// One quad with a colour at each corner (p0..p3 in order round it).
inline void quad4(ImDrawList* fg, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 c0, ImU32 c1, ImU32 c2,
                  ImU32 c3) noexcept {
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  fg->PrimReserve(6, 4);
  const auto i = static_cast<ImDrawIdx>(fg->_VtxCurrentIdx);
  fg->PrimWriteIdx(i);
  fg->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
  fg->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
  fg->PrimWriteIdx(i);
  fg->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
  fg->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
  fg->PrimWriteVtx(p0, uv, c0);
  fg->PrimWriteVtx(p1, uv, c1);
  fg->PrimWriteVtx(p2, uv, c2);
  fg->PrimWriteVtx(p3, uv, c3);
}

// The four corners of a box as brackets, each `len` along both sides.
inline void corner_brackets(ImDrawList* fg, ImVec2 a, ImVec2 b, float len, ImU32 col, float thick) noexcept {
  const ImVec2 pts[4][3] = {
      {ImVec2(a.x, a.y + len), a, ImVec2(a.x + len, a.y)},
      {ImVec2(b.x - len, a.y), ImVec2(b.x, a.y), ImVec2(b.x, a.y + len)},
      {ImVec2(b.x, b.y - len), b, ImVec2(b.x - len, b.y)},
      {ImVec2(a.x + len, b.y), ImVec2(a.x, b.y), ImVec2(a.x, b.y - len)},
  };
  for (const auto& p : pts) fg->AddPolyline(p, 3, col, 0, thick);
}

// A dashed rectangle (a fix running on the face).
inline void dashed_rect(ImDrawList* fg, ImVec2 a, ImVec2 b, float dash, ImU32 col, float thick) noexcept {
  const ImVec2 corners[5] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), a};
  for (int s = 0; s < 4; ++s) {
    const ImVec2 p = corners[s], q = corners[s + 1];
    const float len = std::hypot(q.x - p.x, q.y - p.y);
    if (len <= 0.0f) continue;
    const float dx = (q.x - p.x) / len, dy = (q.y - p.y) / len;
    for (float t = 0.0f; t < len; t += 2.0f * dash) {
      const float e = std::min(t + dash, len);
      fg->AddLine(ImVec2(p.x + dx * t, p.y + dy * t), ImVec2(p.x + dx * e, p.y + dy * e), col, thick);
    }
  }
}

inline void dashed_circle(ImDrawList* fg, ImVec2 c, float r, ImU32 col, float thick) noexcept {
  const int segs = std::clamp(static_cast<int>(r * 0.5f), 12, 64) & ~1;
  const float step = 6.2831853f / static_cast<float>(segs);
  for (int i = 0; i < segs; i += 2) {
    const float a0 = step * static_cast<float>(i), a1 = a0 + step;
    fg->PathArcTo(c, r, a0, a1, 4);
    fg->PathStroke(col, 0, thick);
  }
}

// A hatched band: a wash and diagonal lines, clipped to it.
inline void hatch(ImDrawList* fg, ImVec2 a, ImVec2 b, ImU32 wash, ImU32 line, float step, float thick) noexcept {
  if (!(b.x > a.x && b.y > a.y)) return;
  fg->AddRectFilled(a, b, wash);
  fg->PushClipRect(a, b, true);
  const float h = b.y - a.y;
  // Lines anchored to the screen (x + y a multiple of `step` along each), so
  // a band that grows or moves does not crawl: the line through (x, b.y) has
  // x + b.y = k * step, and the first that reaches the band starts past a.x - h.
  const float start = std::floor((a.x - h + b.y) / step) * step - b.y;
  for (float x = start; x < b.x; x += step) {
    fg->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), line, thick);
  }
  fg->PopClipRect();
}

}  // namespace detail

// S6: Extend's frame (edit_view::tool_extend) over the picture.
template <class ToScreen>
void draw_extend_frame(const edit_view& v, float pw, float ph, float scale, std::uint32_t accent,
                       ToScreen to_screen) {
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  const float l = v.tool_extend[0], t = v.tool_extend[1], r = v.tool_extend[2], b = v.tool_extend[3];
  const ImVec2 p0 = to_screen(0.0f, 0.0f), p1 = to_screen(pw, ph);
  const ImVec2 f0 = to_screen(-l * pw, -t * ph), f1 = to_screen((1.0f + r) * pw, (1.0f + b) * ph);
  // The part of the photo the frame leaves out: cropped on Fill, dimmed.
  const ImU32 shade = IM_COL32(0, 0, 0, 140);
  const float ix0 = std::max(p0.x, f0.x), iy0 = std::max(p0.y, f0.y);
  const float ix1 = std::min(p1.x, f1.x), iy1 = std::min(p1.y, f1.y);
  if (ix0 < ix1 && iy0 < iy1) {
    if (iy0 > p0.y) fg->AddRectFilled(p0, ImVec2(p1.x, iy0), shade);
    if (iy1 < p1.y) fg->AddRectFilled(ImVec2(p0.x, iy1), p1, shade);
    if (ix0 > p0.x) fg->AddRectFilled(ImVec2(p0.x, iy0), ImVec2(ix0, iy1), shade);
    if (ix1 < p1.x) fg->AddRectFilled(ImVec2(ix1, iy0), ImVec2(p1.x, iy1), shade);
  }
  // The new picture past the photo's edges, hatched; a side past the warning
  // in the caution tint.
  const float step = 9.0f * scale;
  const auto band = [&](ImVec2 a, ImVec2 c, bool warn) {
    const std::uint32_t ink = warn ? kExtendCautionRgba : accent;
    detail::hatch(fg, a, c, detail::rgba(ink, warn ? 0.22f : 0.12f), detail::rgba(ink, warn ? 0.7f : 0.45f), step,
                  1.0f * scale);
  };
  if (f0.y < p0.y) band(f0, ImVec2(f1.x, std::min(f1.y, p0.y)), t > kExtendWarn);
  if (f1.y > p1.y) band(ImVec2(f0.x, std::max(f0.y, p1.y)), f1, b > kExtendWarn);
  const float my0 = std::max(f0.y, p0.y), my1 = std::min(f1.y, p1.y);
  if (my0 < my1) {
    if (f0.x < p0.x) band(ImVec2(f0.x, my0), ImVec2(std::min(f1.x, p0.x), my1), l > kExtendWarn);
    if (f1.x > p1.x) band(ImVec2(std::max(f0.x, p1.x), my0), ImVec2(f1.x, my1), r > kExtendWarn);
  }
  // The frame: a light line over a dark one, the held or hovered edge in the
  // accent; handles at the corners and the middles of the edges.
  const ImU32 dark = IM_COL32(0, 0, 0, 170);
  const ImU32 light = IM_COL32(255, 255, 255, 235);
  const ImU32 hot = detail::rgba(v.tool_extend_warn ? kExtendCautionRgba : accent);
  fg->AddRect(ImVec2(f0.x - scale, f0.y - scale), ImVec2(f1.x + scale, f1.y + scale), dark, 0.0f, 0, 1.5f * scale);
  fg->AddRect(f0, f1, light, 0.0f, 0, 1.5f * scale);
  const std::uint8_t hh = v.tool_extend_handle;
  if (hh & extend_left) fg->AddLine(f0, ImVec2(f0.x, f1.y), hot, 3.0f * scale);
  if (hh & extend_right) fg->AddLine(ImVec2(f1.x, f0.y), f1, hot, 3.0f * scale);
  if (hh & extend_top) fg->AddLine(f0, ImVec2(f1.x, f0.y), hot, 3.0f * scale);
  if (hh & extend_bottom) fg->AddLine(ImVec2(f0.x, f1.y), f1, hot, 3.0f * scale);
  const float k = 4.0f * scale;
  const float mx = (f0.x + f1.x) * 0.5f, my = (f0.y + f1.y) * 0.5f;
  for (const ImVec2 c : {f0, ImVec2(mx, f0.y), ImVec2(f1.x, f0.y), ImVec2(f1.x, my), f1, ImVec2(mx, f1.y),
                         ImVec2(f0.x, f1.y), ImVec2(f0.x, my)}) {
    fg->AddRectFilled(ImVec2(c.x - k - scale, c.y - k - scale), ImVec2(c.x + k + scale, c.y + k + scale), dark);
    fg->AddRectFilled(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), light);
  }
}

// One-click Enhance's working glow over the photo shown from `a` to `b` on
// screen, held to `clip` (the canvas; a zero-width clip is none). After the
// Apple Intelligence edge light (owner 2026-11-05: "more natural"): light
// that bleeds in from the photo's edges and falls away softly (a fine bright
// core on the edge, a bloom, a faint wide wash, and a little spill outside),
// coloured by a few soft blobs of lilac, pink, periwinkle, coral and peach
// that drift round the edge at their own speeds and swell and ebb at their
// own rates, so no two moments match and nothing turns like a wheel. It grows
// in from the edges as it comes and recedes as it goes. No sweep across the
// picture. Reduce Motion: the same light, still.
namespace detail {

struct glow_blob {
  float r, g, b;
  float start;   // where on the edge it begins (fraction of the way round)
  float speed;   // fractions of the way round per second (sign: direction)
  float width;   // its spread along the edge (fraction of the way round)
  float period;  // seconds per swell
  float phase;
};

// Apple's palette for the edge light (lilac, pink, periwinkle, coral, peach,
// violet), spread round the edge, moving at unrelated speeds.
inline constexpr glow_blob kGlowBlobs[] = {
    {0.62f, 0.36f, 1.00f, 0.00f, 0.031f, 0.11f, 3.7f, 0.0f},   // violet
    {1.00f, 0.42f, 0.78f, 0.17f, -0.043f, 0.09f, 2.9f, 1.3f},  // pink
    {0.33f, 0.52f, 1.00f, 0.35f, 0.024f, 0.12f, 4.3f, 2.1f},   // blue
    {1.00f, 0.38f, 0.42f, 0.53f, 0.052f, 0.09f, 3.1f, 4.0f},   // coral
    {1.00f, 0.66f, 0.30f, 0.70f, -0.029f, 0.10f, 5.1f, 0.7f},  // amber
    {0.45f, 0.70f, 1.00f, 0.86f, 0.037f, 0.10f, 3.4f, 5.2f},   // sky
};

struct glow_tint {
  float r, g, b;
  float level;  // how bright the light is here, about 0.5 .. 1
};

// The light's colour and strength at `u` (fraction of the way round) at `t`
// seconds: the blob strongest here sets the colour (weights cubed, so
// neighbours blend at their meeting and never average to grey).
[[nodiscard]] inline glow_tint glow_tint_at(float u, double t, bool still) noexcept {
  float r = 0.0f, g = 0.0f, b = 0.0f, sum = 0.0f, raw = 0.0f;
  for (const glow_blob& blob : kGlowBlobs) {
    const double c = static_cast<double>(blob.start) + (still ? 0.0 : static_cast<double>(blob.speed) * t);
    float d = u - static_cast<float>(c - std::floor(c));
    d -= std::round(d);  // the shorter way round
    const float swell =
        still ? 0.85f
              : 0.6f + 0.4f * static_cast<float>(std::sin(t * 6.283185307179586 / blob.period + blob.phase));
    const float w = swell * std::exp(-(d * d) / (blob.width * blob.width));
    const float w3 = w * w * w + 1e-6f;
    r += blob.r * w3, g += blob.g * w3, b += blob.b * w3, sum += w3;
    raw = std::max(raw, w);
  }
  return {r / sum, g / sum, b / sum, std::clamp(0.5f + 0.5f * raw, 0.5f, 1.0f)};
}

// How much light is left `x` of the way from the edge into the band (0 the
// edge, 1 the band's inner end, where none is left): a bright core, a bloom
// and a wide faint wash, each falling off exponentially.
[[nodiscard]] inline float glow_falloff(float x) noexcept {
  const auto f = [](float v) {
    return 0.55f * std::exp(-v / 0.025f) + 0.33f * std::exp(-v / 0.13f) + 0.12f * std::exp(-v / 0.40f);
  };
  return std::max(0.0f, (f(x) - f(1.0f)) / (f(0.0f) - f(1.0f)));
}

[[nodiscard]] inline ImU32 glow_vertex(const glow_tint& c, float alpha, float white) noexcept {
  const auto ch = [white](float v) { return static_cast<int>(std::lround((v + (1.0f - v) * white) * 255.0f)); };
  return IM_COL32(ch(c.r), ch(c.g), ch(c.b), static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

}  // namespace detail

inline void draw_work_glow(ImDrawList* fg, ImVec2 a, ImVec2 b, ImVec4 clip, float scale, std::uint64_t now_ms,
                           std::uint64_t on_ms, std::uint64_t off_ms, bool reduce_motion) noexcept {
  float k = 1.0f;  // the glow's strength: coming in, on, going out
  if (on_ms != 0) {
    if (!reduce_motion && now_ms > on_ms) k = std::min(1.0f, static_cast<float>(now_ms - on_ms) / 700.0f);
    if (!reduce_motion && now_ms <= on_ms) k = 0.0f;
  } else if (off_ms != 0 && !reduce_motion && now_ms < off_ms + kWorkGlowFade) {
    k = 1.0f - static_cast<float>(now_ms - std::min(now_ms, off_ms)) / static_cast<float>(kWorkGlowFade);
  } else {
    return;
  }
  if (k <= 0.0f) return;
  const float ease = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);  // ease-out: it arrives, then settles
  ImVec2 ra = a, rb = b;
  if (clip.z > clip.x && clip.w > clip.y) {
    ra = ImVec2(std::max(a.x, clip.x), std::max(a.y, clip.y));
    rb = ImVec2(std::min(b.x, clip.z), std::min(b.y, clip.w));
    fg->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
  } else {
    fg->PushClipRect(ImVec2(-1e6f, -1e6f), ImVec2(1e6f, 1e6f), false);
  }
  const float w = rb.x - ra.x, h = rb.y - ra.y;
  if (w < 8.0f || h < 8.0f) {
    fg->PopClipRect();
    return;
  }
  const double secs = static_cast<double>(now_ms) / 1000.0;
  // How far the light reaches in: grows in from the edges as it comes.
  const float full = std::clamp(std::min(w, h) * 0.14f, 26.0f * scale, std::min(150.0f * scale, std::min(w, h) * 0.45f));
  const float band = full * (0.35f + 0.65f * ease);
  const float spill = std::min(band * 0.30f, 26.0f * scale);
  // Rings from the edge inward (fractions of `band`), close together where
  // the light changes fastest; and outward, the spill.
  static constexpr float kIn[] = {0.0f, 0.012f, 0.035f, 0.07f, 0.13f, 0.22f, 0.34f, 0.50f, 0.70f, 1.0f};
  static constexpr float kOut[] = {0.0f, 0.25f, 0.6f, 1.0f};
  constexpr int kInRings = static_cast<int>(sizeof(kIn) / sizeof(kIn[0]));
  constexpr int kOutRings = static_cast<int>(sizeof(kOut) / sizeof(kOut[0]));
  const float perimeter = 2.0f * (w + h);
  const ImVec2 o[4] = {ra, ImVec2(rb.x, ra.y), rb, ImVec2(ra.x, rb.y)};
  // Each edge casts its own light inward along its whole length; at a
  // corner two edges' light overlaps, so the corner is a little brighter and
  // its falloff rounds off, as light from two sides does (no mitre seam).
  static constexpr float kNormal[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
  const float side_len[4] = {w, h, w, h};
  const auto at = [&](int e, float t, float depth) {
    const int n = (e + 1) % 4;
    return ImVec2(o[e].x + (o[n].x - o[e].x) * t + kNormal[e][0] * depth,
                  o[e].y + (o[n].y - o[e].y) * t + kNormal[e][1] * depth);
  };
  const auto spill_alpha = [k](float x) { return 0.34f * (1.0f - x) * (1.0f - x) * k; };
  float along = 0.0f;
  for (int e = 0; e < 4; ++e) {
    const int segs = std::max(6, static_cast<int>(std::lround(160.0f * side_len[e] / perimeter)));
    for (int i = 0; i < segs; ++i) {
      const float t0 = static_cast<float>(i) / static_cast<float>(segs);
      const float t1 = static_cast<float>(i + 1) / static_cast<float>(segs);
      const detail::glow_tint c0 = detail::glow_tint_at((along + side_len[e] * t0) / perimeter, secs, reduce_motion);
      const detail::glow_tint c1 = detail::glow_tint_at((along + side_len[e] * t1) / perimeter, secs, reduce_motion);
      // Inside: from the edge in, each ring's light, the core on the edge a
      // little toward white (light that strong reads paler).
      for (int r = 0; r + 1 < kInRings; ++r) {
        const float x0 = kIn[r], x1 = kIn[r + 1];
        const float a0 = 0.85f * detail::glow_falloff(x0) * k, a1 = 0.85f * detail::glow_falloff(x1) * k;
        const float wh0 = 0.3f * std::exp(-x0 / 0.015f), wh1 = 0.3f * std::exp(-x1 / 0.015f);
        detail::quad4(fg, at(e, t0, band * x0), at(e, t1, band * x0), at(e, t1, band * x1), at(e, t0, band * x1),
                      detail::glow_vertex(c0, a0 * c0.level, wh0), detail::glow_vertex(c1, a0 * c1.level, wh0),
                      detail::glow_vertex(c1, a1 * c1.level, wh1), detail::glow_vertex(c0, a1 * c0.level, wh1));
      }
      // Outside: the light spilling a little past the edge.
      for (int r = 0; r + 1 < kOutRings; ++r) {
        const float d0 = -spill * kOut[r], d1 = -spill * kOut[r + 1];
        const float a0 = spill_alpha(kOut[r]), a1 = spill_alpha(kOut[r + 1]);
        detail::quad4(fg, at(e, t0, d0), at(e, t1, d0), at(e, t1, d1), at(e, t0, d1),
                      detail::glow_vertex(c0, a0 * c0.level, 0.0f), detail::glow_vertex(c1, a0 * c1.level, 0.0f),
                      detail::glow_vertex(c1, a1 * c1.level, 0.0f), detail::glow_vertex(c0, a1 * c0.level, 0.0f));
      }
    }
    // The spill round the corner ending this edge: a quarter of a ring of the
    // corner's colour, between this edge's outward normal and the next one's.
    {
      const int n = (e + 1) % 4;
      const detail::glow_tint c = detail::glow_tint_at((along + side_len[e]) / perimeter, secs, reduce_motion);
      const float a_from = std::atan2(-kNormal[e][1], -kNormal[e][0]);
      constexpr int kArc = 8;
      for (int j = 0; j < kArc; ++j) {
        const float q0 = a_from + 1.5707963f * static_cast<float>(j) / kArc;
        const float q1 = a_from + 1.5707963f * static_cast<float>(j + 1) / kArc;
        for (int r = 0; r + 1 < kOutRings; ++r) {
          const float d0 = spill * kOut[r], d1 = spill * kOut[r + 1];
          const ImVec2 p = o[n];
          const auto pt = [p](float q, float d) { return ImVec2(p.x + std::cos(q) * d, p.y + std::sin(q) * d); };
          const ImU32 v0 = detail::glow_vertex(c, spill_alpha(kOut[r]) * c.level, 0.0f);
          const ImU32 v1 = detail::glow_vertex(c, spill_alpha(kOut[r + 1]) * c.level, 0.0f);
          detail::quad4(fg, pt(q0, d0), pt(q1, d0), pt(q1, d1), pt(q0, d1), v0, v0, v1, v1);
        }
      }
    }
    along += side_len[e];
  }
  fg->PopClipRect();
}

// `pw` x `ph` the shown picture, `zoom` screen pixels per picture pixel,
// `scale` the display's (physical pixels per point), `now_ms` steady_ms().
// `mask` the lab's brush tint texture when it belongs to this view (the
// backend's ImTextureID: an SRV on Windows, an MTLTexture on the Mac), else 0.
// `clip` the canvas on screen (x0, y0, x1, y1) for the working glow, and
// `reduce_motion` the system's setting.
template <class ToScreen>
void draw_tool_overlay(const edit_view& v, float pw, float ph, float zoom, float scale, std::uint32_t accent,
                       std::uint64_t now_ms, ImTextureID mask, ToScreen to_screen, ImVec4 clip = ImVec4(0, 0, 0, 0),
                       bool reduce_motion = false) {
  if (pw <= 0.0f || ph <= 0.0f || zoom <= 0.0f) return;
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  // One-click Enhance: the working glow, under the tool's own marks.
  if (v.work_glow_on_ms != 0 || v.work_glow_off_ms != 0) {
    draw_work_glow(fg, to_screen(0.0f, 0.0f), to_screen(pw, ph), clip, scale, now_ms, v.work_glow_on_ms,
                   v.work_glow_off_ms, reduce_motion);
  }
  const float long_side = std::max(pw, ph);
  const float dim = v.tool_dim ? 0.5f : 1.0f;
  // S4a: the committed strokes, one textured quad over the whole picture
  // (dimmed with the tool put down).
  if (mask != 0 && v.brush_mask) {
    fg->AddImage(ImTextureRef(mask), to_screen(0.0f, 0.0f), to_screen(pw, ph), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                 IM_COL32(255, 255, 255, static_cast<int>(255 * dim)));
  }
  // The live dabs: overlapping dabs build up toward solid, as a soft brush
  // would; an erase dab is a dark hole until the next build takes it in.
  const ImU32 paint = detail::rgba(accent, 0.18f * dim);
  const ImU32 hole = IM_COL32(0, 0, 0, static_cast<int>(70 * dim));
  for (std::uint32_t i = 0; i < v.brush_count && i < kBrushDabs; ++i) {
    const brush_dab& d = v.brush[i];
    fg->AddCircleFilled(to_screen(d.u * pw, d.v * ph), d.r * long_side * zoom, d.erase ? hole : paint);
  }
  const ImU32 dark = IM_COL32(0, 0, 0, 170);
  const ImU32 light = IM_COL32(255, 255, 255, 235);
  const ImU32 accent_full = detail::rgba(accent);
  // Red-eye fixes: a ring and a check; the one under the pointer shows the
  // cross a click acts on.
  for (std::uint32_t i = 0; i < v.tool_spot_count && i < static_cast<std::uint32_t>(kToolSpots); ++i) {
    const ImVec2 c = to_screen(v.tool_spots[i][0] * pw, v.tool_spots[i][1] * ph);
    const float r = std::max(v.tool_spots[i][2] * long_side * zoom, 4.0f * scale);
    const bool hover = v.tool_hover_spot == static_cast<std::int8_t>(i);
    fg->AddCircle(c, r + scale, dark, 0, 1.5f * scale);
    fg->AddCircle(c, r, hover ? light : accent_full, 0, 1.5f * scale);
    const float m = std::max(3.0f * scale, r * 0.35f);
    const ImVec2 b(c.x + r * 0.72f, c.y - r * 0.72f);  // the badge at the ring's top right
    fg->AddCircleFilled(b, m, hover ? dark : accent_full);
    if (hover) {
      const float k = m * 0.5f;
      fg->AddLine(ImVec2(b.x - k, b.y - k), ImVec2(b.x + k, b.y + k), light, 1.5f * scale);
      fg->AddLine(ImVec2(b.x - k, b.y + k), ImVec2(b.x + k, b.y - k), light, 1.5f * scale);
    } else {
      fg->PathLineTo(ImVec2(b.x - m * 0.5f, b.y));
      fg->PathLineTo(ImVec2(b.x - m * 0.12f, b.y + m * 0.4f));
      fg->PathLineTo(ImVec2(b.x + m * 0.5f, b.y - m * 0.38f));
      fg->PathStroke(light, 0, 1.5f * scale);
    }
  }
  // The pulse where a fix just landed: a ring growing and fading.
  if (v.tool_pulse[0] >= 0.0f && now_ms < v.tool_pulse_until_ms) {
    const float left = static_cast<float>(v.tool_pulse_until_ms - now_ms);
    const float t = std::clamp(1.0f - left / 300.0f, 0.0f, 1.0f);
    const ImVec2 c = to_screen(v.tool_pulse[0] * pw, v.tool_pulse[1] * ph);
    const float r = std::max(v.tool_pulse[2] * long_side * zoom, 4.0f * scale) * (1.0f + 0.6f * t);
    fg->AddCircle(c, r, detail::rgba(accent, 1.0f - t), 0, 2.0f * scale);
  }
  // S8: the Faces tool's boxes. Chosen faces wear the accent and a faint
  // wash; the rest white brackets, so the photo stays readable. The badge's
  // number is the panel's.
  for (std::uint32_t i = 0; i < v.tool_face_count && i < static_cast<std::uint32_t>(kToolFaces); ++i) {
    const ImVec2 p0 = to_screen(v.tool_faces[i][0] * pw, v.tool_faces[i][1] * ph);
    const ImVec2 p1 = to_screen((v.tool_faces[i][0] + v.tool_faces[i][2]) * pw,
                                (v.tool_faces[i][1] + v.tool_faces[i][3]) * ph);
    const ImVec2 a(std::min(p0.x, p1.x), std::min(p0.y, p1.y)), b(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
    if (b.x - a.x < 2.0f || b.y - a.y < 2.0f) continue;
    const bool chosen = ((v.tool_face_chosen >> i) & 1u) != 0u;
    const bool hover = v.tool_face_hover == static_cast<std::int8_t>(i);
    const bool busy = ((v.tool_face_busy >> i) & 1u) != 0u;
    const float round = 6.0f * scale;
    if (chosen) fg->AddRectFilled(a, b, detail::rgba(accent, 0.10f), round);
    if (hover) fg->AddRect(a, b, IM_COL32(255, 255, 255, chosen ? 90 : 130), round, 0, 1.0f * scale);
    const float len = std::clamp(std::min(b.x - a.x, b.y - a.y) * 0.24f, 6.0f * scale, 28.0f * scale);
    const float thick = (hover ? 3.0f : 2.0f) * scale;
    detail::corner_brackets(fg, a, b, len, dark, thick + 1.5f * scale);
    detail::corner_brackets(fg, a, b, len, chosen ? accent_full : IM_COL32(255, 255, 255, hover ? 255 : 200), thick);
    if (busy) detail::dashed_rect(fg, a, b, 5.0f * scale, accent_full, 1.5f * scale);
    // The badge on the top-left corner: the face's number.
    const float br = 9.0f * scale;
    const ImVec2 bc(a.x + br * 0.35f, a.y + br * 0.35f);
    fg->AddCircleFilled(bc, br + 1.5f * scale, dark);
    fg->AddCircleFilled(bc, br, chosen ? accent_full : IM_COL32(30, 30, 30, 220));
    if (!chosen) fg->AddCircle(bc, br, light, 0, 1.0f * scale);
    char num[4];
    std::snprintf(num, sizeof(num), "%u", i + 1);
    const ImVec2 ns = ImGui::CalcTextSize(num);
    fg->AddText(ImVec2(bc.x - ns.x * 0.5f, bc.y - ns.y * 0.5f), IM_COL32(255, 255, 255, 255), num);
    // The person's name in a pill under the box.
    if (v.tool_face_names[i][0] != '\0') {
      const char* name = v.tool_face_names[i];
      const ImVec2 ts = ImGui::CalcTextSize(name);
      const float pad = 6.0f * scale;
      const float cx = (a.x + b.x) * 0.5f;
      const ImVec2 pa(cx - ts.x * 0.5f - pad, b.y + 4.0f * scale);
      const ImVec2 pb(cx + ts.x * 0.5f + pad, b.y + 4.0f * scale + ts.y + pad * 0.6f);
      fg->AddRectFilled(pa, pb, IM_COL32(0, 0, 0, 170), (pb.y - pa.y) * 0.5f);
      fg->AddText(ImVec2(pa.x + pad, pa.y + pad * 0.3f), chosen ? accent_full : light, name);
    }
  }
  // S6: Extend's frame past the photo's edges.
  if (v.tool == kToolExtendWire) draw_extend_frame(v, pw, ph, scale, accent, to_screen);
  // The ring under the pointer, in screen pixels.
  if (v.tool_ring_px > 0.0f && !v.tool_dim) {
    const ImVec2 c = to_screen(v.brush_cursor[0] * pw, v.brush_cursor[1] * ph);
    const float r = v.tool_ring_px;
    if (v.tool_erase) {
      detail::dashed_circle(fg, c, r + 0.75f * scale, dark, 1.5f * scale);
      detail::dashed_circle(fg, c, r, light, 1.0f * scale);
    } else {
      fg->AddCircle(c, r + 0.75f * scale, dark, 0, 1.5f * scale);
      fg->AddCircle(c, r, light, 0, 1.0f * scale);
    }
    fg->AddCircleFilled(c, 1.0f * scale + 0.5f, dark);
    fg->AddCircleFilled(c, 1.0f * scale, light);
    // The size, for a moment after it changed, fading over its last 200 ms.
    if (now_ms < v.tool_label_until_ms && v.tool_label_px > 0.0f) {
      const float left = static_cast<float>(v.tool_label_until_ms - now_ms);
      const float a = std::clamp(left / 200.0f, 0.0f, 1.0f);
      char text[16];
      std::snprintf(text, sizeof(text), "%d px", static_cast<int>(std::lround(v.tool_label_px)));
      const ImVec2 size = ImGui::CalcTextSize(text);
      const ImVec2 at(c.x + r + 8.0f * scale, c.y - size.y * 0.5f);
      const float pad = 4.0f * scale;
      fg->AddRectFilled(ImVec2(at.x - pad, at.y - pad * 0.5f), ImVec2(at.x + size.x + pad, at.y + size.y + pad * 0.5f),
                        IM_COL32(0, 0, 0, static_cast<int>(170 * a)), 4.0f * scale);
      fg->AddText(at, IM_COL32(255, 255, 255, static_cast<int>(255 * a)), text);
    }
  }
}

}  // namespace mv::shell
