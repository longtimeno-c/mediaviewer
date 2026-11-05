// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S8: the Faces tool (shell::canvas_tool::faces).
// Armed, it reads the faces of the photo on screen once (on the pool: the AI
// pack's eyes_json answer, then the still for each face's thumbnail and
// whether its eyes look shut), draws a box per face on the canvas
// (shell/tool_overlay.h) and lists them in the host's Faces panel (Mac
// FacesPanelView.swift, Windows IslandHost.Faces.cs) with the person's name
// from People and "from 37 photos of Anna". A click on a box chooses it or
// leaves it out; a fix changes only the chosen faces (addon::media::fix_faces
// with a faces_mask), and every result is a new file.
//
// The per-face fixes are data (kFaceActions): a fix is appended there with
// its addon::media::face_fix value, and the chromes list what a face's
// `actions` bits offer. S10's "Restore faces" is the next row.
//
// Portable (both hosts link it; the Windows exe reaches the pack and the fix
// through mediaviewer_core's C entries). faces_tool is UI-thread state;
// read_faces is the worker's half.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "edit/edit_stack.h"
#include "image/linear.h"
#include "shell/input_state.h"

namespace mv::shell {

// A face's thumbnail in the panel, pixels a side (56 pt at 2x, with room).
inline constexpr std::uint32_t kFaceThumbPx = 128;

// The per-face fixes, by wire value (addon::media::face_fix, mv_adjust_faces'
// `fix`). Appended only.
enum class face_action : std::uint8_t { open_eyes = 0, improve = 1 };
inline constexpr int kFaceActionCount = 2;
// "Do all" (mv_adjust_faces' -1): each chosen face's best fix, one file.
inline constexpr int kFaceDoAll = -1;

struct face_action_row {
  face_action id;
  std::string_view label;  // the panel's button
  std::string_view busy;   // the panel's note while it runs
  bool needs_shut;         // offered only on a face whose eyes look shut
};
inline constexpr std::array<face_action_row, kFaceActionCount> kFaceActions{{
    {face_action::open_eyes, "Open eyes", "Opening eyes from another photo…", true},
    {face_action::improve, "Improve face", "Looking for a better photo of this face…", false},
}};

// What the panel's and the bar's controls send (chrome_cmd_faces' argument,
// the Mac's mv_chrome_faces_op): an op, a face (-1: the chosen ones) and a
// fix (face_action, or kFaceDoAll).
enum class faces_op : std::uint8_t {
  run = 0,         // `action` on `face` (or the chosen faces)
  toggle = 1,      // choose `face` or leave it out
  choose_all = 2,  // every face chosen
  choose_none = 3,
  people = 4,      // turn People on (the chrome opens its settings)
};
struct faces_op_args {
  faces_op op = faces_op::run;
  int face = -1;
  int action = 0;
};
// op × 100000 + (face + 1) × 100 + (action + 1): fits a float exactly.
[[nodiscard]] constexpr std::int32_t pack_faces_op(faces_op op, int face, int action) noexcept {
  return static_cast<std::int32_t>(op) * 100000 + (face + 1) * 100 + (action + 1);
}
[[nodiscard]] constexpr std::optional<faces_op_args> unpack_faces_op(std::int32_t v) noexcept {
  if (v < 0) return std::nullopt;
  const int op = v / 100000, face = (v / 100) % 1000 - 1, action = v % 100 - 1;
  if (op > static_cast<int>(faces_op::people) || face < -1 || face >= kToolFaces || action < kFaceDoAll ||
      action >= kFaceActionCount) {
    return std::nullopt;
  }
  return faces_op_args{static_cast<faces_op>(op), face, action};
}

// One face of the photo, as the panel and the canvas show it.
struct face_card {
  std::array<float, 4> box{};     // the pack's x, y, w, h: 0..1 of the upright still
  std::array<float, 4> region{};  // edit::face_region: drawn on the canvas; a fix changes nothing outside it
  std::array<float, 10> lm{};
  std::int64_t person = 0;  // People's cluster, 0 none
  std::string name;         // People's name, "" unnamed
  std::uint32_t photos = 0; // People's other photos of them (the donors' pool)
  bool shut = false;        // the eyes look shut (edit::eyes_look_shut)
  std::uint32_t thumb_w = 0, thumb_h = 0;
  std::vector<std::uint8_t> thumb;  // sRGB RGBA8, opaque
};

// The fixes a face offers, a bit per face_action (kFaceActions' rule).
[[nodiscard]] std::uint32_t actions_of(const face_card& f) noexcept;

// Where the read stands (the panel's empty states; wire values for both chromes).
enum class faces_status : std::uint8_t {
  idle = 0,        // not read (the tool is down)
  looking = 1,     // on the pool
  ready = 2,       // read: `faces` (maybe none)
  no_pack = 3,     // Local search is not installed
  people_off = 4,  // People is off in Local search
  failed = 5,      // the still did not read
};

struct faces_scan {
  faces_status status = faces_status::failed;
  std::vector<face_card> faces;
};

// Worker: the cards from the pack's answer (`pack` the call's status:
// not_found no pack, invalid_arg People off) and the upright still it was
// asked about (`still` may be null: no thumbnails, nothing reads as shut).
// At most kToolFaces faces, the pack's order (faces_mask's bits).
[[nodiscard]] faces_scan read_faces(status pack, std::string_view pack_json, const image::linear_image* still,
                                    std::uint32_t thumb_px = kFaceThumbPx);

// The tool's state on the UI thread.
struct faces_tool {
  std::uint64_t item = 0;  // the photo `faces` belong to (0 none)
  faces_status status = faces_status::idle;
  std::vector<face_card> faces;
  std::uint32_t chosen = 0;  // bit i: face i is chosen
  int hover = -1;            // the face under the pointer
  bool running = false;      // a fix is on the pool
  int running_face = -1;     // -1: the chosen faces
  int running_action = 0;    // face_action, or kFaceDoAll
  std::uint32_t revision = 0;  // bumps with every change the panel shows

