// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 — the Edit workspace (docs/design/20): one visible door ("Edit image" /
// "Edit video", Enter) to the edits PRs 10-14 built, as a strip of tabs over
// the right pane column. Shared by both hosts.
//
// Pure and UI-thread only. This decides WHICH tab a command lands on and
// whether the workspace opens or closes; the host then shows that tab's pane
// (Crop and Trim are new panes; Colour, Info and Jobs are the PR 11, PR 9 and
// PR 13 panes) and runs the command's own work exactly as before. No edit
// logic lives here: edit_session and trim_state stay the only owners.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "shell/commands.h"

namespace mv::shell {

// Values are stable: both chromes send them back as the tab to show.
enum class edit_tab : std::uint8_t {
  crop = 0,    // still: crop, aspect, straighten, rotate, flip
  colour = 1,  // still: the PR 11 adjust pane
  info = 2,    // still: the PR 9 / 12 metadata pane (rating, comment)
  trim = 3,    // clip: in / out, save, split, clip tools
  jobs = 4,    // clip: the PR 13 job queue
  // docs/plans/edit-refresh.md S2: the Enhance tab (the rail's third icon):
  // Remove objects, Restyle, Faces, Enhance image, Extend edges. Any still;
  // without the Enhance add-on its cards offer to install it.
  enhance = 5,
  count
};

enum class edit_subject : std::uint8_t {
  none,   // nothing on the canvas, or something neither side edits (an animation)
  still,
  clip,
};

[[nodiscard]] std::span<const edit_tab> tabs_for(edit_subject s) noexcept;
[[nodiscard]] bool tab_offered(edit_subject s, edit_tab t) noexcept;
[[nodiscard]] edit_tab default_tab(edit_subject s) noexcept;
[[nodiscard]] const char* tab_label(edit_tab t) noexcept;       // "Crop", "Adjust", ...
[[nodiscard]] const char* workspace_title(edit_subject s) noexcept;  // "Edit image" / "Edit video"

struct edit_workspace {
  bool open = false;
  edit_tab tab = edit_tab::crop;
  // docs/plans/edit-refresh.md S2: the inspector folded away, the rail alone
  // beside the canvas (a click on the rail's active icon). Opening, selecting
  // a tab or closing unfolds it.
  bool collapsed = false;
};

enum class workspace_action : std::uint8_t {
  none,    // not the workspace's command here: the host does what it always did
  open,    // open on `tab`
  select,  // already open: show `tab` (and unfold a collapsed inspector)
  close,
  collapse,  // S2: fold the inspector to the rail; the tab stays chosen
};

struct workspace_step {
  workspace_action action = workspace_action::none;
  edit_tab tab = edit_tab::crop;
};

// What `id` does to the workspace for the item on the canvas. The command's
// own effect (start cropping, arm trim, ...) is still the host's to run.
//   edit_workspace  toggles; opens on the subject's first tab
//   crop_mode       still: opens / selects Crop (the host starts cropping)
//   adjust_pane     still: opens / selects Colour; on Colour it closes (Shift+A toggles)
//   trim_mode       clip: opens / selects Trim (the host arms or disarms trim)
//   metadata_pane   only while open on a still: selects Info; on Info it closes
//   jobs_pane       only while open on a clip: selects Jobs; on Jobs it closes
[[nodiscard]] workspace_step route_workspace(const edit_workspace& ws, edit_subject subject,
                                             command_id id) noexcept;

// docs/plans/edit-refresh.md S2: a click on the rail's icon for `t`. Another
// tab selects it; the active tab's icon folds the inspector to the rail, and
// any icon of a folded rail unfolds it on that tab. A tab the subject lacks
// does nothing.
[[nodiscard]] workspace_step rail_pick(const edit_workspace& ws, edit_subject subject, edit_tab t) noexcept;

// Applies a step. Returns true when anything changed.
bool apply_step(edit_workspace& ws, const workspace_step& step) noexcept;

// The item on the canvas changed. With nothing editable the workspace closes;
// otherwise it stays open and a tab the new subject lacks becomes its first
// (Crop on a still, Trim on a clip). Returns true when anything changed.
bool follow_subject(edit_workspace& ws, edit_subject subject) noexcept;

// ---- Extend on the canvas (docs/plans/edit-refresh.md S6, §4.4) ----------------
//
// Extend's frame over the shown (edited) picture while canvas_tool::extend is
// armed: each side's distance past the picture's edge, as a fraction of its
// width (left, right) or height (top, bottom). Positive is new picture the
// Enhance add-on fills; negative is inside the photo, a crop. Shell state
// only, never in the edit stack (docs/design/07): Fill splits it into an
// ordinary crop (the part over the photo) and the margins the host table's
// still_extend hands Enhance. Pure and UI-thread only, like the rest here.
struct extend_draft {
  float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
  bool on = false;  // the Extend tool holds this draft (false: nothing to draw)
};

// Owner call E (2026-11-04): a soft warning past 25 % a side ("fills may smear
// past here"), a hard stop at 50 % a side (Enhance's extend_preset grows a
// picture at most twice each way).
inline constexpr float kExtendWarn = 0.25f;
inline constexpr float kExtendMax = 0.5f;
// The frame keeps at least this much of the photo each way.
inline constexpr float kExtendMinKeep = 0.05f;
// The camera frames the photo and the frame together with this much room
// around them (input_snapshot::fit_rect_uv).
inline constexpr float kExtendFitPad = 0.08f;
// Enhance's caps (src/addons/enhance/remove.h extend_from_fractions; kept
// equal, "[enhance][extend]" checks the two agree).
inline constexpr std::uint32_t kExtendMaxEdge = 16384;
inline constexpr std::uint64_t kExtendMaxPixels = 100'000'000;

[[nodiscard]] inline bool extend_empty(const extend_draft& d) noexcept {
  constexpr float e = 1e-6f;
  return std::fabs(d.l) < e && std::fabs(d.t) < e && std::fabs(d.r) < e && std::fabs(d.b) < e;
}
// Some side reaches past the photo: Fill needs Enhance.
[[nodiscard]] inline bool extend_overhangs(const extend_draft& d) noexcept {
  return d.l > 1e-6f || d.t > 1e-6f || d.r > 1e-6f || d.b > 1e-6f;
}
// Some side is past the soft warning.
[[nodiscard]] inline bool extend_warns(const extend_draft& d) noexcept {
  return d.l > kExtendWarn + 1e-6f || d.t > kExtendWarn + 1e-6f || d.r > kExtendWarn + 1e-6f ||
         d.b > kExtendWarn + 1e-6f;
}

// Which edges a press grabs (bits), or the whole frame (move).
enum extend_handle : std::uint8_t {
  extend_none = 0,
  extend_left = 1,
  extend_top = 2,
  extend_right = 4,
  extend_bottom = 8,
  extend_move = 16,
};

// Each side held to [-(1 - keep - the other side's crop), kExtendMax]: never
// past the hard stop, and at least kExtendMinKeep of the photo stays inside.
[[nodiscard]] inline extend_draft clamp_extend(extend_draft d) noexcept {
  const auto fix = [](float& v) {
    if (!std::isfinite(v)) v = 0.0f;
  };
  fix(d.l), fix(d.t), fix(d.r), fix(d.b);
  const auto axis = [](float& a, float& c) {
    a = std::min(a, kExtendMax);
    c = std::min(c, kExtendMax);
    // The crop part's width: min(1, 1 + c) - max(0, -a) >= keep.
    a = std::max(a, kExtendMinKeep - std::min(1.0f, 1.0f + c));
    c = std::max(c, kExtendMinKeep - std::min(1.0f, 1.0f + a));
  };
  axis(d.l, d.r);
  axis(d.t, d.b);
  return d;
}

// The frame in the shown picture's uv: x, y, w, h (may reach past 0..1).
[[nodiscard]] inline std::array<float, 4> extend_rect(const extend_draft& d) noexcept {
  return {-d.l, -d.t, 1.0f + d.l + d.r, 1.0f + d.t + d.b};
}
// The part of the frame over the photo, in its uv: the crop Fill lands on the
// edit (the whole picture when no side goes inside).
[[nodiscard]] inline std::array<float, 4> extend_crop(const extend_draft& d) noexcept {
  const float x0 = std::max(0.0f, -d.l), y0 = std::max(0.0f, -d.t);
  const float x1 = std::min(1.0f, 1.0f + d.r), y1 = std::min(1.0f, 1.0f + d.b);
  return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}
// Whether Fill crops the photo first (some side inside it).
[[nodiscard]] inline bool extend_crops(const extend_draft& d) noexcept {
  return d.l < -1e-6f || d.t < -1e-6f || d.r < -1e-6f || d.b < -1e-6f;
}
// The margins past the cropped picture's edges as fractions of IT (what
// still_extend hands Enhance once the crop is on the edit): left, top,
// right, bottom.
[[nodiscard]] inline std::array<float, 4> extend_overhang(const extend_draft& d) noexcept {
  const std::array<float, 4> c = extend_crop(d);
  const auto over = [](float side, float of) { return side > 0.0f && of > 0.0f ? side / of : 0.0f; };
  return {over(d.l, c[2]), over(d.t, c[3]), over(d.r, c[2]), over(d.b, c[3])};
}

// What the camera frames while Extend is armed: the photo and the frame
// together, kExtendFitPad of their size around them (shown uv; x, y, w, h).
[[nodiscard]] inline std::array<float, 4> extend_fit(const extend_draft& d) noexcept {
  const std::array<float, 4> f = extend_rect(d);
  const float x0 = std::min(0.0f, f[0]), y0 = std::min(0.0f, f[1]);
  const float x1 = std::max(1.0f, f[0] + f[2]), y1 = std::max(1.0f, f[1] + f[3]);
  const float pw = (x1 - x0) * kExtendFitPad, ph = (y1 - y0) * kExtendFitPad;
  return {x0 - pw, y0 - ph, x1 - x0 + 2.0f * pw, y1 - y0 + 2.0f * ph};
}

// The edges near shown uv (u, v): within `reach_u` / `reach_v` (the grab
// distance in uv, from screen pixels) of an edge of the frame, its bits (two
// at a corner); inside the frame away from the edges, extend_move; else none.
[[nodiscard]] inline std::uint8_t extend_hit(const extend_draft& d, float u, float v, float reach_u,
                                             float reach_v) noexcept {
  const std::array<float, 4> f = extend_rect(d);
  const float x0 = f[0], y0 = f[1], x1 = f[0] + f[2], y1 = f[1] + f[3];
  if (u < x0 - reach_u || u > x1 + reach_u || v < y0 - reach_v || v > y1 + reach_v) return extend_none;
  std::uint8_t bits = 0;
  // The nearer edge wins on a frame thinner than two reaches.
  if (std::fabs(u - x0) <= reach_u && std::fabs(u - x0) <= std::fabs(u - x1)) bits |= extend_left;
  else if (std::fabs(u - x1) <= reach_u) bits |= extend_right;
  if (std::fabs(v - y0) <= reach_v && std::fabs(v - y0) <= std::fabs(v - y1)) bits |= extend_top;
  else if (std::fabs(v - y1) <= reach_v) bits |= extend_bottom;
  if (bits != 0) return bits;
  return u > x0 && u < x1 && v > y0 && v < y1 ? extend_move : extend_none;
}

// The draft a drag of `handle` by (du, dv) (shown uv, from the press) makes of
// `start`, clamped. A move keeps the frame's size: both sides of an axis
// shift together, as far as the stops let the nearer one go.
[[nodiscard]] inline extend_draft extend_drag(const extend_draft& start, std::uint8_t handle, float du,
                                              float dv) noexcept {
  extend_draft d = start;
  if (!std::isfinite(du)) du = 0.0f;
  if (!std::isfinite(dv)) dv = 0.0f;
  if (handle & extend_move) {
    const auto shift = [](float& a, float& c, float delta) {
      // a = -x0 and c = x1 - 1 for a frame [x0, x1] of fixed size w: held so
      // neither side passes the stop and kExtendMinKeep of the photo stays in.
      const float w = 1.0f + a + c;
      const float lo = std::max(w - 1.0f - kExtendMax, kExtendMinKeep - 1.0f);
      const float hi = std::min(kExtendMax, w - kExtendMinKeep);
      if (lo <= hi) a = std::clamp(a - delta, lo, hi);
      c = w - 1.0f - a;
    };
    shift(d.l, d.r, du);
    shift(d.t, d.b, dv);
    d.on = start.on;
    return clamp_extend(d);
  }
  if (handle & extend_left) d.l = start.l - du;
  if (handle & extend_right) d.r = start.r + du;
  if (handle & extend_top) d.t = start.t - dv;
  if (handle & extend_bottom) d.b = start.b + dv;
  return clamp_extend(d);
}

// What Fill makes of a draft over a `w` x `h` shown picture (pixels): the crop
// part's size, the margins Enhance adds to it (its extend_from_fractions
// sums), the result's size and its growth over the photo in permille
// (area; +180 reads "+18 %").
struct extend_size_t {
  std::uint32_t crop_w = 0, crop_h = 0;
  std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
  std::uint32_t width = 0, height = 0;
  std::int32_t growth_permille = 0;
};

// The margins Enhance gives fractions (l, t, r, b) of a w x h picture: the
// twin of src/addons/enhance/remove.cpp extend_from_fractions, sum for sum.
[[nodiscard]] inline std::array<std::uint32_t, 4> extend_margins_of(const std::array<float, 4>& f, std::uint32_t w,
                                                                    std::uint32_t h) noexcept {
  std::array<std::uint32_t, 4> m{};  // left, top, right, bottom
  if (w == 0 || h == 0) return m;
  const auto px = [](float v, std::uint32_t side) -> std::uint32_t {
    if (!(v > 0.0f)) return 0;
    return static_cast<std::uint32_t>(std::floor(std::min(static_cast<double>(v), 1.0) * side + 0.5));
  };
  m[0] = px(f[0], w);
  m[2] = px(f[2], w);
  m[1] = px(f[1], h);
  m[3] = px(f[3], h);
  const auto cap = [](std::uint32_t& a, std::uint32_t& c, std::uint32_t side) {
    const std::uint32_t most = std::min(side, kExtendMaxEdge > side ? kExtendMaxEdge - side : 0u);
    if (a + c > most) {
      const double k = static_cast<double>(most) / (a + c);
      a = static_cast<std::uint32_t>(a * k);
      c = static_cast<std::uint32_t>(c * k);
    }
  };
  cap(m[0], m[2], w);
  cap(m[1], m[3], h);
  const auto area = [&] { return std::uint64_t{w + m[0] + m[2]} * std::uint64_t{h + m[1] + m[3]}; };
  if (area() > kExtendMaxPixels) {
    if (std::uint64_t{w} * h >= kExtendMaxPixels) return {};
    const double ax = static_cast<double>(m[0] + m[2]), ay = static_cast<double>(m[1] + m[3]);
    const double qa = ax * ay, qb = static_cast<double>(w) * ay + static_cast<double>(h) * ax;
    const double qc = static_cast<double>(w) * h - static_cast<double>(kExtendMaxPixels);
    double k = qa > 0.0 ? (-qb + std::sqrt(qb * qb - 4.0 * qa * qc)) / (2.0 * qa) : qb > 0.0 ? -qc / qb : 0.0;
    k = std::clamp(k, 0.0, 1.0);
    for (std::uint32_t& v : m) v = static_cast<std::uint32_t>(v * k);
    // Enhance takes the largest of left, top, right, bottom first (ties: that order).
    for (int guard = 0; area() > kExtendMaxPixels && guard < 4096; ++guard) {
      std::uint32_t* most = &m[0];
      for (int i : {1, 2, 3}) {
        if (m[static_cast<std::size_t>(i)] > *most) most = &m[static_cast<std::size_t>(i)];
      }
      if (*most == 0) return {};
      --*most;
    }
  }
  return m;
}

[[nodiscard]] inline extend_size_t extend_size(const extend_draft& d, std::uint32_t w, std::uint32_t h) noexcept {
  extend_size_t s;
  if (w == 0 || h == 0) return s;
  const std::array<float, 4> c = extend_crop(d);
  // A crop's size as place() rounds it.
  s.crop_w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(c[2] * static_cast<float>(w))));
  s.crop_h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(c[3] * static_cast<float>(h))));
  if (!extend_crops(d)) {
    s.crop_w = w;
    s.crop_h = h;
  }
  const std::array<std::uint32_t, 4> m = extend_margins_of(extend_overhang(d), s.crop_w, s.crop_h);
  s.left = m[0], s.top = m[1], s.right = m[2], s.bottom = m[3];
  s.width = s.crop_w + s.left + s.right;
  s.height = s.crop_h + s.top + s.bottom;
  const double grown = static_cast<double>(s.width) * s.height / (static_cast<double>(w) * h) - 1.0;
  s.growth_permille = static_cast<std::int32_t>(std::lround(grown * 1000.0));
  return s;
}

