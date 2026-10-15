// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text in images (docs/design/16): the pixels both hosts' recognisers are
// handed, the word layout, hit-testing and selection, and the notices. The
// recognisers themselves (Vision, Windows.Media.Ocr) are OS API and are faked
// here.
#include <catch2/catch_approx.hpp>
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
using mv::shell::read_text;
using mv::shell::text_layout;
using mv::shell::text_selection;
using mv::shell::text_word;
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
  text_layout layout;
  std::uint32_t seen_w = 0, seen_h = 0;
  int calls = 0;
  text_recognition recognise(const text_image& image, text_layout& out) noexcept override {
    ++calls;
    seen_w = image.width;
    seen_h = image.height;
    out = layout;
    return answer;
  }
};

text_word word(float x0, float y0, float x1, float y1, std::uint32_t line, const char* text) {
  text_word w;
  w.x0 = x0;
  w.y0 = y0;
  w.x1 = x1;
  w.y1 = y1;
  w.line = line;
  w.text = text;
  return w;
}

// Two lines: "Hello world" at the top, "second line here" below it.
text_layout two_lines() {
  text_layout l;
  l.words = {word(0.10f, 0.10f, 0.30f, 0.20f, 0, "Hello"), word(0.35f, 0.10f, 0.60f, 0.20f, 0, "world"),
             word(0.10f, 0.40f, 0.30f, 0.50f, 1, "second"), word(0.35f, 0.40f, 0.50f, 0.50f, 1, "line"),
             word(0.55f, 0.40f, 0.70f, 0.50f, 1, "here")};
  return l;
}

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

TEST_CASE("recognition runs the port once", "[shell][text]") {
  temp_png png(16, 16);
  fake_recognizer r;
  SECTION("words") {
    r.layout = two_lines();
    const read_text out = mv::shell::run_text_recognition(png.path.string(), {}, r);
    REQUIRE(r.calls == 1);
    REQUIRE(r.seen_w == 16);
    REQUIRE(out.outcome == text_recognition::ok);
    REQUIRE(out.layout.words.size() == 5);
    REQUIRE(mv::shell::text_recognition_notice(out).empty());
  }
  SECTION("Windows without an OCR language says so, once there is something to say") {
    r.answer = text_recognition::no_language;
    r.layout = two_lines();
    const read_text out = mv::shell::run_text_recognition(png.path.string(), {}, r);
    REQUIRE(out.layout.empty());
    REQUIRE(mv::shell::text_recognition_notice(out).find("language") != std::string::npos);
  }
  SECTION("an unreadable file never reaches the recogniser, and says nothing") {
    const read_text out = mv::shell::run_text_recognition((png.path.string() + ".missing"), {}, r);
    REQUIRE(r.calls == 0);
    REQUIRE(out.unreadable);
    REQUIRE(mv::shell::text_recognition_notice(out).empty());
  }
}

TEST_CASE("Windows' word records parse into fractions of the image", "[shell][text]") {
  const text_layout l = mv::shell::parse_word_records(
      "0\t10\t20\t30\t10\tHello,\n0\t50\t20\t40\t10\tworld\r\n"
      "bad line\n1\tx\t0\t1\t1\tnope\n1\t10\t40\t20\t10\tcaf\xC3\xA9\n",
      100, 200);
  REQUIRE(l.words.size() == 3);
  REQUIRE(l.words[0].text == "Hello,");
  REQUIRE(l.words[0].x0 == 0.1f);
  REQUIRE(l.words[0].y0 == 0.1f);
  REQUIRE(l.words[0].x1 == 0.4f);
  REQUIRE(l.words[1].text == "world");
  REQUIRE(l.words[2].line == 1);
  REQUIRE(l.words[2].text == "caf\xC3\xA9");
  REQUIRE(mv::shell::parse_word_records("0\t1\t1\t1\t1\tx\n", 0, 10).empty());
}