  // A read starts for `photo`: what was there goes.
  void looking(std::uint64_t photo) noexcept;
  // The read for `photo` landed; every face starts chosen. False when it is
  // for another photo (dropped).
  bool landed(std::uint64_t photo, faces_scan scan);
  // Forget everything (the tool went down on another photo, a walk).
  void clear() noexcept;
  [[nodiscard]] int count() const noexcept { return static_cast<int>(faces.size()); }
  [[nodiscard]] int chosen_count() const noexcept;
  [[nodiscard]] bool is_chosen(int i) const noexcept { return i >= 0 && i < 32 && ((chosen >> i) & 1u) != 0u; }
  bool toggle(int i) noexcept;
  void choose_all() noexcept;
  void choose_none() noexcept;
  bool set_hover(int i) noexcept;
  // The boxes (x, y, w, h each) of `face` (or the chosen faces for -1) that a
  // run sends: the run's own answer is matched to them (media::faces_mask_of).
  [[nodiscard]] std::vector<float> run_boxes(int face) const;
};

// A face region (0..1 of the upright still) as the shown picture's uv rect
// (x, y, w, h): the bounds of its corners through the placement's map,
// inverted (a straightened photo turns it).
[[nodiscard]] std::array<float, 4> region_to_shown(const edit::placement& p, const std::array<float, 4>& r) noexcept;
// The face under shown uv (u, v), the smallest region holding it; -1 none.
[[nodiscard]] int face_at(const faces_tool& t, const edit::placement& p, float u, float v) noexcept;
// The boxes onto the canvas's edit view (the overlay draws them while the
// Faces tool is armed); `busy` marks the faces a fix is running on.
void apply_faces(edit_view& v, const faces_tool& t, const edit::placement& p) noexcept;
// The bar's words: "3 faces · 2 chosen", "Looking for faces…", ….
[[nodiscard]] std::string faces_status_line(const faces_tool& t);
// The panel's "from 37 photos of Anna" ("" when People holds nothing more).
[[nodiscard]] std::string face_source_line(const face_card& f);

}  // namespace mv::shell
