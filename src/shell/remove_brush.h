// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slice 7 (docs/plans/edit-adjust.md): the Remove brush's strokes over the
// shown (edited) still. Round dabs in the shown image's uv, the radius a
// fraction of its long side, so the canvas draws them through its camera
// and Enhance gets them as a mask of the edited picture it is handed. A
// geometry change or another item forgets them (they would no longer lie over
// what was brushed). Pure; the UI thread owns it.
//
// docs/plans/edit-refresh.md S4a (brush v2): the strokes are a stack of at
// most kBrushStrokes strokes and kBrushStackDabs dabs. Undo and redo take one
// stroke; Clear is one more undo step (undo puts every stroke back); an erase
// dab takes back what earlier dabs painted. The canvas draws the committed
// strokes from a tint texture a worker builds (shell/brush_mask.h); only the
// dabs that texture does not hold yet travel in the snapshot (edit_view).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mv::shell {

// The most dabs the snapshot carries live (edit_view::brush): the stroke in
// progress and whatever the mask texture does not hold yet.
inline constexpr std::size_t kBrushDabs = 512;
// S4a: what one brushing holds in all, until Remove takes it.
inline constexpr std::size_t kBrushStrokes = 64;
inline constexpr std::size_t kBrushStackDabs = 8192;
// How many Clears stay undoable (each keeps the strokes it took off).
inline constexpr std::size_t kBrushClears = 8;
// Dabs this far apart, in radii, draw a smooth stroke.
inline constexpr float kBrushSpacing = 0.5f;

struct brush_dab {
  float u = 0.0f, v = 0.0f;  // the shown image's uv
  float r = 0.0f;            // a fraction of its long side
  // docs/plans/edit-refresh.md S3: an erase dab takes back what earlier dabs
  // painted (the mask is built in order).
  std::uint8_t erase = 0;
};

class remove_brush {
 public:
  // The shown image's size, for even spacing along a stroke and for the mask.
  void set_frame(std::uint32_t width, std::uint32_t height) noexcept;

  // A press, a move with the button down, and the release. A move adds dabs
  // every kBrushSpacing radii along the way; false once the brushing is full
  // (kBrushStackDabs). begin is false when full() (kBrushStrokes strokes or
  // kBrushStackDabs dabs) or the radius is not positive.
  bool begin(float u, float v, float r, bool erase = false);
  bool drag(float u, float v);
  void end() noexcept { stroking_ = false; }

  // The last step off: a stroke, or a Clear (every stroke it took off comes
  // back). False when there is none.
  bool undo_stroke();
  // The last step undone back on (until a new stroke starts). False when
  // there is none or it no longer fits.
  bool redo_stroke();
  [[nodiscard]] bool can_undo() const noexcept { return !done_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  // Every stroke off, as one undo step. False when there was nothing.
  bool clear();
  // Forgets the strokes and their history (another item, a new geometry,
  // Remove handed them over). Not undoable.
  void reset() noexcept;

  [[nodiscard]] bool empty() const noexcept { return dabs_.empty(); }
  // Nothing brushed and nothing to undo or redo.
  [[nodiscard]] bool pristine() const noexcept { return dabs_.empty() && done_.empty() && redo_.empty(); }
  [[nodiscard]] bool full() const noexcept {
    return dabs_.size() >= kBrushStackDabs || starts_.size() >= kBrushStrokes;
  }
  [[nodiscard]] bool stroking() const noexcept { return stroking_; }
  [[nodiscard]] bool erasing() const noexcept { return stroking_ && erasing_; }
  [[nodiscard]] std::size_t strokes() const noexcept { return starts_.size(); }
  [[nodiscard]] std::span<const brush_dab> dabs() const noexcept { return dabs_; }
  // Bumped whenever dabs are taken off (undo, clear, reset, a redone Clear):
  // dabs()[0, n) of the same generation never change, so a mask built of them
  // still holds (shell/brush_mask.h).
  [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

  // What Enhance's still_mask row is handed (u, v, r per dab): the painted
  // dabs, minus any whose centre a later erase dab covers (the row has no
  // erase; the mask below is exact).
  [[nodiscard]] std::vector<float> handoff() const;

  // The mask at `width` x `height` (255 brushed, 0 not), for Enhance.
  [[nodiscard]] std::vector<std::uint8_t> mask(std::uint32_t width, std::uint32_t height) const;

 private:
  bool add(float u, float v, float r);

  // One undo step: a stroke (the last of starts_; its dabs while undone), or
  // a Clear (the strokes it took off while done).
  struct step {
    bool clear = false;
    std::vector<brush_dab> dabs;
    std::vector<std::size_t> starts;
  };

  std::vector<brush_dab> dabs_;
  std::vector<std::size_t> starts_;  // the first dab of each stroke
  std::vector<step> done_;           // undoable, last on top
  std::vector<step> redo_;           // undone, last on top
  float aspect_w_ = 1.0f, aspect_h_ = 1.0f;  // width and height over the long side
  std::uint32_t generation_ = 1;
  bool stroking_ = false;
  bool erasing_ = false;  // the stroke in progress erases
};

}  // namespace mv::shell
