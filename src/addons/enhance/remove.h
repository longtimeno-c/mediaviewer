// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Remove (docs/plans/edit-adjust.md slice 7, owner 2026-11-03): what
// the person brushed over, filled in from its surroundings by an inpainting
// model (LaMa, a fixed square input) on a crop of the full-resolution photo.
//
//   1. remove_box: the mask's bounds, grown for context and made square where
//      the photo allows.
//   2. The crop and its mask scaled to the model's side, filled, scaled back.
//   3. paste_back: only the brushed pixels (feathered a few pixels outward)
//      replaced; everything outside the box is the photo's own.
//
// The model is not here: these are the steps around it, so they are tested
// without it. The runner's thread only (rule 1).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/enhance/diffusion.h"

namespace mv::enhance {

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

[[nodiscard]] rgb8 crop_rgb(const rgb8& src, const pixel_box& box);
// The mask's crop scaled to width x height, nearest, then 0 or 255.
[[nodiscard]] mask8 crop_mask(const mask8& src, const pixel_box& box, std::uint32_t width, std::uint32_t height);

// `filled` (the box's size) over `photo` where the mask is brushed, blended
// over `feather` pixels outward. Pixels farther than 2 * feather from a
// brushed one are left exactly as they were.
void paste_back(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& mask, std::uint32_t feather);

// ---- Extend the picture (owner 2026-11-04: "recropping that extends") ----
// New margins around the photo, in pixels, filled from it by the inpainting
// model. A preset ("wider", "taller", "all", "square", "16x9", "4x5") or the
// host's own drag (later) gives them.
struct extend_margins {
  std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
  [[nodiscard]] bool empty() const noexcept { return left + top + right + bottom == 0; }
};
// The largest a picture grows: twice its size each way, and 16384 a side.
[[nodiscard]] extend_margins extend_preset(std::string_view preset, std::uint32_t w, std::uint32_t h) noexcept;
// The photo in the middle of a bigger picture, the new margins filled with
// its nearest edge pixel (a start the model refines), and the mask of them.
void extend_canvas(const rgb8& photo, const extend_margins& m, rgb8& out, mask8& mask);
// Overlapping square boxes along the margins, each half margin half photo
// (context for the model), in the order they are filled: each sees the ones
// before it as photo.
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
