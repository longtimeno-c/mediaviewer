// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Remove (docs/plans/edit-adjust.md slice 7, owner 2026-11-03): what
// the person brushed over, filled in from its surroundings by an inpainting
// model (LaMa, a fixed square input) on a crop of the full-resolution photo.
//
//   1. remove_boxes: one box per blob brushed apart (remove_box's bounds,
//      grown for context and made square where the photo allows).
//   2. Each box's crop and its mask scaled to the model's side, filled,
//      scaled back.
//   3. A box over 1.5x the model's side filled again at full resolution on
//      model-side tiles (refine_fill), blended by weight.
//   4. paste_back_box: only that box's brushed pixels (feathered a few pixels
//      outward) replaced; everything outside the boxes is the photo's own.
//
// The model is not here: these are the steps around it, so they are tested
// without it. The runner's thread only (rule 1).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/enhance/diffusion.h"

namespace mv::enhance {

class inpaint_filler;

// 8-bit, the photo's size; over 127 is brushed.
struct mask8 {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> a;  // width * height
};

struct pixel_box {
  std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Context on each side: half the brushed area's longer side, at least
// kRemoveMinContext. nullopt when nothing is brushed.
inline constexpr std::uint32_t kRemoveMinContext = 128;
[[nodiscard]] std::optional<pixel_box> remove_box(const mask8& mask);

// Several things brushed apart, each filled in its own box (edit-refresh S4b):
// two dust spots at opposite corners are two small boxes at the model's
// detail, not one photo-sized box filled at 512 px and scaled up.
//
// The mask is downscaled to a grid of at most kRemoveGrid cells a side (a
// cell brushed when any of its pixels is), dilated 2 cells, and split into
// 8-connected blobs. Each blob gets remove_box's grown box of its own exact
// bounds. Two boxes whose union is under 1.3x their summed area are one
// (their bounds joined and grown again), until none merge. More than
// kRemoveMaxBoxes boxes left (or more than kRemoveMaxBlobs blobs to merge):
// one remove_box over everything, as before.
inline constexpr std::uint32_t kRemoveGrid = 256;
inline constexpr std::size_t kRemoveMaxBoxes = 8;
inline constexpr std::size_t kRemoveMaxBlobs = 64;
struct remove_plan {
  std::vector<pixel_box> boxes;  // in the order they are filled (top to bottom, then left to right)
  // The blob grid: each cell a `cell` x `cell` square of the photo; `owner`
  // the box (index + 1) filling its brushed pixels, 0 where nothing is.
  std::uint32_t cell = 1, grid_w = 0, grid_h = 0;
  std::vector<std::uint8_t> owner;
  [[nodiscard]] std::uint8_t owner_at(std::uint32_t x, std::uint32_t y) const noexcept {
    return owner[std::size_t{y / cell} * grid_w + x / cell];
  }
};
// No boxes when nothing is brushed.
[[nodiscard]] remove_plan remove_boxes(const mask8& mask);
// Box `i`'s mask, the box's size: `open` false, the brushed pixels it fills
// (pasted back); true, those and the ones later boxes fill (still holes when
// it runs, so the model is never shown what is being taken out).
[[nodiscard]] mask8 box_mask(const mask8& mask, const remove_plan& plan, std::size_t i, bool open);

[[nodiscard]] rgb8 crop_rgb(const rgb8& src, const pixel_box& box);
// The mask's crop scaled to width x height, nearest, then 0 or 255.
[[nodiscard]] mask8 crop_mask(const mask8& src, const pixel_box& box, std::uint32_t width, std::uint32_t height);

// The paste-back feather for a box: grows with it, 2 to 16 pixels.
[[nodiscard]] std::uint32_t remove_feather(const pixel_box& box) noexcept;

// ---- Quality over speed (owner 2026-11-04) ----
// A box over 1.5x the model's side was filled at a fraction of its size (a
// 1500-px box at 512 is three times softer than the photo). A second pass
// fills it again at full resolution: model-side tiles, half overlapping,
// over the brushed pixels the box pastes back (and their feather), blended
// by weight as Extend's are, for the detail.
[[nodiscard]] bool needs_refine(const pixel_box& box, std::uint32_t side) noexcept;
// The second pass's tiles for box `i` of `plan`: side x side (smaller only
// where the photo is), inside the photo, each holding some of the pixels the
// box pastes back or their feather, and only those refine_fill runs (some of
// box i's open mask, more than a tenth known), so a run's progress counts
// fills; none when the box does not need it.
[[nodiscard]] std::vector<pixel_box> refine_tiles(const mask8& mask, const remove_plan& plan, std::size_t i,
                                                  std::uint32_t side);
// `filled` (box `i`'s size, the first pass's fill scaled back) given the
// detail the first pass could not keep: each tile is filled at full
// resolution against `photo` (its holes box i's open mask, box_mask's rule),
// and what it has above the first pass's scale (the fill less the fill
// shrunk by that scale and grown back) is added, the weighted mean of the
// tiles over each pixel (a tent, highest in a tile's middle, times how much
// of the tile was known; a tile under a tenth known is not run). The first
// pass keeps the shape and colour (it saw the whole box; a tile deep in a
// large hole sees only what is near it, measured 2026-11-05), the tiles give
// the grain, and where they disagree only their fine detail averages, never
// two shapes. `next()` runs before each tile; false cancels.
[[nodiscard]] expected refine_fill(const rgb8& photo, const mask8& mask, const remove_plan& plan, std::size_t i,
                                   const std::vector<pixel_box>& tiles, inpaint_filler& model, rgb8& filled,
                                   const std::function<bool()>& next);

// `filled` (the box's size) over `photo` where the mask is brushed, blended
// over `feather` pixels outward. Pixels farther than 2 * feather from a
// brushed one are left exactly as they were.
void paste_back(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& mask, std::uint32_t feather);
// The same with `own`, a mask of the box's size (box_mask's).
void paste_back_box(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& own, std::uint32_t feather);

// ---- Extend the picture (owner 2026-11-04: "recropping that extends") ----
// New margins around the photo, in pixels, filled from it by the inpainting
// model. A preset ("wider", "taller", "all", "square", "16x9", "4x5") or the
// host's own drag (extend_from_fractions) gives them.
struct extend_margins {
  std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
  [[nodiscard]] bool empty() const noexcept { return left + top + right + bottom == 0; }
};
// The largest a picture grows: twice its size each way, and 16384 a side.
[[nodiscard]] extend_margins extend_preset(std::string_view preset, std::uint32_t w, std::uint32_t h) noexcept;
// edit-refresh S6: the host's own frame, dragged past the photo's edges on
// the canvas (the host table's still_extend): each margin a fraction of the
// w x h picture still_open answers with (left and right of its width, top and
// bottom of its height), rounded to whole pixels, then held to extend_preset's
// caps (twice the size each way, 16384 a side) and to kExtendMaxPixels in all
// (the margins shrunk evenly). The host's readout (shell/edit_workspace.h
// extend_size) does the same sums, so the file is the size it previewed.
inline constexpr std::uint32_t kExtendMaxEdge = 16384;
inline constexpr std::uint64_t kExtendMaxPixels = 100'000'000;
[[nodiscard]] extend_margins extend_from_fractions(float l, float t, float r, float b, std::uint32_t w,
                                                   std::uint32_t h) noexcept;
// The photo in the middle of a bigger picture, the new margins filled with
// its nearest edge pixel (a start the model refines), and the mask of them.
void extend_canvas(const rgb8& photo, const extend_margins& m, rgb8& out, mask8& mask);
// Overlapping square boxes along the margins of a w x h picture (the
// extended one), each reaching into the photo for context: a side three
// times the margin's depth, at least kExtendMinTile where the picture allows
// (quality over speed, owner 2026-11-04), two-thirds overlapping along the
// margin, with two more anchored at each end of it, of other sizes (larger
// where the picture has room, else smaller), so that every margin pixel is
// in at least three tiles (kExtendCover) for extend_fill to blend.
inline constexpr std::uint32_t kExtendMinTile = 768;
inline constexpr std::uint32_t kExtendCover = 3;
[[nodiscard]] std::vector<pixel_box> extend_tiles(std::uint32_t w, std::uint32_t h, const extend_margins& m);

// The host's brush (u, v, r triples: the picture's uv, the radius over its
// longer side) as a mask of a width x height picture.
[[nodiscard]] mask8 mask_from_dabs(const std::vector<float>& dabs, std::uint32_t width, std::uint32_t height);

// The model's tensors, from its spec's io: a square `side`, the picture and
// mask inputs (NCHW, 0..1, the mask 1 where brushed) and the filled output,
// whose values `out_scale` takes to 0..255 (LaMa's port already gives
// 0..255, measured: 1).
struct inpaint_io {
  std::uint32_t side = 512;
  std::string image = "image", mask = "mask", output = "output";
  float out_scale = 1.0f;
};

// What the runner fills with (a stand-in in the tests).
class inpaint_filler {
 public:
  virtual ~inpaint_filler() = default;
  [[nodiscard]] virtual std::uint32_t side() const noexcept = 0;
  // `image` and `mask` are side x side.
  [[nodiscard]] virtual result<rgb8> fill(const rgb8& image, const mask8& mask) = 0;
};

// The margins of `canvas` (where `mask` is set) filled through `tiles`: each
// tile filled at the model's side against the photo only, and every margin
// pixel the weighted mean of the tiles over it (weights highest at a tile's
// middle), so neighbouring tiles fade into each other with no seam. The
// photo's own pixels are not touched. `progress(done, total)` false cancels.
[[nodiscard]] expected extend_fill(rgb8& canvas, const mask8& mask, const std::vector<pixel_box>& tiles,
                                   inpaint_filler& model, const std::function<bool(std::size_t, std::size_t)>& progress);

class inpaint_model final : public inpaint_filler {
 public:
  ~inpaint_model() override;
  [[nodiscard]] static result<std::unique_ptr<inpaint_model>> open(infer::runtime& rt, const std::string& file,
                                                                   const inpaint_io& io);
  [[nodiscard]] std::uint32_t side() const noexcept override;
  [[nodiscard]] result<rgb8> fill(const rgb8& image, const mask8& mask) override;

 private:
  struct impl;
  explicit inpaint_model(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

}  // namespace mv::enhance
