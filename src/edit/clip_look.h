// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's look (docs/plans/video-editor-look.md): one set of
// settings for the whole edit, beside its pieces. The preview shows it (the
// video blit's uv map and the player), Export exact writes it
// (op::keep_ranges with a look, in the helper), Save frame bakes it.
//
// Geometry order, the same everywhere: the coded picture is turned by the
// clip's own display rotation plus `turns`, then mirrored (as shown), then
// cropped to `crop`'s ratio about the centre. A crop of a mirrored picture is
// the mirror of the crop, so the two commute.
// Pure: no FFmpeg, no GPU, no I/O.
#pragma once

#include <cstdint>
#include <span>

#include "edit/adjust.h"

namespace mv::edit::clip {

enum class crop_ratio : std::uint8_t { original = 0, r16_9, r9_16, r1_1, r4_3, r4_5, count };

struct look {
  std::uint8_t turns = 0;  // clockwise quarter turns on top of the clip's own, 0..3
  bool flip_h = false;     // mirrored left to right, as shown
  bool flip_v = false;     // upside down, as shown
  crop_ratio crop = crop_ratio::original;
  colour light{};          // PR 11's colour set (slice 2)
  bool mute = false;       // slice 3
  float volume = 1.0f;     // 0..2
  std::int32_t fade_in_ms = 0;   // 0..kMaxFadeMs, on the program
  std::int32_t fade_out_ms = 0;
  float speed = 1.0f;  // slice 4: 0.5, 1 or 2

  friend constexpr bool operator==(const look&, const look&) = default;
  [[nodiscard]] bool geometry_identity() const noexcept {
    return turns == 0 && !flip_h && !flip_v && crop == crop_ratio::original;
  }
  [[nodiscard]] bool sound_identity() const noexcept {
    return !mute && volume == 1.0f && fade_in_ms == 0 && fade_out_ms == 0;
  }
  [[nodiscard]] bool identity() const noexcept {
    return geometry_identity() && light.identity() && sound_identity() && speed == 1.0f;
  }
  // What packet copy (Export on keyframes) can carry: a turn (the display
  // matrix) and mute (no sound stream). Anything else needs Export exact.
  [[nodiscard]] bool copyable() const noexcept {
    return !flip_h && !flip_v && crop == crop_ratio::original && light.identity() && volume == 1.0f &&
           fade_in_ms == 0 && fade_out_ms == 0 && speed == 1.0f;
  }
};

inline constexpr std::int32_t kMaxFadeMs = 5000;

// Clamped and snapped: turns mod 4, ratio in range, volume 0..2, fades
// 0..kMaxFadeMs, speed one of 0.5 / 1 / 2, colour through clamp_param.
[[nodiscard]] look sanitised(look l) noexcept;

// Short names for the crop menu and the wire ("original", "16:9", ...).
[[nodiscard]] const char* crop_name(crop_ratio r) noexcept;
[[nodiscard]] bool crop_from_name(const char* name, crop_ratio& out) noexcept;

// The picture a look makes from a coded w x h whose display rotation is
// `source_rotation` degrees clockwise (0 / 90 / 180 / 270).
struct frame_geometry {
  std::uint32_t turns = 0;     // total clockwise quarter turns, coded -> shown
  bool flip_h = false, flip_v = false;
  std::uint32_t shown_w = 0, shown_h = 0;  // turned, before the crop
  // The crop in shown pixels; for an encoder, w and h are even.
  std::uint32_t crop_x = 0, crop_y = 0, out_w = 0, out_h = 0;
};
[[nodiscard]] frame_geometry geometry_of(std::uint32_t coded_w, std::uint32_t coded_h, int source_rotation,
                                         const look& l) noexcept;

// The output pixel (x, y) of `g` reads coded pixel (cx, cy).
inline void coded_of(const frame_geometry& g, std::uint32_t coded_w, std::uint32_t coded_h, std::uint32_t x,
                     std::uint32_t y, std::uint32_t& cx, std::uint32_t& cy) noexcept {
  std::uint32_t sx = g.crop_x + x;  // shown
  std::uint32_t sy = g.crop_y + y;
  if (g.flip_h) sx = g.shown_w - 1 - sx;
  if (g.flip_v) sy = g.shown_h - 1 - sy;
  switch (g.turns & 3u) {
    case 0: cx = sx; cy = sy; break;
    case 1: cx = sy; cy = coded_h - 1 - sx; break;  // shown is coded turned 90° clockwise
    case 2: cx = coded_w - 1 - sx; cy = coded_h - 1 - sy; break;
    default: cx = coded_w - 1 - sy; cy = sx; break;
  }
}

// Bakes the geometry: `src` is coded w x h, `channels` bytes a pixel, tightly
// packed; `dst` must hold g.out_w x g.out_h x channels.
void apply_geometry(const frame_geometry& g, std::span<const std::uint8_t> src, std::uint32_t coded_w,
                    std::uint32_t coded_h, std::uint32_t channels, std::span<std::uint8_t> dst) noexcept;

// The shader's view of the same map, in normalised coordinates: the output's
// uv (0..1) -> shown uv = crop_origin + uv * crop_size, mirrored by the flips;
// then the existing quarter-turn unmap to coded uv.
struct uv_crop {
  float origin[2] = {0.0f, 0.0f};
  float size[2] = {1.0f, 1.0f};
};
[[nodiscard]] uv_crop uv_crop_of(const frame_geometry& g) noexcept;

}  // namespace mv::edit::clip
