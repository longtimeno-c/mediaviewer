// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/brush_mask.h"

#include <algorithm>
#include <cmath>

namespace mv::shell {

std::pair<std::uint32_t, std::uint32_t> brush_mask_size(std::uint32_t frame_w, std::uint32_t frame_h) noexcept {
  if (frame_w == 0 || frame_h == 0) return {0, 0};
  const std::uint32_t l = std::max(frame_w, frame_h);
  const std::uint32_t side = std::min(l, kBrushMaskLongSide);
  const double s = static_cast<double>(side) / static_cast<double>(l);
  const auto w = static_cast<std::uint32_t>(std::max(1.0, std::round(frame_w * s)));
  const auto h = static_cast<std::uint32_t>(std::max(1.0, std::round(frame_h * s)));
  return {w, h};
}

void stamp_brush_dabs(std::span<const brush_dab> dabs, std::uint32_t w, std::uint32_t h,
                      std::vector<std::uint8_t>& cover) noexcept {
  if (w == 0 || h == 0 || cover.size() < std::size_t{w} * h) return;
  const float l = static_cast<float>(std::max(w, h));
  const auto lo = [](float v) { return static_cast<std::int64_t>(std::floor(v)); };
  for (const brush_dab& d : dabs) {
    const std::uint8_t value = d.erase ? 0 : 255;
    const float cx = d.u * static_cast<float>(w), cy = d.v * static_cast<float>(h);
    const float r = d.r * l;
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(r)) continue;
    // Under a pixel: the pixel the centre is in (a tiny brush still marks).
    if (r < 0.75f) {
      const std::int64_t x = lo(cx), y = lo(cy);
      if (x >= 0 && y >= 0 && x < static_cast<std::int64_t>(w) && y < static_cast<std::int64_t>(h)) {
        cover[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = value;
      }
      continue;
    }
    const std::int64_t x0 = std::max<std::int64_t>(0, lo(cx - r));
    const std::int64_t x1 = std::min<std::int64_t>(static_cast<std::int64_t>(w) - 1, lo(cx + r));
    const std::int64_t y0 = std::max<std::int64_t>(0, lo(cy - r));
    const std::int64_t y1 = std::min<std::int64_t>(static_cast<std::int64_t>(h) - 1, lo(cy + r));
    const float rr = r * r;
    for (std::int64_t y = y0; y <= y1; ++y) {
      const float py = static_cast<float>(y) + 0.5f - cy;
      const float left = rr - py * py;
      if (left < 0.0f) continue;
      // The row's span inside the disc, not a test per pixel.
      const float half = std::sqrt(left);
      const std::int64_t a = std::max<std::int64_t>(x0, static_cast<std::int64_t>(std::ceil(cx - half - 0.5f)));
      const std::int64_t b = std::min<std::int64_t>(x1, static_cast<std::int64_t>(std::floor(cx + half - 0.5f)));
      if (a > b) continue;
      std::uint8_t* row = cover.data() + static_cast<std::size_t>(y) * w;
      std::fill(row + a, row + b + 1, value);
    }
  }
}

void tint_brush_mask(std::span<const std::uint8_t> cover, std::uint32_t w, std::uint32_t h, std::uint32_t accent,
                     std::vector<std::uint8_t>& rgba) {
  const std::size_t n = std::size_t{w} * h;
  rgba.assign(n * 4, 0);
  if (n == 0 || cover.size() < n) return;
  const auto r = static_cast<std::uint8_t>((accent >> 24) & 0xFFu);
  const auto g = static_cast<std::uint8_t>((accent >> 16) & 0xFFu);
  const auto b = static_cast<std::uint8_t>((accent >> 8) & 0xFFu);
  const auto dark = [](std::uint8_t c) { return static_cast<std::uint8_t>((c * 3 + 2) / 5); };
  const std::uint8_t er = dark(r), eg = dark(g), eb = dark(b);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* row = cover.data() + std::size_t{y} * w;
    const std::uint8_t* up = y > 0 ? row - w : nullptr;
    const std::uint8_t* down = y + 1 < h ? row + w : nullptr;
    std::uint8_t* out = rgba.data() + std::size_t{y} * w * 4;
    for (std::uint32_t x = 0; x < w; ++x, out += 4) {
      if (!row[x]) {
        out[0] = er;  // transparent, the edge's colour: no dark fringe when filtered
        out[1] = eg;
        out[2] = eb;
        continue;
      }
      const bool edge = x == 0 || x + 1 == w || !up || !down || !row[x - 1] || !row[x + 1] || !up[x] || !down[x];
      if (edge) {
        out[0] = er;
        out[1] = eg;
        out[2] = eb;
        out[3] = kBrushMaskEdgeAlpha;
      } else {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        out[3] = kBrushMaskFillAlpha;
      }
    }
  }
}