// The Enhance add-on's presets as chips on the bar (its extend_preset, the
// same sums as fractions of the shown picture), after Original (no frame).
// Indices are the bar's wire values (tool_bar_action::extend_preset + i).
inline constexpr int kExtendPresetCount = 7;
[[nodiscard]] constexpr const char* extend_preset_label(int i) noexcept {
  constexpr const char* kLabels[kExtendPresetCount] = {"Original", "Wider", "Taller", "All round", "1:1", "16:9", "4:5"};
  return i >= 0 && i < kExtendPresetCount ? kLabels[i] : "";
}
// The add-on's name for preset `i` ("" for Original).
[[nodiscard]] constexpr const char* extend_preset_name(int i) noexcept {
  constexpr const char* kNames[kExtendPresetCount] = {"", "wider", "taller", "all", "square", "16x9", "4x5"};
  return i >= 0 && i < kExtendPresetCount ? kNames[i] : "";
}
// The draft preset `i` sets over a w x h picture (src/addons/enhance/remove.cpp
// extend_preset's margins over w and h; Original is no frame).
[[nodiscard]] inline extend_draft extend_preset_draft(int i, std::uint32_t w, std::uint32_t h) noexcept {
  extend_draft d;
  d.on = true;
  if (w == 0 || h == 0 || i <= 0 || i >= kExtendPresetCount) return d;
  std::uint32_t l = 0, t = 0, r = 0, b = 0;
  const auto to_aspect = [&](double aspect) {
    const double now = static_cast<double>(w) / h;
    if (aspect > now) {
      const auto extra = static_cast<std::uint32_t>(h * aspect - w);
      l = extra / 2;
      r = extra - l;
    } else if (aspect < now) {
      const auto extra = static_cast<std::uint32_t>(w / aspect - h);
      t = extra / 2;
      b = extra - t;
    }
  };
  switch (i) {
    case 1: l = r = w / 8; break;
    case 2: t = b = h / 8; break;
    case 3: l = r = w / 10, t = b = h / 10; break;
    case 4: to_aspect(1.0); break;
    case 5: to_aspect(16.0 / 9.0); break;
    case 6: to_aspect(4.0 / 5.0); break;
    default: break;
  }
  const auto cap = [](std::uint32_t& a, std::uint32_t& c, std::uint32_t side) {
    const std::uint32_t most = std::min(side, kExtendMaxEdge > side ? kExtendMaxEdge - side : 0u);
    if (a + c > most) {
      const double k = static_cast<double>(most) / (a + c);
      a = static_cast<std::uint32_t>(a * k);
      c = static_cast<std::uint32_t>(c * k);
    }
  };
  cap(l, r, w);
  cap(t, b, h);
  const auto fw = static_cast<float>(w), fh = static_cast<float>(h);
  d.l = static_cast<float>(l) / fw;
  d.r = static_cast<float>(r) / fw;
  d.t = static_cast<float>(t) / fh;
  d.b = static_cast<float>(b) / fh;
  return d;
}
// The preset chip a draft is (-1: a frame of the person's own).
[[nodiscard]] inline int extend_preset_of(const extend_draft& d, std::uint32_t w, std::uint32_t h) noexcept {
  if (extend_empty(d)) return 0;
  for (int i = 1; i < kExtendPresetCount; ++i) {
    const extend_draft p = extend_preset_draft(i, w, h);
    if (std::fabs(p.l - d.l) < 1e-5f && std::fabs(p.t - d.t) < 1e-5f && std::fabs(p.r - d.r) < 1e-5f &&
        std::fabs(p.b - d.b) < 1e-5f) {
      return i;
    }
  }
  return -1;
}

}  // namespace mv::shell
