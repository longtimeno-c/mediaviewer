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
#include "shell/input_state.h"

namespace mv::shell {

// The latest deadline the overlay animates to (0: none).
[[nodiscard]] inline std::uint64_t tool_hud_until(const edit_view& v) noexcept {
  return std::max(v.tool_label_until_ms, v.tool_pulse_until_ms);
}

// Whether the overlay has anything to draw.
[[nodiscard]] inline bool tool_overlay_wanted(const edit_view& v) noexcept {
  return (v.brush_on || v.brush_count > 0 || v.brush_mask != 0 || v.tool != 0 || v.tool_ring_px > 0.0f) &&
         !v.crop_overlay;
}

namespace detail {

[[nodiscard]] inline ImU32 rgba(std::uint32_t c, float alpha_scale = 1.0f) noexcept {
  const auto a = static_cast<int>(std::clamp(static_cast<float>(c & 0xFFu) * alpha_scale, 0.0f, 255.0f));
  return IM_COL32((c >> 24) & 0xFFu, (c >> 16) & 0xFFu, (c >> 8) & 0xFFu, a);
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

}  // namespace detail

// `pw` x `ph` the shown picture, `zoom` screen pixels per picture pixel,
// `scale` the display's (physical pixels per point), `now_ms` steady_ms().
// `mask` the lab's brush tint texture when it belongs to this view (the
// backend's ImTextureID: an SRV on Windows, an MTLTexture on the Mac), else 0.
template <class ToScreen>
void draw_tool_overlay(const edit_view& v, float pw, float ph, float zoom, float scale, std::uint32_t accent,
                       std::uint64_t now_ms, ImTextureID mask, ToScreen to_screen) {
  if (pw <= 0.0f || ph <= 0.0f || zoom <= 0.0f) return;
  ImDrawList* fg = ImGui::GetForegroundDrawList();
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
