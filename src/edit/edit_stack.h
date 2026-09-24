// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10 — the EditStack (plan/07-photo-editing.md) and its geometry ops.
//
// An edit is an ordered list of small POD parameter blocks. The original is
// never modified by the stack; undo pops, reset clears. Geometry is folded
// into one canonical `geometry` so that preview, export and the lossless JPEG
// path all agree on what the ops mean:
//
//   source → D4 (rotate / flip) → straighten about the centre → crop → resize
//
// This is plan/07's "crop, straighten, rotate, flip" order read the other way
// round (the ops commute once the crop and angle are carried through the D4,
// which `fold` does). Colour ops (PR 11) append to the same stack after
// geometry; nothing here knows about pixels or a GPU.
//
// Straighten, resize and exact crop change pixels; rotate, flip and an
// MCU-aligned crop of a JPEG do not (lossless_jpeg.h).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/orientation.h"

namespace mv::edit {

// A rectangle normalised to the frame it is set in: 0..1, top-left origin.
struct rect {
  float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
  friend constexpr bool operator==(const rect&, const rect&) = default;
};

enum class resize_mode : std::uint8_t {
  none = 0,
  long_edge,  // `a` = the long edge in pixels; aspect kept
  exact,      // `a` x `b` pixels, aspect not kept
  percent,    // `percent` of the cropped size
};

struct resize_spec {
  resize_mode mode = resize_mode::none;
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  float percent = 100.0f;
  friend constexpr bool operator==(const resize_spec&, const resize_spec&) = default;
};

enum class op_kind : std::uint8_t {
  rotate_cw = 0,
  rotate_ccw,
  flip_h,
  flip_v,
  crop,        // set the crop to `crop`, in the straightened (uncropped) frame
  straighten,  // set the straighten angle to `degrees`, clockwise, ±kMaxStraighten
  resize,      // set the output size to `resize`
};

// One parameter block. POD, so a stack copies, compares and serialises as
// plain data; only the field for `kind` is meaningful.
struct op {
  op_kind kind = op_kind::rotate_cw;
  rect crop{};
  float degrees = 0.0f;
  resize_spec resize{};
};

inline constexpr float kMaxStraighten = 45.0f;

struct edit_stack {
  // Identifies the original the ops were made against (path hash + size +
  // mtime from the host). A stack is never applied to another source.
  std::uint64_t source_id = 0;
  std::vector<op> ops;
  int version = 1;

  [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
  void push(const op& o) { ops.push_back(o); }
  // Undo is popping the list (plan/07). False when there is nothing to undo.
  bool undo() noexcept {
    if (ops.empty()) return false;
    ops.pop_back();
    return true;
  }
  // Reset returns the original exactly: no op, no pixels touched.
  void reset() noexcept { ops.clear(); }
};

// The stack folded to its canonical form.
struct geometry {
  codec::d4 orient{};        // user rotate/flip on top of what is displayed
  float straighten = 0.0f;   // degrees, clockwise
  rect crop{};               // in the straightened, uncropped frame
  resize_spec resize{};

  [[nodiscard]] bool identity() const noexcept {
    return orient.identity() && straighten == 0.0f && crop == rect{} &&
           resize.mode == resize_mode::none;
  }
  // Only rotate / flip: the lossless JPEG case from the viewer.
  [[nodiscard]] bool orientation_only() const noexcept {
    return straighten == 0.0f && crop == rect{} && resize.mode == resize_mode::none;
  }
};

[[nodiscard]] geometry fold(std::span<const op> ops) noexcept;
[[nodiscard]] inline geometry fold(const edit_stack& s) noexcept { return fold(s.ops); }

// Output → source mapping, affine, in normalised coordinates:
//   source_uv = (m[0] * u + m[1] * v + m[2],  m[3] * u + m[4] * v + m[5])
// `u, v` are 0..1 across the edited output (after crop, before resize);
// `source_uv` is 0..1 across the source the geometry is applied to. This is
// the whole geometry chain for the GPU preview: a blit samples the source at
// source_uv, so rotate/flip/straighten/crop cost nothing per frame.
struct affine {
  float m[6] = {1, 0, 0, 0, 1, 0};
  [[nodiscard]] bool identity() const noexcept {
    return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 1 && m[5] == 0;
  }
};

// The inverse map (source uv → output uv). Every geometry map is invertible
// (a signed permutation, a rotation and a positive scale); a degenerate one
// comes back as the identity.
[[nodiscard]] affine invert(const affine& a) noexcept;

struct size2 {
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  friend constexpr bool operator==(const size2&, const size2&) = default;
};

// The geometry applied to a `source` of the given size, through `base` first
// (the file's EXIF orientation when the source is the file's stored pixels;
// identity when the source is the already-oriented display texture).
struct placement {
  size2 oriented{};  // source through base + orient
  size2 cropped{};   // after straighten + crop, before resize (the preview size)
  size2 output{};    // after resize (what an export writes)
  codec::d4 total{}; // base then orient: stored → output rotation
  affine map{};      // output uv → source uv
  // The crop in output pixels of the oriented frame (straighten = 0 only;
  // otherwise the rounded bounding box). What the lossless path checks.
  std::uint32_t crop_x = 0, crop_y = 0;
  // No straighten and no resize: the output is source pixels rearranged
  // (rotate / flip / crop), with no resampling.
  bool exact_copy = true;
};

// `keep_frame` places the crop as given, without constrain_crop: crop mode
// shows the whole straightened frame (corners and all) under its overlay.
[[nodiscard]] placement place(const geometry& g, size2 source, codec::d4 base = {},
                              bool keep_frame = false) noexcept;

// Clamps `r` to the frame and, when `straighten` is not zero, shrinks it until
// its corners lie inside the rotated source (no transparent corners in an
// export). The rect keeps its aspect; it shrinks about its own centre when
// that centre is inside the source, else toward the frame centre.
// `frame` is the oriented source size (the aspect matters, not the scale).
[[nodiscard]] rect constrain_crop(rect r, float straighten, size2 frame) noexcept;

// The largest rect of the frame's own aspect that fits `straighten` —
// the default crop the moment an angle is set.
[[nodiscard]] rect auto_crop(float straighten, size2 frame) noexcept;

}  // namespace mv::edit
