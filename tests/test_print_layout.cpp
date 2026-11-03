// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #614: Ctrl+P / ⌘P's paper layout (shell/print_layout.h), both hosts.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "shell/print_layout.h"

namespace {

using namespace mv::shell;

constexpr print_page kLetter{612.0, 792.0, {}};

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

}  // namespace

TEST_CASE("print: the area keeps a quarter inch and the device's own margins", "[shell][print]") {
  const print_rect a = print_area(kLetter);
  CHECK(a == print_rect{18.0, 18.0, 576.0, 756.0});

  // A device that cannot mark the outer half inch: its margins win.
  const print_page wide{612.0, 792.0, {36.0, 36.0, 540.0, 720.0}};
  CHECK(print_area(wide) == print_rect{36.0, 36.0, 540.0, 720.0});

  // A borderless printer's whole-paper area still gets the white quarter inch.
  const print_page borderless{612.0, 792.0, {0.0, 0.0, 612.0, 792.0}};
  CHECK(print_area(borderless) == a);

  CHECK(print_area(print_page{}) == print_rect{});
  CHECK(print_area(print_page{30.0, 30.0, {}}) == print_rect{});
}

TEST_CASE("print: fit shows the whole picture, centred", "[shell][print]") {
  const print_rect area{18.0, 18.0, 576.0, 756.0};
  // A 3:2 landscape on portrait paper: the width binds.
  const print_placement p = place_picture(area, 6000, 4000, print_fit::fit);
  CHECK(p.source == print_rect{0.0, 0.0, 1.0, 1.0});
  CHECK(near(p.dest.w, 576.0));
  CHECK(near(p.dest.h, 384.0));
  CHECK(near(p.dest.x, 18.0));
  CHECK(near(p.dest.y, 18.0 + (756.0 - 384.0) / 2));

  // Portrait 2:3: the height binds.
  const print_placement q = place_picture(area, 4000, 6000, print_fit::fit);
  CHECK(near(q.dest.h, 756.0));
  CHECK(near(q.dest.w, 504.0));
  CHECK(near(q.dest.x, 18.0 + (576.0 - 504.0) / 2));
}

TEST_CASE("print: fill covers the area and crops the overflow evenly", "[shell][print]") {
  const print_rect area{18.0, 18.0, 576.0, 756.0};
  const print_placement p = place_picture(area, 6000, 4000, print_fit::fill);
  CHECK(p.dest == area);
  // Scale 756/4000: the width drawn is 576 / (6000 * 0.189) of the picture.
  const double fw = 576.0 / (6000.0 * (756.0 / 4000.0));
  CHECK(near(p.source.w, fw));
  CHECK(near(p.source.h, 1.0));
  CHECK(near(p.source.x, (1.0 - fw) / 2));
  CHECK(near(p.source.y, 0.0));

  // Same aspect as the area: nothing cropped either way.
  const print_placement same = place_picture(area, 576, 756, print_fit::fill);
  CHECK(near(same.source.w, 1.0));
  CHECK(near(same.source.h, 1.0));

  // Nothing to place: an empty destination, the whole source.
  CHECK(place_picture(area, 0, 10, print_fit::fill).dest == print_rect{});
}

TEST_CASE("print: pixels at 300 dpi", "[shell][print]") {
  CHECK(pixels_for({0, 0, 72.0, 144.0}) == print_pixels{300, 600});
  CHECK(pixels_for({0, 0, 576.0, 756.0}) == print_pixels{2400, 3150});
  CHECK(pixels_for({0, 0, 1.0, 1.0}, 72.0) == print_pixels{1, 1});
  CHECK(pixels_for({0, 0, 0.0, 10.0}) == print_pixels{});
}

TEST_CASE("print: the sheet's grid follows the count, or the dialog's choice", "[shell][print]") {
  CHECK(contact_grid(2) == print_grid{2, 2});
  CHECK(contact_grid(4) == print_grid{2, 2});
  CHECK(contact_grid(5) == print_grid{3, 4});
  // The verify line: 12 marks print as a 3x4 sheet, one page.
  CHECK(contact_grid(12) == print_grid{3, 4});
  CHECK(contact_pages(12, contact_grid(12)) == 1);
  CHECK(contact_grid(13) == print_grid{4, 5});
  CHECK(contact_grid(20) == print_grid{4, 5});
  CHECK(contact_grid(41) == print_grid{4, 5});
  CHECK(contact_pages(41, contact_grid(41)) == 3);
  CHECK(contact_pages(0, print_grid{2, 2}) == 0);

  CHECK(contact_grid(12, 0) == print_grid{2, 2});
  CHECK(contact_pages(12, contact_grid(12, 0)) == 3);
  CHECK(contact_grid(3, 2) == print_grid{4, 5});
  CHECK(contact_grid(3, 7) == print_grid{2, 2});  // out of range: automatic

  CHECK(std::string_view(contact_grid_label(-1)) == "Automatic");
  CHECK(std::string_view(contact_grid_label(1)) == "3 \xC3\x97 4");
}

TEST_CASE("print: sheet cells tile the area in reading order, page by page", "[shell][print]") {
  const print_rect area{18.0, 18.0, 576.0, 756.0};
  const print_grid g{3, 4};
  const double cw = (576.0 - 2 * kContactGutter) / 3;
  const double ch = (756.0 - 3 * kContactGutter) / 4;

  const contact_cell first = contact_cell_at(area, g, 0, false);
  CHECK(first.page == 0);
  CHECK(near(first.frame.x, 18.0));
  CHECK(near(first.frame.y, 18.0));
  CHECK(near(first.frame.w, cw));
  CHECK(near(first.frame.h, ch));
  CHECK(first.caption == print_rect{});

  // Index 4: second row, second column.
  const contact_cell c4 = contact_cell_at(area, g, 4, false);
  CHECK(near(c4.frame.x, 18.0 + cw + kContactGutter));
  CHECK(near(c4.frame.y, 18.0 + ch + kContactGutter));

  // The last cell ends on the area's bottom-right corner.
  const contact_cell last = contact_cell_at(area, g, 11, false);
  CHECK(near(last.frame.x + last.frame.w, 18.0 + 576.0));
  CHECK(near(last.frame.y + last.frame.h, 18.0 + 756.0));

  // Index 12 starts page 2 at the top-left again.
  const contact_cell next = contact_cell_at(area, g, 12, false);
  CHECK(next.page == 1);
  CHECK(next.frame == first.frame);

  // With captions the picture gives up one line; the line sits under it.
  const contact_cell cap = contact_cell_at(area, g, 4, true);
  CHECK(near(cap.frame.h, ch - kCaptionHeight));
  CHECK(near(cap.caption.y, cap.frame.y + cap.frame.h));
  CHECK(near(cap.caption.h, kCaptionHeight));
  CHECK(near(cap.caption.w, cw));
  CHECK(contact_frame(area, g, true).h == cap.frame.h);
}

TEST_CASE("print: a caption is the file name only", "[shell][print]") {
  CHECK(print_caption("/Volumes/Card/DCIM/IMG_0001.JPG") == "IMG_0001.JPG");
  CHECK(print_caption("C:\\Users\\x\\Pictures\\a.heic") == "a.heic");
  CHECK(print_caption("plain.png") == "plain.png");
}

TEST_CASE("print: two or more marks print as a sheet", "[shell][print]") {
  CHECK_FALSE(print_as_sheet(0));
  CHECK_FALSE(print_as_sheet(1));
  CHECK(print_as_sheet(2));
  CHECK(print_as_sheet(12));
}
