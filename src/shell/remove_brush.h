// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slice 7 (docs/plans/edit-adjust.md): the Remove brush's strokes over the
// shown (edited) still. Round dabs in the shown image's uv, the radius a
// fraction of its long side, so the canvas draws them through its camera
// and Enhance gets them as a mask of the edited picture it is handed. A
// geometry change or another item clears them (they would no longer lie over
// what was brushed). Pure; the UI thread owns it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mv::shell {

// The most dabs a brushing holds (and the canvas draws): edit_view carries
// them by value.
inline constexpr std::size_t kBrushDabs = 512;

struct brush_dab {
  float u = 0.0f, v = 0.0f;  // the shown image's uv
  float r = 0.0f;            // a fraction of its long side
};

class remove_brush {
 public:
  // The shown image's size, for even spacing along a stroke and for the mask.
  void set_frame(std::uint32_t width, std::uint32_t height) noexcept;

  // A press, a move with the button down, and the release. A move adds dabs
  // every few tenths of a radius along the way; false once the brushing is
  // full (kBrushDabs).
  bool begin(float u, float v, float r);
  bool drag(float u, float v);
  void end() noexcept { stroking_ = false; }

  // The last stroke off (one undo step a stroke). False when there is none.
  bool undo_stroke();
  void clear() noexcept;

  [[nodiscard]] bool empty() const noexcept { return dabs_.empty(); }
  [[nodiscard]] bool full() const noexcept { return dabs_.size() >= kBrushDabs; }
  [[nodiscard]] std::size_t strokes() const noexcept { return starts_.size(); }
  [[nodiscard]] std::span<const brush_dab> dabs() const noexcept { return dabs_; }

  // The mask at `width` x `height` (255 brushed, 0 not), for Enhance.
  [[nodiscard]] std::vector<std::uint8_t> mask(std::uint32_t width, std::uint32_t height) const;

 private:
  bool add(float u, float v, float r);

  std::vector<brush_dab> dabs_;
  std::vector<std::size_t> starts_;  // the first dab of each stroke
  float aspect_w_ = 1.0f, aspect_h_ = 1.0f;  // width and height over the long side
  bool stroking_ = false;
};

}  // namespace mv::shell
