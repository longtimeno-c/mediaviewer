// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "io/pairing.h"

using mv::io::dir_entry;
using mv::io::pair_kind;
using mv::io::pair_listing;

namespace {

dir_entry e(const char* name, const char* path = nullptr) {
  dir_entry d;
  d.name_utf8 = name;
  d.path_utf8 = path ? path : name;
  d.size = 1;
  return d;
}

}  // namespace

TEST_CASE("RAW+JPEG of the same stem is one stop, JPEG primary", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_0123.NEF"), e("DSC_0123.JPG")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::raw_jpeg);
  REQUIRE(listed[0].primary.name_utf8 == "DSC_0123.JPG");
  REQUIRE(listed[0].secondary.name_utf8 == "DSC_0123.NEF");
}

TEST_CASE("Live Photo HEIC+MOV is one stop, still primary", "[io][pairing]") {
  auto listed = pair_listing({e("IMG_1234.MOV"), e("IMG_1234.HEIC")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::live_photo);
  REQUIRE(listed[0].primary.name_utf8 == "IMG_1234.HEIC");
  REQUIRE(listed[0].secondary.name_utf8 == "IMG_1234.MOV");
}

TEST_CASE("unpaired RAW stays a stop", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_0001.ARW"), e("other.jpg")});
  REQUIRE(listed.size() == 2);
  REQUIRE(listed[0].kind == pair_kind::none);
  REQUIRE(listed[0].primary.name_utf8 == "DSC_0001.ARW");
  REQUIRE(listed[0].secondary.path_utf8.empty());
}

TEST_CASE("three files of one stem stay separate", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_1.NEF"), e("DSC_1.JPG"), e("DSC_1.MOV")});
  REQUIRE(listed.size() == 3);
  for (const auto& it : listed) {
    REQUIRE(it.kind == pair_kind::none);
    REQUIRE(it.secondary.path_utf8.empty());
  }
}

TEST_CASE("case-insensitive stem match", "[io][pairing]") {
  auto listed = pair_listing({e("img_9.heic"), e("IMG_9.mov")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::live_photo);
}
