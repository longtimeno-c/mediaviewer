// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a post is (docs/design/30): up to 20 chosen items, in order, one shape
// for them all, and how each is framed in that shape. No I/O and no host: the
// engine owns one, the tests drive it directly.
//
// A frame is kept apart from the shape on purpose. It says where the person
// looked (a centre and a zoom over the picture), so the shape can change
// along the way and every item keeps its framing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mv::instagram {

inline constexpr std::size_t kMaxChosen = 20;

enum class shape : std::uint8_t { square, portrait, tall, widescreen, wide };

struct shape_info {
  shape s;
  std::string_view id;     // the choice's id
  std::string_view label;  // the words on the control
  std::uint32_t out_w;     // what an export is, in pixels
  std::uint32_t out_h;
};

[[nodiscard]] const shape_info& info_of(shape s) noexcept;
[[nodiscard]] std::span<const shape_info> shapes() noexcept;
[[nodiscard]] std::optional<shape> shape_from(std::string_view id) noexcept;
[[nodiscard]] double aspect_of(shape s) noexcept;
// A tile in the strip, in points: one height for the tall shapes, and a wide
// post no wider than the strip can carry.
struct tile_size {
  std::int32_t w;
  std::int32_t h;
};
[[nodiscard]] tile_size tile_of(shape s) noexcept;

// The Edit workspace's colour parameters, by wire id (edit/adjust.h): Light,
// Colour, Black & White and Vignette. Applied when a tile is drawn and when
// the post is exported, so no file is written to try a look.
inline constexpr std::size_t kAdjustValues = 16;
using adjustments = std::array<float, kAdjustValues>;
[[nodiscard]] bool adjusted(const adjustments& a) noexcept;

struct slider_info {
  std::uint32_t wire;
  std::string_view label;
  double min, max, step;
  int section;   // 0 Light, 1 Colour, 2 Finish
  float scale;   // parameter = slider x scale (Exposure: 100 on the slider is +3 EV)
};
[[nodiscard]] std::span<const slider_info> sliders() noexcept;

struct frame {
  double cx = 0.5;  // the crop's centre, 0..1 of the picture
  double cy = 0.5;
  double zoom = 1.0;  // 1: the largest crop of the shape the picture holds
};

// In fractions of the picture.
struct rect {
  double x = 0, y = 0, w = 1, h = 1;
};

inline constexpr double kMaxZoom = 6.0;

// The crop of a w x h picture for `aspect` under `f`, moved back inside the
// picture. Empty pictures get the whole frame.
[[nodiscard]] rect crop_of(std::uint32_t w, std::uint32_t h, double aspect, const frame& f) noexcept;
// `f` after a drag of the picture by dx, dy tile-widths / heights: dragging
// right moves the picture right, so the crop moves left.
[[nodiscard]] frame dragged(const frame& f, std::uint32_t w, std::uint32_t h, double aspect, double dx,
                            double dy) noexcept;
[[nodiscard]] frame zoomed(const frame& f, double scale) noexcept;

struct chosen {
  std::uint64_t uid = 0;  // a stable id for its nodes and surfaces
  std::string path;       // a file, or "photos:<id>"
  std::string name;
  bool video = false;
  frame f;
  adjustments adjust{};
};

class post {
 public:
  [[nodiscard]] const std::vector<chosen>& items() const noexcept { return items_; }
  [[nodiscard]] shape current() const noexcept { return shape_; }
  void set_shape(shape s) noexcept { shape_ = s; }
  [[nodiscard]] std::uint64_t focus() const noexcept { return focus_; }
  void set_focus(std::uint64_t uid) noexcept;
  [[nodiscard]] chosen* find(std::uint64_t uid) noexcept;
  [[nodiscard]] std::size_t index_of(std::uint64_t uid) const noexcept;  // items().size() when absent
  [[nodiscard]] bool has(std::string_view path) const noexcept;

  // Appends in the order given; false when it was already there or the post
  // is full.
  bool add(std::string path, std::string name, bool video);
  bool remove(std::uint64_t uid);
  // The item is now this file, in the same place and with the same framing.
  bool replace(std::uint64_t uid, std::string path, std::string name);
  // One place earlier (-1) or later (+1). false at an end.
  bool move(std::uint64_t uid, int by);

  struct described {
    std::string path, name;
    bool video = false;
  };
  struct selection_change {
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t refused = 0;  // past the 20th
    std::uint64_t last_added = 0;
  };
  // The gallery answered with the whole selection: what is new is appended
  // (in the order given), what is gone is removed, and nothing else moves.
  selection_change apply_selection(std::span<const described> wanted);

  [[nodiscard]] std::vector<std::string> paths() const;

 private:
  std::vector<chosen> items_;
  shape shape_ = shape::portrait;
  std::uint64_t focus_ = 0;
  std::uint64_t next_uid_ = 1;
};

// "IMG_0412.HEIC" contains "0412"; plain ASCII case folding, as a file-name
// filter wants.
[[nodiscard]] bool name_matches(std::string_view name, std::string_view query) noexcept;
[[nodiscard]] bool looks_like_video(std::string_view name) noexcept;
[[nodiscard]] bool listable(std::string_view name) noexcept;

}  // namespace mv::instagram
