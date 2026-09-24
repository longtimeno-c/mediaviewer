// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10: the render thread's side of input_state.h's edit_view, shared by
// both labs (present_lab.cpp, present_lab_mac.mm) so they place a texture
// through its edit geometry the same way. Pure; no GPU, no platform header.
// PR 11 adds the colour uniforms (same_adjust, apply_adjust).
#pragma once

#include <cstdint>

#include "edit/adjust.h"
#include "edit/edit_stack.h"
#include "shell/input_state.h"

namespace mv::shell {

[[nodiscard]] inline edit::geometry geometry_of(const edit_view& v) noexcept {
  edit::geometry g;
  g.orient = codec::d4{v.d4[0], v.d4[1], v.d4[2], v.d4[3]};
  g.straighten = v.straighten;
  g.crop = edit::rect{v.crop[0], v.crop[1], v.crop[2], v.crop[3]};
  return g;
}

// The slot for a texture of `item`, or null. With two slots for one item (a
// file rewritten by a lossless rotate, reopened under a new generation) the
// one whose generation matches wins; otherwise slot 0, the item on the
// canvas now. Mac passes generation 0 and gives every open its own item id.
[[nodiscard]] inline const edit_view* match_edit(const edit_view (&slots)[2], std::uint64_t item,
                                                 std::uint32_t generation) noexcept {
  if (item == 0) return nullptr;
  const edit_view* any = nullptr;
  for (const edit_view& v : slots) {
    if (v.item != item) continue;
    if (v.generation == generation) return &v;
    if (!any) any = &v;
  }
  return any;
}

[[nodiscard]] inline edit::placement place_through(const edit_view* v, std::uint32_t width,
                                                   std::uint32_t height) noexcept {
  const edit::geometry g = v ? geometry_of(*v) : edit::geometry{};
  return edit::place(g, edit::size2{width, height}, {}, v && v->keep_frame);
}

[[nodiscard]] inline bool same_geometry(const edit_view& a, const edit_view& b) noexcept {
  return a.item == b.item && a.generation == b.generation && a.d4[0] == b.d4[0] &&
         a.d4[1] == b.d4[1] && a.d4[2] == b.d4[2] && a.d4[3] == b.d4[3] &&
         a.straighten == b.straighten && a.crop[0] == b.crop[0] && a.crop[1] == b.crop[1] &&
         a.crop[2] == b.crop[2] && a.crop[3] == b.crop[3] && a.keep_frame == b.keep_frame;
}

// PR 11: the colour half, compared apart from the geometry so a slider drag
// redraws without refitting the camera.
[[nodiscard]] inline bool same_adjust(const edit_view& a, const edit_view& b) noexcept {
  if (a.adjust != b.adjust) return false;
  if (!a.adjust) return true;
  for (int i = 0; i < 4; ++i) {
    if (a.adjust0[i] != b.adjust0[i] || a.adjust1[i] != b.adjust1[i]) return false;
  }
  return true;
}

// The colour uniforms of `v` onto a blit's parameters (either host's
// blit_params / blit_params_mac: the fields are named the same).
template <class BlitParams>
inline void apply_adjust(const edit_view* v, BlitParams& bp) noexcept {
  bp.adjust = v != nullptr && v->adjust;
  if (!bp.adjust) return;
  for (int i = 0; i < 4; ++i) {
    bp.adjust0[i] = v->adjust0[i];
    bp.adjust1[i] = v->adjust1[i];
  }
}

[[nodiscard]] inline bool same_overlay(const edit_view& a, const edit_view& b) noexcept {
  return a.crop_overlay == b.crop_overlay && a.overlay[0] == b.overlay[0] &&
         a.overlay[1] == b.overlay[1] && a.overlay[2] == b.overlay[2] &&
         a.overlay[3] == b.overlay[3];
}

// The edit_view the UI publishes for a session's current item.
template <class Session>
[[nodiscard]] edit_view view_of(const Session& s, std::uint64_t item, std::uint32_t generation) {
  edit_view v;
  if (!s.has_item() || item == 0) return v;
  const edit::geometry g = s.preview();
  v.item = item;
  v.generation = generation;
  v.d4[0] = g.orient.a;
  v.d4[1] = g.orient.b;
  v.d4[2] = g.orient.c;
  v.d4[3] = g.orient.d;
  v.straighten = g.straighten;
  v.crop[0] = g.crop.x;
  v.crop[1] = g.crop.y;
  v.crop[2] = g.crop.w;
  v.crop[3] = g.crop.h;
  v.keep_frame = s.preview_keeps_frame();
  v.crop_overlay = s.crop_active();
  const edit::rect r = s.crop_overlay();
  v.overlay[0] = r.x;
  v.overlay[1] = r.y;
  v.overlay[2] = r.w;
  v.overlay[3] = r.h;
  const edit::colour c = s.colour();
  if (!c.identity()) {
    const edit::adjust_uniforms u = edit::uniforms_of(c);
    v.adjust = true;
    for (int i = 0; i < 4; ++i) {
      v.adjust0[i] = u.a0[i];
      v.adjust1[i] = u.a1[i];
    }
  }
  return v;
}

}  // namespace mv::shell
