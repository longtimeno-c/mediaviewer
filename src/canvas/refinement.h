// SPDX-License-Identifier: GPL-2.0-or-later
// Preview → full without a visible pop (plan/04, "Progressive display", step 4).
//
// A still reaches the render thread more than once: a first-pixel preview (JPEG
// DCT 1/4, a RAW's embedded JPEG), then the full decode, sometimes a top-level
// texture before its mip chain. The later publishes are *refinements* of the
// item already on screen — the camera must not refit and the swap must not pop
// — while a publish for a different view intent is navigation and behaves as
// it always has (fit, or sticky zoom). This header is the pure part: which one
// a publish is, how the camera maps across a size change, and the fade curve.
// No GPU types, so the Metal host can share it (D9).
#pragma once

#include <cstdint>

namespace mv::canvas {

// Ordered: a publish never refines toward a lower rung.
enum class image_quality : std::uint8_t {
  preview = 0,   // embedded preview / DCT-scaled first pixel
  full_top = 1,  // full resolution, top level only (before the CPU mip pyramid)
  full = 2,      // full resolution with mips, or a tiled pyramid
};

// Who a published texture belongs to. `item_key` names the item (a hash of the
// path the caller decoded for), `view_generation` the view intent it was
// published under — navigating bumps it, so returning to the same file is a
// new view, not a refinement of one left behind.
struct publish_identity {
  std::uint64_t item_key = 0;
  std::uint32_t view_generation = 0;
  image_quality quality = image_quality::full;
};

enum class publish_kind : std::uint8_t {
  new_item,    // nothing on screen, or a different item / view: navigation rules
  refinement,  // same item, same view, equal or better quality: keep the view, fade
  stale,       // same item and view but worse quality (a slow preview racing the
               // full decode): drop it, never downgrade what is on screen
};

[[nodiscard]] constexpr publish_kind classify_publish(bool has_current,
                                                     const publish_identity& current,
                                                     const publish_identity& next) noexcept {
  if (!has_current) return publish_kind::new_item;
  if (current.item_key == 0 || next.item_key == 0) return publish_kind::new_item;
  if (current.item_key != next.item_key || current.view_generation != next.view_generation) {
    return publish_kind::new_item;
  }
  if (static_cast<int>(next.quality) < static_cast<int>(current.quality)) {
    return publish_kind::stale;
  }
  return publish_kind::refinement;
}

// FNV-1a. Stable, allocation-free, and never 0 for a non-empty path (0 means
// "no identity", which is always treated as a new item).
[[nodiscard]] constexpr std::uint64_t item_key_for(const char* utf8, std::uint64_t n) noexcept {
  std::uint64_t h = 1469598103934665603ull;
  for (std::uint64_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint8_t>(utf8[i]);
    h *= 1099511628211ull;
  }
  return h == 0 ? 1 : h;
}

// plan/04: "Cross-fade preview → full over 80 ms so the swap isn't a visible
// pop." Driven from real elapsed time (plan/03 rule 2), never a frame count.
inline constexpr double k_refine_fade_seconds = 0.080;
// A RAW's embedded JPEG and LibRaw's full render (camera WB, auto-bright) of
// the same frame measured 7-41 sRGB luma levels apart on real CR2/NEF/ARW/CR3/
// DNG. Geometry matches, so at 80 ms the swap reads as a brightness step, not a
// pop of detail. Past a small difference the fade stretches with it, up to a
// quarter second — still well under the decode it hides.
inline constexpr double k_refine_fade_max_seconds = 0.250;
inline constexpr int k_refine_luma_free_levels = 6;
inline constexpr int k_refine_luma_full_levels = 40;

[[nodiscard]] constexpr double refine_fade_seconds(int old_luma, int new_luma) noexcept {
  int d = new_luma - old_luma;
  if (d < 0) d = -d;
  if (d <= k_refine_luma_free_levels) return k_refine_fade_seconds;
  if (d >= k_refine_luma_full_levels) return k_refine_fade_max_seconds;
  const double t = static_cast<double>(d - k_refine_luma_free_levels) /
                   static_cast<double>(k_refine_luma_full_levels - k_refine_luma_free_levels);
  return k_refine_fade_seconds + t * (k_refine_fade_max_seconds - k_refine_fade_seconds);
}

struct crossfade {
  double start_seconds = -1.0;  // < 0: no fade
  double duration_seconds = k_refine_fade_seconds;

  void begin(double now, double duration = k_refine_fade_seconds) noexcept {
    start_seconds = now;
    duration_seconds = duration > 0.0 ? duration : k_refine_fade_seconds;
  }
  void cancel() noexcept { start_seconds = -1.0; }

  [[nodiscard]] bool active(double now) const noexcept {
    return start_seconds >= 0.0 && now - start_seconds < duration_seconds;
  }

  // Opacity of the incoming texture: 0 at the publish, 1 once the fade is done.
  // Smoothstep, so neither end of the fade has a velocity kink.
  [[nodiscard]] float alpha(double now) const noexcept {
    if (start_seconds < 0.0) return 1.0f;
    double t = (now - start_seconds) / duration_seconds;
    if (t <= 0.0) return 0.0f;
    if (t >= 1.0) return 1.0f;
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
  }
};

}  // namespace mv::canvas
