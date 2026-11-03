// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/print_layout.h"

#include <algorithm>
#include <cmath>

namespace mv::shell {

print_grid contact_grid(std::size_t count, int preset) noexcept {
  if (preset >= 0 && preset < kContactGridCount) return kContactGrids[preset];
  for (const print_grid& g : kContactGrids) {
    if (count <= g.per_page()) return g;
  }
  return kContactGrids[kContactGridCount - 1];
}

const char* contact_grid_label(int preset) noexcept {
  switch (preset) {
    case 0: return "2 \xC3\x97 2";
    case 1: return "3 \xC3\x97 4";
    case 2: return "4 \xC3\x97 5";
    default: return "Automatic";
  }
}

std::uint32_t contact_pages(std::size_t count, print_grid grid) noexcept {
  const std::size_t per = grid.per_page();
  if (count == 0 || per == 0) return 0;
  return static_cast<std::uint32_t>((count + per - 1) / per);
}

print_rect print_area(const print_page& page) noexcept {
  if (!(page.width > 0.0) || !(page.height > 0.0)) return {};
  print_rect p = page.printable;
  if (!(p.w > 0.0) || !(p.h > 0.0)) p = {0.0, 0.0, page.width, page.height};
  const double left = std::max(p.x, kPrintMargin);
  const double top = std::max(p.y, kPrintMargin);
  const double right = std::min(p.x + p.w, page.width - kPrintMargin);
  const double bottom = std::min(p.y + p.h, page.height - kPrintMargin);
  if (right <= left || bottom <= top) return {};
  return {left, top, right - left, bottom - top};
}

print_placement place_picture(const print_rect& area, std::uint32_t w, std::uint32_t h,
                              print_fit fit) noexcept {
  print_placement out;
  out.source = {0.0, 0.0, 1.0, 1.0};
  if (w == 0 || h == 0 || !(area.w > 0.0) || !(area.h > 0.0)) return out;
  const double sx = area.w / w;
  const double sy = area.h / h;
  if (fit == print_fit::fill) {
    // Cover the area; the source narrows on the axis that overflows.
    const double s = std::max(sx, sy);
    const double fw = std::min(1.0, area.w / (w * s));
    const double fh = std::min(1.0, area.h / (h * s));
    out.dest = area;
    out.source = {(1.0 - fw) * 0.5, (1.0 - fh) * 0.5, fw, fh};
    return out;
  }
  const double s = std::min(sx, sy);
  const double dw = w * s;
  const double dh = h * s;
  out.dest = {area.x + (area.w - dw) * 0.5, area.y + (area.h - dh) * 0.5, dw, dh};
  return out;
}

print_pixels pixels_for(const print_rect& r, double dpi) noexcept {
  if (!(r.w > 0.0) || !(r.h > 0.0) || !(dpi > 0.0)) return {};
  auto px = [dpi](double pt) {
    const double v = std::ceil(pt / 72.0 * dpi - 1e-6);
    return static_cast<std::uint32_t>(std::clamp(v, 1.0, 65535.0));
  };
  return {px(r.w), px(r.h)};
}

namespace {

double cell_height(const print_rect& area, print_grid grid) noexcept {
  return (area.h - kContactGutter * (grid.rows - 1)) / grid.rows;
}

}  // namespace

print_rect contact_frame(const print_rect& area, print_grid grid, bool captions) noexcept {
  if (grid.cols == 0 || grid.rows == 0 || !(area.w > 0.0) || !(area.h > 0.0)) return {};
  const double cw = (area.w - kContactGutter * (grid.cols - 1)) / grid.cols;
  const double ch = cell_height(area, grid);
  const double ph = captions ? ch - kCaptionHeight : ch;
  if (!(cw > 0.0) || !(ph > 0.0)) return {};
  return {0.0, 0.0, cw, ph};
}

contact_cell contact_cell_at(const print_rect& area, print_grid grid, std::size_t index,
                             bool captions) noexcept {
  contact_cell out;
  const print_rect box = contact_frame(area, grid, captions);
  const std::size_t per = grid.per_page();
  if (per == 0 || !(box.w > 0.0)) return out;
  out.page = static_cast<std::uint32_t>(index / per);
  const std::size_t at = index % per;
  const double col = static_cast<double>(at % grid.cols);
  const double row = static_cast<double>(at / grid.cols);
  const double x = area.x + col * (box.w + kContactGutter);
  const double y = area.y + row * (cell_height(area, grid) + kContactGutter);
  out.frame = {x, y, box.w, box.h};
  if (captions) out.caption = {x, y + box.h, box.w, kCaptionHeight};
  return out;
}

std::string_view print_caption(std::string_view path) noexcept {
  const std::size_t sep = path.find_last_of("/\\");
  return sep == std::string_view::npos ? path : path.substr(sep + 1);
}

}  // namespace mv::shell
