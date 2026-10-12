// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// New from Clipboard (docs/design/16): a packed DIB as a BMP file, the unsaved item
// held in memory and read like a file, never written in place, and Save Copy
// into a chosen folder as "Untitled.jpg".
#include "catch_compat.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "codec/dib.h"
#include "codec/format.h"
#include "fixtures.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/memory_file.h"
#include "io/replace.h"
#include "shell/clipboard_image.h"
#include "shell/edit_session.h"

namespace {

namespace fs = std::filesystem;

struct temp_dir {
  fs::path path;
  temp_dir() {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("mv_clipboard_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "_" + std::to_string(++counter));
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}

// A 2x1 top-down 32-bit BI_BITFIELDS DIB, red then blue, as CF_DIB (a
// BITMAPINFOHEADER and its three masks) or CF_DIBV5 (masks in the header;
// `doubled_masks` appends a BITMAPINFOHEADER's three again, as some writers do).
std::vector<std::uint8_t> bitfields_dib(bool v5, bool doubled_masks) {
  const std::uint32_t header = v5 ? 124 : 40;
  const std::size_t masks_after = !v5 || doubled_masks ? 12 : 0;
  std::vector<std::uint8_t> d(header + masks_after + 8, 0);
  put32(d, 0, header);
  put32(d, 4, 2);
  put32(d, 8, 0xFFFFFFFFu);  // height -1: top-down
  d[12] = 1;                 // planes
  d[14] = 32;                // bpp
  put32(d, 16, 3);           // BI_BITFIELDS
  put32(d, 20, 8);           // image bytes
  if (v5) {
    put32(d, 40, 0x00FF0000u);
    put32(d, 44, 0x0000FF00u);
    put32(d, 48, 0x000000FFu);
    put32(d, 52, 0xFF000000u);
    put32(d, 56, 0x73524742u);  // 'sRGB'
  }
  if (masks_after != 0) {
    put32(d, header, 0x00FF0000u);
    put32(d, header + 4, 0x0000FF00u);
    put32(d, header + 8, 0x000000FFu);
  }
  const std::size_t px = header + masks_after;
  put32(d, px, 0xFFFF0000u);      // red
  put32(d, px + 4, 0xFF0000FFu);  // blue
  return d;
}

void check_red_then_blue(const std::vector<std::uint8_t>& bmp) {
  REQUIRE(mv::codec::probe(bmp) == mv::codec::format_family::bmp);
  auto r = mv::codec::decode(bmp);
  REQUIRE(r);
  REQUIRE(r->width == 2);
  REQUIRE(r->height == 1);
  CHECK(r->rgba[0] == 255);
  CHECK(r->rgba[1] == 0);
  CHECK(r->rgba[2] == 0);
  CHECK(r->rgba[4] == 0);
  CHECK(r->rgba[5] == 0);
  CHECK(r->rgba[6] == 255);
}

std::vector<std::uint8_t> png_8x6() {
  std::vector<std::uint8_t> rgba(8 * 6 * 4, 200);
  return fixtures::png_rgba(8, 6, rgba.data());
}

}  // namespace

TEST_CASE("a packed DIB from the clipboard becomes a BMP file", "[clipboard]") {
  std::vector<std::uint8_t> rgba(3 * 2 * 4);
  for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<std::uint8_t>(i * 9);
  const std::vector<std::uint8_t> file = fixtures::bmp_rgba(3, 2, rgba.data());
  // CF_DIB is the file without its 14-byte header.
  const std::vector<std::uint8_t> packed(file.begin() + 14, file.end());
  auto wrapped = mv::codec::dib::bmp_from_packed(packed);
  REQUIRE(wrapped);
  CHECK(*wrapped == file);

  auto plain = mv::codec::dib::bmp_from_packed(bitfields_dib(false, false));
  REQUIRE(plain);
  check_red_then_blue(*plain);
  auto v5 = mv::codec::dib::bmp_from_packed(bitfields_dib(true, false));
  REQUIRE(v5);
  check_red_then_blue(*v5);
  auto doubled = mv::codec::dib::bmp_from_packed(bitfields_dib(true, true));
  REQUIRE(doubled);
  check_red_then_blue(*doubled);

  CHECK_FALSE(mv::codec::dib::bmp_from_packed({}));
  const std::vector<std::uint8_t> header_only(packed.begin(), packed.begin() + 40);
  CHECK_FALSE(mv::codec::dib::bmp_from_packed(header_only));
}

TEST_CASE("the unsaved item reads like a file and is never written", "[clipboard]") {
  const std::vector<std::uint8_t> png = png_8x6();
  auto key = mv::shell::adopt_clipboard_image(png, mv::shell::clipboard_flavor::file);
  REQUIRE(key);
  CHECK(mv::io::is_memory_path(*key));
  CHECK(mv::io::file_name_of(*key) == "Untitled");
  CHECK_FALSE(mv::io::is_memory_path("/Users/a/clipboard:1/Untitled"));

  auto all = mv::io::read_all(*key);
  REQUIRE(all);
  CHECK(*all == png);
  auto head = mv::io::read_prefix(*key, 8);
  REQUIRE(head);
  CHECK(head->size() == 8);
  auto st = mv::io::stat_path(*key);
  REQUIRE(st);
  CHECK(st->size == png.size());
  CHECK_FALSE(st->is_directory);
  CHECK(mv::io::file_exists(*key));

  // Nothing writes it in place, or beside it.
  CHECK(mv::io::write_all(*key, png).error() == mv::status::permission_denied);
  CHECK(mv::io::write_new(*key, png).error() == mv::status::permission_denied);
  CHECK(mv::io::write_new_atomic(*key + ".xmp", png).error() == mv::status::permission_denied);
  CHECK(mv::io::replace_atomic(*key, png).error() == mv::status::permission_denied);
  CHECK(mv::shell::run_export(*key, {}, {}).error() == mv::status::permission_denied);

  // Not an image: refused, and the item before it stays.
  const std::vector<std::uint8_t> text{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!'};
  CHECK(mv::shell::adopt_clipboard_image(text, mv::shell::clipboard_flavor::file).error() ==
        mv::status::unsupported_format);
  CHECK(mv::shell::adopt_clipboard_image(text, mv::shell::clipboard_flavor::packed_dib).error() ==
        mv::status::unsupported_format);
  CHECK(mv::io::file_exists(*key));

  // A second paste is a new key and releases the first.
  auto dib = mv::shell::adopt_clipboard_image(bitfields_dib(true, false), mv::shell::clipboard_flavor::packed_dib);
  REQUIRE(dib);
  CHECK(*dib != *key);
  CHECK_FALSE(mv::io::file_exists(*key));
  CHECK(mv::io::read_all(*key).error() == mv::status::not_found);
  auto bmp = mv::io::read_all(*dib);
  REQUIRE(bmp);
  check_red_then_blue(*bmp);
  mv::io::clear_memory_files();
  CHECK_FALSE(mv::io::file_exists(*dib));
}

TEST_CASE("Save Copy writes the unsaved item into the chosen folder as Untitled", "[clipboard]") {
  temp_dir dir;
  auto key = mv::shell::adopt_clipboard_image(png_8x6(), mv::shell::clipboard_flavor::file);
  REQUIRE(key);
  mv::edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  auto a = mv::shell::run_export_to(*key, dir.path.string(), g, {});
  REQUIRE(a);
  CHECK(fs::path(*a).parent_path() == dir.path);
  CHECK(fs::path(*a).filename() == "Untitled.jpg");
  auto b = mv::shell::run_export_to(*key, dir.path.string(), g, {});
  REQUIRE(b);
  CHECK(fs::path(*b).filename() == "Untitled (2).jpg");
  mv::edit::export_options png;
  png.encode.format = mv::edit::image_format::png;
  auto c = mv::shell::run_export_to(*key, dir.path.string(), g, png);
  REQUIRE(c);
  CHECK(fs::path(*c).filename() == "Untitled.png");

  auto written = mv::io::read_all(*a);
  REQUIRE(written);
  auto decoded = mv::codec::decode(*written);
  REQUIRE(decoded);
  CHECK(decoded->width == 6);  // turned: the stack is baked
  CHECK(decoded->height == 8);
  mv::io::clear_memory_files();
}

TEST_CASE("an unsaved JPEG turns on its stack, never in place", "[clipboard][edit]") {
  mv::shell::edit_session s;
  (void)s.set_item(mv::shell::edit_item{"clipboard:7/Untitled", 1000, 1, 600, 400, true});
  CHECK(s.run(mv::shell::command_id::rotate_cw) == mv::shell::edit_effect::redraw);
  CHECK_FALSE(s.take_pending_write());
  CHECK(s.export_geometry().orient == mv::codec::kRotateCw);
}

TEST_CASE("the clipboard list reads as Clipboard, not a search", "[clipboard]") {
  CHECK(mv::shell::is_clipboard_list(mv::shell::kClipboardListTitle));
  CHECK_FALSE(mv::shell::is_clipboard_list("sunset"));
  CHECK(mv::shell::unsaved_copy_name("clipboard:3/Untitled", true) == "Untitled.png");
  CHECK(mv::shell::unsaved_copy_name("clipboard:3/Untitled", false) == "Untitled.jpg");
}
