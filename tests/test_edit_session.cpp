// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10: the host edit state both hosts drive — `[` `]` from the viewer as a
// debounced lossless write, crop mode's draft, undo / reset, export naming.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "edit/encode.h"
#include "edit/lossless_jpeg.h"
#include "io/replace.h"
#include "shell/edit_session.h"
#include "shell/edit_view.h"

namespace {

namespace fs = std::filesystem;
using mv::shell::command_id;
using mv::shell::edit_effect;
using mv::shell::edit_item;
using mv::shell::edit_session;

edit_item jpeg_item(std::uint64_t size = 1000, std::int64_t mtime = 1) {
  return edit_item{"/photos/IMG_0001.JPG", size, mtime, 600, 400, true};
}

std::vector<std::uint8_t> jpeg_bytes(std::uint32_t w, std::uint32_t h) {
  mv::codec::raster r;
  r.width = w;
  r.height = h;
  r.rgba.assign(static_cast<std::size_t>(w) * h * 4, 180);
  auto bytes = mv::edit::encode(r, {}, {});
  REQUIRE(bytes);
  return std::move(bytes).value();
}

struct temp_dir {
  fs::path path;
  temp_dir() {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("mv_edit_session_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(++counter));
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("`]` on a JPEG turns the preview and asks for one lossless write", "[shell][edit]") {
  edit_session s;
  REQUIRE_FALSE(s.set_item(jpeg_item()));
  REQUIRE(s.run(command_id::rotate_cw) == edit_effect::write_rotation);
  CHECK(s.preview_placement().cropped == mv::edit::size2{400, 600});

  auto w = s.take_pending_write();
  REQUIRE(w);
  CHECK(w->op == mv::codec::kRotateCw);
  CHECK(w->size == 1000);
  CHECK_FALSE(s.take_pending_write());  // one at a time

  // Pressed while the write is in flight: previewed now, written after.
  REQUIRE(s.run(command_id::rotate_cw) == edit_effect::write_rotation);
  CHECK(s.preview_placement().cropped == mv::edit::size2{600, 400});
  s.write_finished(true);
  // The rewritten file lands (new size / mtime, now 400 x 600 displayed):
  // the second turn is carried onto it and wants its own write.
  edit_item after = jpeg_item(1010, 2);
  after.width = 400;
  after.height = 600;
  REQUIRE(s.set_item(after));
  CHECK(s.preview_placement().cropped == mv::edit::size2{600, 400});
  auto w2 = s.take_pending_write();
  REQUIRE(w2);
  CHECK(w2->op == mv::codec::kRotateCw);
  CHECK(w2->size == 1010);
}

TEST_CASE("a rewrite that keeps size and mtime still lands once", "[shell][edit]") {
  // A 180° rewrite of an already-optimised file can come back the same size
  // within the same second: the key must still move, or the turn applies twice.
  edit_session s;
  (void)s.set_item(jpeg_item());
  (void)s.run(command_id::rotate_cw);
  (void)s.run(command_id::rotate_cw);
  REQUIRE(s.take_pending_write());
  s.write_finished(true);
  CHECK_FALSE(s.set_item(jpeg_item()));  // same size, same mtime
  CHECK(s.export_geometry().identity());
  CHECK(s.preview_placement().map.identity());
}

TEST_CASE("a relist that reopens the file mid-write counts as the landing", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  (void)s.run(command_id::rotate_cw);
  REQUIRE(s.take_pending_write());
  // The watcher saw the swap first and reopened the file.
  CHECK_FALSE(s.set_item(jpeg_item(1010, 2)));
  CHECK(s.export_geometry().identity());
  // A turn now belongs to the rewritten file, and waits for the job slot.
  REQUIRE(s.run(command_id::rotate_ccw) == edit_effect::write_rotation);
  CHECK_FALSE(s.take_pending_write());
  s.write_finished(true);  // already carried: nothing is applied twice
  auto w = s.take_pending_write();
  REQUIRE(w);
  CHECK(w->op == mv::codec::kRotateCcw);
  CHECK(w->size == 1010);
}

TEST_CASE("turns that cancel before the debounce write nothing", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  REQUIRE(s.run(command_id::rotate_cw) == edit_effect::write_rotation);
  REQUIRE(s.run(command_id::rotate_ccw) == edit_effect::redraw);
  CHECK_FALSE(s.take_pending_write());
  REQUIRE(s.run(command_id::flip_horizontal) == edit_effect::write_rotation);
  REQUIRE(s.run(command_id::flip_horizontal) == edit_effect::redraw);
  CHECK_FALSE(s.take_pending_write());
}

TEST_CASE("a non-JPEG turns in the preview only", "[shell][edit]") {
  edit_session s;
  edit_item png = jpeg_item();
  png.jpeg = false;
  (void)s.set_item(png);
  REQUIRE(s.run(command_id::rotate_cw) == edit_effect::redraw);
  CHECK_FALSE(s.take_pending_write());
  CHECK(s.export_geometry().orient == mv::codec::kRotateCw);
}

TEST_CASE("a failed write stops asking and keeps the turn for export", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  (void)s.run(command_id::rotate_cw);
  REQUIRE(s.take_pending_write());
  s.write_finished(false);
  CHECK_FALSE(s.take_pending_write());
  CHECK(s.run(command_id::rotate_cw) == edit_effect::redraw);
  CHECK(s.export_geometry().orient == mv::codec::from_exif(3));
}

TEST_CASE("crop mode drafts, commits and cancels", "[shell][edit]") {
  edit_session s;
  edit_item png = jpeg_item();
  png.jpeg = false;
  (void)s.set_item(png);
  REQUIRE(s.run(command_id::crop_mode) == edit_effect::redraw);
  REQUIRE(s.crop_active());
  // A full-frame rect cannot move until it is smaller.
  CHECK(s.run(command_id::crop_move_right) == edit_effect::none);
  for (int i = 0; i < 20; ++i) REQUIRE(s.run(command_id::crop_narrower) == edit_effect::redraw);
  for (int i = 0; i < 10; ++i) REQUIRE(s.run(command_id::crop_move_right) == edit_effect::redraw);
  CHECK(std::abs(s.crop_overlay().w - 0.8f) < 1e-4f);
  CHECK(std::abs(s.crop_overlay().x - 0.1f) < 1e-4f);
  // The preview shows the whole frame while cropping.
  CHECK(s.preview_placement().cropped == mv::edit::size2{600, 400});

  s.cancel_crop();
  CHECK_FALSE(s.crop_active());
  CHECK(s.export_geometry().identity());

  REQUIRE(s.run(command_id::crop_mode) == edit_effect::redraw);
  for (int i = 0; i < 50; ++i) (void)s.run(command_id::crop_shorter);
  REQUIRE(s.run(command_id::crop_commit) == edit_effect::redraw);
  CHECK_FALSE(s.crop_active());
  CHECK(s.preview_placement().cropped == mv::edit::size2{600, 200});
  // Undo pops the crop; reset returns the original placement exactly.
  REQUIRE(s.run(command_id::undo_edit) == edit_effect::redraw);
  CHECK(s.export_geometry().identity());
  (void)s.run(command_id::rotate_cw);
  REQUIRE(s.run(command_id::reset_edits) == edit_effect::redraw);
  CHECK(s.preview_placement().map.identity());
  CHECK(s.run(command_id::undo_edit) == edit_effect::refused);
}

TEST_CASE("straighten keeps the largest fit until the rect is placed", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  REQUIRE(s.run(command_id::crop_mode) == edit_effect::redraw);
  for (int i = 0; i < 4; ++i) REQUIRE(s.run(command_id::straighten_cw) == edit_effect::redraw);
  CHECK(s.crop_angle() == 2.0f);
  const auto fit = mv::edit::auto_crop(2.0f, {600, 400});
  CHECK(std::abs(s.crop_overlay().w - fit.w) < 1e-5f);
  // No lossless write while cropping, and none after: a straighten is pixels.
  REQUIRE(s.run(command_id::rotate_cw) == edit_effect::redraw);
  REQUIRE(s.run(command_id::crop_commit) == edit_effect::redraw);
  CHECK_FALSE(s.take_pending_write());
  const auto g = s.export_geometry();
  CHECK(g.straighten == 2.0f);
  CHECK_FALSE(s.preview_placement().exact_copy);
}

TEST_CASE("crop and export wait for a write in flight", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  (void)s.run(command_id::rotate_cw);
  REQUIRE(s.take_pending_write());
  CHECK(s.run(command_id::crop_mode) == edit_effect::refused);
  CHECK(s.run(command_id::export_image) == edit_effect::refused);
  s.write_finished(true);
  // Written but not yet reloaded: the stack still holds the turn the file has.
  CHECK(s.run(command_id::export_image) == edit_effect::refused);
  edit_item after = jpeg_item(1010, 2);
  (void)s.set_item(after);
  CHECK(s.run(command_id::export_image) == edit_effect::export_image);
  CHECK(s.export_geometry().identity());
}

TEST_CASE("a relist that reopens the same bytes keeps crop mode", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  REQUIRE(s.run(command_id::crop_mode) == edit_effect::redraw);
  edit_item again = jpeg_item();
  again.width = 0;  // the host has not seen the pixels of the reopen yet
  again.height = 0;
  (void)s.set_item(again);
  CHECK(s.crop_active());
  CHECK(s.preview_placement().cropped == mv::edit::size2{600, 400});
  (void)s.set_item(jpeg_item(2000, 5));  // the file changed: a new item
  CHECK_FALSE(s.crop_active());
}

TEST_CASE("stacks are kept per file for the session", "[shell][edit]") {
  edit_session s;
  (void)s.set_item(jpeg_item());
  (void)s.run(command_id::flip_vertical);
  edit_item other = jpeg_item();
  other.path = "/photos/IMG_0002.JPG";
  (void)s.set_item(other);
  CHECK(s.export_geometry().identity());
  CHECK(s.set_item(jpeg_item()));  // back: the flip is still there, still unwritten
  CHECK(s.export_geometry().orient == mv::codec::kFlipV);
}

TEST_CASE("the I/O jobs rotate in place and export beside the original", "[shell][edit][io]") {
  temp_dir dir;
  const fs::path p = dir.path / "IMG_0003.jpg";
  const auto bytes = jpeg_bytes(64, 48);
  {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  // An op made against other bytes is refused.
  CHECK_FALSE(mv::shell::run_rotation_write({p.string(), bytes.size() + 1, mv::codec::kRotateCw}));
  REQUIRE(mv::shell::run_rotation_write({p.string(), bytes.size(), mv::codec::kRotateCw}));
  std::ifstream in(p, std::ios::binary);
  const std::vector<std::uint8_t> now{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  auto l = mv::edit::read_layout(now);
  REQUIRE(l);
  CHECK(l->width == 48);
  CHECK(l->height == 64);

  mv::edit::geometry g;
  g.crop = {0.25f, 0.25f, 0.5f, 0.5f};
  auto a = mv::shell::run_export(p.string(), g, {});
  REQUIRE(a);
  CHECK(fs::path(*a).filename() == "IMG_0003-edit.jpg");
  auto b = mv::shell::run_export(p.string(), g, {});
  REQUIRE(b);
  CHECK(fs::path(*b).filename() == "IMG_0003-edit (2).jpg");
  std::ifstream ex(*a, std::ios::binary);
  const std::vector<std::uint8_t> out{std::istreambuf_iterator<char>(ex), std::istreambuf_iterator<char>()};
  auto lo = mv::edit::read_layout(out);
  REQUIRE(lo);
  CHECK(lo->width == 24);
  CHECK(lo->height == 32);
}

TEST_CASE("the export dialog's choice round-trips through one small integer", "[shell][edit]") {
  for (auto format : {mv::edit::image_format::jpeg, mv::edit::image_format::png}) {
    for (auto policy : {mv::edit::metadata_policy::all, mv::edit::metadata_policy::minus_gps,
                        mv::edit::metadata_policy::none}) {
      for (int edge = 0; edge < mv::shell::kExportLongEdgeCount; ++edge) {
        for (int quality : {1, 75, 92, 100}) {
          mv::edit::export_options opt;
          opt.encode.format = format;
          opt.encode.quality = quality;
          opt.policy = policy;
          opt.long_edge = mv::shell::kExportLongEdges[edge];
          const std::int32_t packed = mv::shell::pack_export(opt);
          // The island's callback carries a float: every packed value is exact.
          CHECK(static_cast<std::int32_t>(static_cast<float>(packed)) == packed);
          const auto back = mv::shell::unpack_export(packed);
          CHECK(back.encode.format == format);
          CHECK(back.encode.quality == quality);
          CHECK(back.policy == policy);
          CHECK(back.long_edge == opt.long_edge);
        }
      }
    }
  }
  // Garbage never yields an out-of-range option.
  const auto junk = mv::shell::unpack_export(-1);
  CHECK(junk.encode.quality >= 1);
  CHECK(junk.encode.quality <= 100);
  CHECK(static_cast<int>(junk.policy) <= 2);
}

TEST_CASE("an edited tiled image picks its tiles in source space", "[shell][edit][tiles]") {
  // 8000 x 6000 source, turned clockwise: the output is 6000 x 8000.
  mv::edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  const auto p = mv::edit::place(g, {8000, 6000});
  REQUIRE(p.cropped == mv::edit::size2{6000, 8000});
  // The camera looks at output pixel (1000, 2000), zoom 1, a 1600 x 900 view.
  const auto s = mv::shell::view_in_source(p, 8000, 6000, 1000, 2000, 1.0f, 1600, 900);
  // Output (x, y) shows source (y, H - 1 - x): (2000, 5000).
  CHECK(std::abs(s.pan_x - 2000.0f) < 0.5f);
  CHECK(std::abs(s.pan_y - 5000.0f) < 0.5f);
  CHECK(std::abs(s.view_w - 900.0f) < 0.5f);  // a quarter turn swaps the extents
  CHECK(std::abs(s.view_h - 1600.0f) < 0.5f);
  CHECK(s.zoom == 1.0f);

  // The identity is the camera itself.
  const auto id = mv::edit::place({}, {8000, 6000});
  const auto t = mv::shell::view_in_source(id, 8000, 6000, 1234, 567, 0.5f, 1600, 900);
  CHECK(std::abs(t.pan_x - 1234.0f) < 0.5f);
  CHECK(std::abs(t.view_w - 1600.0f) < 0.5f);

  // A straighten grows the box that covers the rotated viewport.
  mv::edit::geometry st;
  st.straighten = 10.0f;
  const auto ps = mv::edit::place(st, {8000, 6000});
  const auto u = mv::shell::view_in_source(ps, 8000, 6000, 2000, 2000, 1.0f, 1600, 900);
  CHECK(u.view_w > 1600.0f);
  CHECK(u.view_h > 900.0f);
}

TEST_CASE("the geometry map inverts exactly", "[shell][edit][tiles]") {
  for (int o = 1; o <= 8; ++o) {
    mv::edit::geometry g;
    g.orient = mv::codec::from_exif(o);
    g.straighten = o == 5 ? 7.5f : 0.0f;
    g.crop = {0.1f, 0.2f, 0.5f, 0.6f};
    const auto p = mv::edit::place(g, {4000, 3000});
    const auto inv = mv::edit::invert(p.map);
    for (float u : {0.0f, 0.3f, 1.0f}) {
      for (float v : {0.0f, 0.7f, 1.0f}) {
        const float su = p.map.m[0] * u + p.map.m[1] * v + p.map.m[2];
        const float sv = p.map.m[3] * u + p.map.m[4] * v + p.map.m[5];
        CHECK(std::abs(inv.m[0] * su + inv.m[1] * sv + inv.m[2] - u) < 1e-4f);
        CHECK(std::abs(inv.m[3] * su + inv.m[4] * sv + inv.m[5] - v) < 1e-4f);
      }
    }
  }
}

TEST_CASE("a metadata-only rewrite keeps the item's edits", "[shell][edit][meta]") {
  // PR 12: a rating or comment rewrites a JPEG's bytes, not its pixels. The
  // stack is keyed by (path, size, mtime), so the host tells the session.
  edit_session s;
  s.set_item(jpeg_item(1000, 1));
  s.set_size(600, 400);
  REQUIRE(s.run(command_id::flip_horizontal) == edit_effect::write_rotation);

  s.metadata_rewritten("/photos/IMG_0001.JPG", 1000, 1, 1180, 5);
  // The same file at its new identity still has its stack...
  s.set_item(jpeg_item(1180, 5));
  s.set_size(600, 400);
  REQUIRE(s.stack() != nullptr);
  CHECK_FALSE(s.export_geometry().orient.identity());
  // ...and a lossless rotation now checks against the bytes that are there.
  const auto w = s.take_pending_write();
  REQUIRE(w);
  CHECK(w->size == 1180);
}

TEST_CASE("a metadata rewrite of a file that is not open moves its stack too", "[shell][edit][meta]") {
  edit_session s;
  s.set_item(jpeg_item(1000, 1));
  s.set_size(600, 400);
  REQUIRE(s.set_adjust(mv::edit::adjust_param::contrast, 0.25f) == edit_effect::redraw);
  // The user has walked to another file; the write on the first one lands late.
  edit_item other = jpeg_item(500, 9);
  other.path = "/photos/IMG_0002.JPG";
  s.set_item(other);
  s.metadata_rewritten("/photos/IMG_0001.JPG", 1000, 1, 1100, 7);
  s.set_item(jpeg_item(1100, 7));
  REQUIRE(s.stack() != nullptr);
  CHECK_FALSE(s.colour().identity());
}
