// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Copy Text in Image (docs/design/16 View): the pixels both hosts' recognisers
// are handed, the tidy-up, the word count and the notice. The recognisers
// themselves (Vision, Windows.Media.Ocr) are OS API and are faked here.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "edit/encode.h"
#include "shell/text_in_image.h"

namespace {

namespace fs = std::filesystem;
using mv::shell::copied_text;
using mv::shell::text_image;
using mv::shell::text_pixel_order;
using mv::shell::text_recognition;

struct temp_png {
  fs::path path;
  // w x h, left half opaque red, right half fully transparent.
  temp_png(std::uint32_t w, std::uint32_t h) {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("mv_text_in_image_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(++counter) + ".png");
    mv::codec::raster r;
    r.width = w;
    r.height = h;
    r.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w / 2; ++x) {
        std::uint8_t* px = &r.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
        px[0] = 255;
        px[3] = 255;
      }
    }
    mv::edit::encode_options opt;
    opt.format = mv::edit::image_format::png;
    auto bytes = mv::edit::encode(r, opt, {});
    REQUIRE(bytes);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
  }
  ~temp_png() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

struct fake_recognizer final : mv::shell::text_recognizer {
  text_recognition answer = text_recognition::ok;
  std::string text;
  std::uint32_t seen_w = 0, seen_h = 0;
  int calls = 0;
  text_recognition recognise(const text_image& image, std::string& out) noexcept override {
    ++calls;
    seen_w = image.width;
    seen_h = image.height;
    out = text;
    return answer;
  }
};

}  // namespace

TEST_CASE("the recogniser sees the still as the canvas shows it, opaque", "[shell][text]") {
  temp_png png(40, 20);
  SECTION("upright, transparent flattened over white") {
    auto image = mv::shell::render_text_image(png.path.string(), {});
    REQUIRE(image);
    REQUIRE(image->width == 40);
    REQUIRE(image->height == 20);
    REQUIRE(image->pixels.size() == 40u * 20u * 4u);
    const std::uint8_t* left = &image->pixels[0];
    const std::uint8_t* right = &image->pixels[39 * 4];
    REQUIRE(left[0] == 255);
    REQUIRE(left[1] == 0);
    REQUIRE(left[3] == 255);
    REQUIRE(right[0] == 255);
    REQUIRE(right[1] == 255);
    REQUIRE(right[2] == 255);
    REQUIRE(right[3] == 255);
  }
  SECTION("BGRA for Windows' SoftwareBitmap") {
    auto image = mv::shell::render_text_image(png.path.string(), {}, text_pixel_order::bgra);
    REQUIRE(image);
    REQUIRE(image->pixels[0] == 0);
    REQUIRE(image->pixels[2] == 255);
  }
  SECTION("rotated with the edit stack") {
    const mv::edit::op rotate{mv::edit::op_kind::rotate_cw};
    const mv::edit::geometry g = mv::edit::fold(std::span(&rotate, 1));
    auto image = mv::shell::render_text_image(png.path.string(), g);
    REQUIRE(image);
    REQUIRE(image->width == 20);
    REQUIRE(image->height == 40);
  }
  SECTION("scaled to the long-edge cap, aspect kept") {
    auto image = mv::shell::render_text_image(png.path.string(), {}, text_pixel_order::rgba, 10);
    REQUIRE(image);
    REQUIRE(image->width == 10);
    REQUIRE(image->height == 5);
  }
}

TEST_CASE("copy text runs the port once and counts what it read", "[shell][text]") {
  temp_png png(16, 16);
  fake_recognizer r;
  SECTION("text") {
    r.text = "  Hello  world \r\nsecond line\t\r\n\r\n";
    const copied_text out = mv::shell::run_copy_text(png.path.string(), {}, r);
    REQUIRE(r.calls == 1);
    REQUIRE(r.seen_w == 16);
    REQUIRE(out.outcome == text_recognition::ok);
    REQUIRE(out.text == "  Hello  world\nsecond line");
    REQUIRE(out.words == 4);
    REQUIRE(mv::shell::copy_text_notice(out) == "Copied 4 words");
  }
  SECTION("nothing found: an empty clipboard text, not whitespace") {
    r.text = " \n\t\n";
    const copied_text out = mv::shell::run_copy_text(png.path.string(), {}, r);
    REQUIRE(out.outcome == text_recognition::ok);
    REQUIRE(out.text.empty());
    REQUIRE(mv::shell::copy_text_notice(out) == "No text found");
  }
  SECTION("Windows without an OCR language says so") {
    r.answer = text_recognition::no_language;
    const copied_text out = mv::shell::run_copy_text(png.path.string(), {}, r);
    REQUIRE(out.text.empty());
    REQUIRE(mv::shell::copy_text_notice(out).find("language") != std::string::npos);
  }
  SECTION("an unreadable file never reaches the recogniser") {
    const copied_text out = mv::shell::run_copy_text((png.path.string() + ".missing"), {}, r);
    REQUIRE(r.calls == 0);
    REQUIRE(out.unreadable);
    REQUIRE(mv::shell::copy_text_notice(out) == "Could not read this image");
  }
}

TEST_CASE("words are runs between whitespace, ASCII or wide", "[shell][text]") {
  REQUIRE(mv::shell::count_words("") == 0);
  REQUIRE(mv::shell::count_words(" \n\t") == 0);
  REQUIRE(mv::shell::count_words("one") == 1);
  REQUIRE(mv::shell::count_words("one two\nthree") == 3);
  REQUIRE(mv::shell::count_words("a\xC2\xA0" "b") == 2);           // no-break space
  REQUIRE(mv::shell::count_words("\xE6\x97\xA5\xE3\x80\x80\xE6\x9C\xAC") == 2);  // ideographic space
  REQUIRE(mv::shell::count_words("caf\xC3\xA9 menu") == 2);
}

TEST_CASE("the notice reads like Preview's", "[shell][text]") {
  copied_text one;
  one.outcome = text_recognition::ok;
  one.words = 1;
  REQUIRE(mv::shell::copy_text_notice(one) == "Copied 1 word");
  copied_text failed;
  failed.outcome = text_recognition::failed;
  REQUIRE(mv::shell::copy_text_notice(failed) == "Could not recognise text in this image");
  REQUIRE(mv::shell::tidy_text("\n\nline\n\n\nnext\n") == "line\n\n\nnext");
}