bool brush_mask_builder::build(const brush_mask_job& job, brush_mask_pixels& out) {
  last_stamped_ = 0;
  const auto [w, h] = brush_mask_size(job.tag.frame_w, job.tag.frame_h);
  const std::size_t count = std::min<std::size_t>(job.tag.count, job.dabs.size());
  if (w == 0 || h == 0 || count == 0) {
    valid_ = false;
    return false;
  }
  // The last job's coverage still holds when this one only adds dabs to it.
  const bool extends = valid_ && w == w_ && h == h_ && held_.item == job.tag.item &&
                       held_.epoch == job.tag.epoch && held_.gen == job.tag.gen && held_.count <= count;
  std::size_t from = 0;
  if (extends) {
    from = held_.count;
  } else {
    cover_.assign(std::size_t{w} * h, 0);
    w_ = w;
    h_ = h;
  }
  const std::span<const brush_dab> dabs(job.dabs.data(), count);
  stamp_brush_dabs(dabs.subspan(from), w, h, cover_);
  last_stamped_ = count - from;
  held_ = job.tag;
  held_.count = static_cast<std::uint32_t>(count);
  valid_ = true;
  out.width = w;
  out.height = h;
  tint_brush_mask(cover_, w, h, job.tag.accent, out.rgba);
  return true;
}

std::optional<brush_mask_job> brush_mask_sync::next(const remove_brush& b, std::uint64_t item, std::uint32_t frame_w,
                                                    std::uint32_t frame_h, std::uint32_t accent) {
  if (in_flight_ || b.empty() || item == 0 || frame_w == 0 || frame_h == 0) return std::nullopt;
  brush_mask_tag want;
  want.item = item;
  want.epoch = epoch_;
  want.gen = b.generation();
  want.count = static_cast<std::uint32_t>(b.dabs().size());
  want.frame_w = frame_w;
  want.frame_h = frame_h;
  want.accent = accent;
  if (landed_ && *landed_ == want) return std::nullopt;
  if (failed_ && *failed_ == want) return std::nullopt;  // never retried in a loop
  if (b.stroking()) {
    // Mid-stroke: only once the live tail has run far enough.
    const std::size_t from = tail_from(b);
    const std::size_t flush = b.erasing() ? kBrushMaskFlushEraseDabs : kBrushMaskFlushDabs;
    if (b.dabs().size() - std::min(from, b.dabs().size()) < flush) return std::nullopt;
  }
  brush_mask_job job;
  job.tag = want;
  job.dabs.assign(b.dabs().begin(), b.dabs().end());
  in_flight_ = want;
  return job;
}

void brush_mask_sync::landed(const brush_mask_tag& tag, bool ok) noexcept {
  if (in_flight_ && *in_flight_ == tag) in_flight_.reset();
  if (tag.epoch != epoch_) return;  // built for a brushing since forgotten
  if (ok) {
    landed_ = tag;
    failed_.reset();
  } else {
    failed_ = tag;
  }
}

void brush_mask_sync::reset() noexcept {
  ++epoch_;
  if (epoch_ == 0) epoch_ = 1;
  landed_.reset();
  failed_.reset();
  // A build still in flight lands for the old epoch and is ignored; until
  // then nothing new starts (one job at a time), which a walk does not need.
}

std::size_t brush_mask_sync::tail_from(const remove_brush& b) const noexcept {
  // Only a texture that landed: a build still in flight is not on screen yet,
  // so its dabs stay live until it is (else a finished stroke would vanish
  // for the frames the build takes).
  return holds_prefix(landed_, b) ? landed_->count : 0;
}

}  // namespace mv::shell
