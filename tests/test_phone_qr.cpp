// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Phone access card's QR in the portable core (addons/phone/qr.h), the
// twin of src.managed/Shared/QrCode.cs. The matrices pinned here are the ones
// QrCodeTests.cs pins (read back by Apple's CIDetector when the C# encoder was
// written, 2026-11-12); a change that moves a module fails here and there.
#include <sodium.h>

#include "catch_compat.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "addons/phone/qr.h"

namespace qr = mv::phone::qr;

namespace {

// The SHA-256 (upper-case hex, as Convert.ToHexString gives) of the matrix
// as '0'/'1' rows, each ended by '\n': QrCodeTests.Matrix.
std::string matrix_sha(const qr::code& q) {
  std::string text;
  for (int y = 0; y < q.size; ++y) {
    for (int x = 0; x < q.size; ++x) text += q.dark(x, y) ? '1' : '0';
    text += '\n';
  }
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(text.data()), text.size());
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char b : h) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

}  // namespace

TEST_CASE("phone qr: the matrix is the one a scanner read back", "[phone-qr]") {
  struct row {
    std::string_view text;
    int version;
    int mask;
    std::string_view sha;
  };
  const row rows[] = {
      {"hello", 1, 7, "D9E93C47DF2FD3419021124B6F90CDE9ADD476DDA7D7B9E721F3B2463D312960"},
      {"mediaviewer://pair#eyJ2IjoxLCJsaWJyYXJ5X2lkIjoiMDEyMyJ9", 4, 1,
       "EFAB0B5E7E736915C0DAE4FB6187AD984C23D93359D6A1C3F451FD1A78B2E2A1"},
  };
  for (const row& r : rows) {
    INFO(std::string(r.text));
    const auto q = qr::encode(r.text);
    REQUIRE(q.has_value());
    CHECK(q->version == r.version);
    CHECK(q->size == r.version * 4 + 17);
    CHECK(q->modules.size() == static_cast<std::size_t>(q->size * q->size));
    CHECK(q->mask == r.mask);
    CHECK(matrix_sha(*q) == r.sha);
  }
}

TEST_CASE("phone qr: the smallest version that holds the bytes", "[phone-qr]") {
  const auto version_for = [](std::size_t n) {
    const std::string zeros(n, '\0');
    const auto q = qr::encode(zeros);
    return q ? q->version : -1;
  };
  CHECK(version_for(14) == 1);     // version 1-M holds 14 bytes
  CHECK(version_for(15) == 2);
  CHECK(version_for(2331) == 40);  // version 40-M holds 2,331
}

TEST_CASE("phone qr: too long is nullopt", "[phone-qr]") {
  CHECK_FALSE(qr::encode(std::string(2332, '\0')).has_value());
}

TEST_CASE("phone qr: finder patterns sit in three corners", "[phone-qr]") {
  const auto q = qr::encode("mediaviewer://pair#x");
  REQUIRE(q.has_value());
  const int s = q->size;
  const int centres[3][2] = {{3, 3}, {s - 4, 3}, {3, s - 4}};
  for (const auto& c : centres) {
    CHECK(q->dark(c[0], c[1]));            // the 3x3 centre
    CHECK_FALSE(q->dark(c[0] + 2, c[1]));  // the light ring
    CHECK(q->dark(c[0] + 3, c[1]));        // the dark ring
  }
  CHECK_FALSE(q->dark(-1, 0));  // outside reads as light (the quiet zone)
}

TEST_CASE("phone qr: drawn as BGRA with a four-module quiet zone", "[phone-qr]") {
  const auto q = qr::encode("hello");  // version 1: 21 modules, 29 with the quiet zone
  REQUIRE(q.has_value());
  REQUIRE(q->size == 21);

  std::vector<std::uint8_t> px;
  int side = -1;
  qr::draw_bgra(*q, 300, px, side);  // floor(300 / 29) = 10 px a module
  REQUIRE(side == 290);
  REQUIRE(px.size() == static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4);

  const auto pixel = [&](int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)) * 4;
    return std::vector<std::uint8_t>(px.begin() + static_cast<std::ptrdiff_t>(i),
                                     px.begin() + static_cast<std::ptrdiff_t>(i + 4));
  };
  const std::vector<std::uint8_t> white{255, 255, 255, 255}, black{0, 0, 0, 255};

  // The quiet zone is white all round.
  for (int i = 0; i < side; ++i)
    for (int d = 0; d < 40; d += 13) {
      CHECK(pixel(i, d) == white);
      CHECK(pixel(d, i) == white);
      CHECK(pixel(i, side - 1 - d) == white);
      CHECK(pixel(side - 1 - d, i) == white);
    }
  // Module (0,0) is the top-left finder's dark corner: pixels 40..49.
  CHECK(pixel(40, 40) == black);
  CHECK(pixel(49, 49) == black);
  CHECK(pixel(39, 40) == white);
  // Module (1,1) is the finder's light ring.
  CHECK(pixel(55, 55) == white);
  // Module (3,3), the finder's centre.
  CHECK(pixel(75, 75) == black);

  // Every module matches the code.
  for (int y = 0; y < q->size; ++y)
    for (int x = 0; x < q->size; ++x) CHECK(pixel((x + 4) * 10 + 5, (y + 4) * 10 + 5) == (q->dark(x, y) ? black : white));

  // Too small still gives one pixel a module.
  qr::draw_bgra(*q, 10, px, side);
  CHECK(side == 29);
  CHECK(px.size() == 29u * 29u * 4u);

  // An empty code draws nothing.
  qr::draw_bgra(qr::code{}, 300, px, side);
  CHECK(side == 0);
  CHECK(px.empty());
}