TEST_CASE("a line boxed as a whole is shared out by length", "[shell][text]") {
  text_layout l;
  mv::shell::add_line_split_evenly(l, 3, "ab  cd\xC3\xA9", 0.0f, 0.5f, 0.7f, 0.6f);
  REQUIRE(l.words.size() == 2);
  REQUIRE(l.words[0].text == "ab");
  REQUIRE(l.words[1].text == "cd\xC3\xA9");
  REQUIRE(l.words[0].line == 3);
  REQUIRE(l.words[0].x0 == 0.0f);
  REQUIRE(l.words[0].x1 == Catch::Approx(0.2f));  // 2 of 7 code points
  REQUIRE(l.words[1].x0 == Catch::Approx(0.4f));
  REQUIRE(l.words[1].x1 == Catch::Approx(0.7f));
  REQUIRE(l.words[1].y0 == 0.5f);
}

TEST_CASE("pressing on a word finds it; off the text finds nothing", "[shell][text]") {
  const text_layout l = two_lines();
  REQUIRE(mv::shell::word_at(l, 0.2f, 0.15f, 1.0f) == 0);
  REQUIRE(mv::shell::word_at(l, 0.5f, 0.45f, 1.0f) == 3);  // between "line" and "here": nearer "line"
  REQUIRE(mv::shell::word_at(l, 0.2f, 0.215f, 1.0f) == 0);  // just under the box, inside the slop
  REQUIRE(mv::shell::word_at(l, 0.2f, 0.3f, 1.0f) == -1);
  REQUIRE(mv::shell::word_at(l, 0.9f, 0.9f, 1.0f) == -1);
  REQUIRE(mv::shell::word_at(text_layout{}, 0.2f, 0.15f, 1.0f) == -1);
}

TEST_CASE("a drag off the text extends to the nearest line and word", "[shell][text]") {
  const text_layout l = two_lines();
  REQUIRE(mv::shell::nearest_word(l, 0.95f, 0.15f) == 1);  // right of "world"
  REQUIRE(mv::shell::nearest_word(l, 0.0f, 0.9f) == 2);    // below, left: "second"
  REQUIRE(mv::shell::nearest_word(l, 0.9f, 0.9f) == 4);    // below, right: "here"
  REQUIRE(mv::shell::nearest_word(text_layout{}, 0.5f, 0.5f) == -1);
  int first = 0, last = 0;
  mv::shell::line_bounds(l, 3, first, last);
  REQUIRE(first == 2);
  REQUIRE(last == 4);
  mv::shell::line_bounds(l, 9, first, last);
  REQUIRE(first == -1);
}

TEST_CASE("the selection reads in order and highlights line by line", "[shell][text]") {
  const text_layout l = two_lines();
  REQUIRE(mv::shell::selected_text(l, text_selection{3, 1}) == "world\nsecond line");
  REQUIRE(mv::shell::selected_text(l, text_selection{0, 0}) == "Hello");
  REQUIRE(mv::shell::selected_text(l, text_selection{}).empty());
  REQUIRE(mv::shell::selected_text(l, text_selection{0, 7}).empty());
  std::vector<mv::shell::text_rect> rects;
  mv::shell::selection_rects(l, text_selection{1, 3}, rects);
  REQUIRE(rects.size() == 2);
  REQUIRE(rects[0].x0 == 0.35f);
  REQUIRE(rects[0].x1 == 0.60f);
  REQUIRE(rects[1].x0 == 0.10f);
  REQUIRE(rects[1].x1 == 0.50f);
  mv::shell::selection_rects(l, text_selection{}, rects);
  REQUIRE(rects.empty());
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

TEST_CASE("the copy notice counts words", "[shell][text]") {
  REQUIRE(mv::shell::copy_text_notice(1) == "Copied 1 word");
  REQUIRE(mv::shell::copy_text_notice(42) == "Copied 42 words");
  REQUIRE(mv::shell::tidy_text("\n\nline\n\n\nnext\n") == "line\n\n\nnext");
}
