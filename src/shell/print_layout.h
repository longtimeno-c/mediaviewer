// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #614 (docs/design/16 "Print"): Ctrl+P / ⌘P. Where the pictures go on the
// paper, shared by both hosts: the photo on screen at fit or fill, or the marks
// as an N-up contact sheet. Pure: no I/O, no OS call; the host asks the OS for
// the paper (PrintDlgEx's DC on Windows, NSPrintInfo on the Mac), lays it out
// here, bakes the pixels on the pool (shell/edit_session.h render_print_*) and
// draws them with the OS's own print path.
//
// Units are points (1/72 inch), origin at the paper's top-left corner.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mv::shell {

struct print_rect {
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
  friend constexpr bool operator==(const print_rect&, const print_rect&) = default;
};

struct print_page {
  double width = 0.0;   // the paper, points
  double height = 0.0;
  print_rect printable; // what the device can mark; empty = the whole paper
};

enum class print_fit : std::uint8_t {
  fit = 0,   // the whole picture, letterboxed in white (the default)
  fill = 1,  // the area covered, the picture's overflow cropped
};

struct print_grid {
  std::uint32_t cols = 0, rows = 0;
  [[nodiscard]] constexpr std::uint32_t per_page() const noexcept { return cols * rows; }
  friend constexpr bool operator==(const print_grid&, const print_grid&) = default;
};

// The sheet layouts the print dialogs offer, in order. Index -1 is automatic.
inline constexpr print_grid kContactGrids[] = {{2, 2}, {3, 4}, {4, 5}};
inline constexpr int kContactGridCount = static_cast<int>(sizeof(kContactGrids) / sizeof(kContactGrids[0]));

// What the print dialog's options carry (the Windows options page, the Mac
// panel's accessory). Kept for the session by each host, never persisted.
struct print_options {
  print_fit fit = print_fit::fit;
  bool captions = false;  // file names under the sheet's pictures: off by default
  int grid = -1;          // index into kContactGrids; -1 automatic
};

inline constexpr double kPrintMargin = 18.0;     // a quarter inch of white at least
inline constexpr double kPrintDpi = 300.0;       // what the bake resolves for
inline constexpr double kContactGutter = 9.0;    // between cells
inline constexpr double kCaptionHeight = 12.0;   // one line of kCaptionPoints type
inline constexpr double kCaptionPoints = 8.0;

// Ctrl+P prints the marks as a sheet when two or more are marked (a gallery
// range counts as marks); otherwise the photo on screen.
[[nodiscard]] constexpr bool print_as_sheet(std::size_t marked) noexcept { return marked >= 2; }

// The grid for `count` pictures: `preset` when it names one, else the
// smallest that holds them all on one page (12 → 3×4), and 4×5 over as many
// pages as it takes past 20.
[[nodiscard]] print_grid contact_grid(std::size_t count, int preset = -1) noexcept;
// "Automatic", "2 × 2", "3 × 4", "4 × 5" (UTF-8), for the dialogs.
[[nodiscard]] const char* contact_grid_label(int preset) noexcept;
[[nodiscard]] std::uint32_t contact_pages(std::size_t count, print_grid grid) noexcept;

// Where pictures may go: the printable area, and at least kPrintMargin in
// from the paper's edge.
[[nodiscard]] print_rect print_area(const print_page& page) noexcept;

struct print_placement {
  print_rect dest;    // where the drawn part of the picture lands, points
  print_rect source;  // which part of the picture is drawn, 0..1 (fill crops)
};
// A `w` x `h` picture in `area`, centred. Fit draws all of it as large as fits;
// fill covers the area and crops the overflow evenly.
[[nodiscard]] print_placement place_picture(const print_rect& area, std::uint32_t w, std::uint32_t h,
                                            print_fit fit) noexcept;

struct print_pixels {
  std::uint32_t w = 0, h = 0;
  friend constexpr bool operator==(const print_pixels&, const print_pixels&) = default;
};
// The pixels `r` spans at `dpi`: what a bake for it needs, and no more.
[[nodiscard]] print_pixels pixels_for(const print_rect& r, double dpi = kPrintDpi) noexcept;

struct contact_cell {
  std::uint32_t page = 0;  // 0-based
  print_rect frame;        // the picture fits (place_picture, fit) in here
  print_rect caption;      // the file name's line; empty without captions
};
// Cell `index` (0-based, in mark order: listing order) of a sheet in `area`.
[[nodiscard]] contact_cell contact_cell_at(const print_rect& area, print_grid grid, std::size_t index,
                                           bool captions) noexcept;
// The picture box of every cell of `grid` in `area` (they are all one size).
[[nodiscard]] print_rect contact_frame(const print_rect& area, print_grid grid, bool captions) noexcept;

// A caption: the file name, without the folder (nothing else of the path is
// printed). Splits on / and \.
[[nodiscard]] std::string_view print_caption(std::string_view path) noexcept;

}  // namespace mv::shell
