// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/write_guard (issue #72): the files the viewer must never change.
#include "catch_compat.h"

#include <string>
#include <vector>

#include "shell/write_guard.h"

using mv::shell::write_protected;

TEST_CASE("anything inside a Photos library bundle is write-protected", "[shell][write_guard]") {
  CHECK(write_protected("/Users/a/Pictures/Photos Library.photoslibrary/originals/4/IMG_1.HEIC"));
  CHECK(write_protected("/Volumes/Ext/Old.PhotosLibrary/resources/renders/x.jpg"));  // any case
  CHECK(write_protected("/Users/a/Pictures/Photos Library.photoslibrary"));
  CHECK_FALSE(write_protected("/Users/a/Pictures/holiday/IMG_1.HEIC"));
  CHECK_FALSE(write_protected("/Users/a/photoslibrary-notes/IMG_1.jpg"));
  CHECK_FALSE(write_protected("/Users/a/Pictures/my.photoslibrary.backup/IMG_1.jpg"));
  CHECK_FALSE(write_protected(""));
}

TEST_CASE("a Photos item key and the Photos cache folder are write-protected", "[shell][write_guard]") {
  // plan/26: the item itself has no file; its previews and on-view downloads
  // live under one folder the host registers.
  CHECK(write_protected("photos:0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9/L0/001"));
  CHECK_FALSE(write_protected("photos:"));  // the root alone is not an item
  CHECK_FALSE(write_protected("/Users/a/photos:odd/IMG_1.jpg"));
  mv::shell::set_read_only_prefix("/Users/a/Library/Caches/MediaViewer/Photos Library");
  CHECK(write_protected("/Users/a/Library/Caches/MediaViewer/Photos Library/preview/x-1/IMG_2 (preview).jpg"));
  CHECK(write_protected("/Users/a/Library/Caches/MediaViewer/Photos Library/icloud/x-1/IMG_2.MOV"));
  CHECK_FALSE(write_protected("/Users/a/Library/Caches/MediaViewer/Photos Library"));  // the folder itself
  CHECK_FALSE(write_protected("/Users/a/Library/Caches/MediaViewer/Photos Libraryx/IMG_2.jpg"));
  CHECK_FALSE(write_protected("/Users/a/Pictures/IMG_2.jpg"));
  mv::shell::set_read_only_prefix("");
  CHECK_FALSE(write_protected("/Users/a/Library/Caches/MediaViewer/Photos Library/icloud/x-1/IMG_2.MOV"));
}

TEST_CASE("a list opener's read-only files are protected until replaced", "[shell][write_guard]") {
  const std::vector<std::string> previews{"/c/Photos Library/x/IMG_2 (preview).jpg"};
  mv::shell::set_read_only_paths(previews);
  CHECK(write_protected(previews[0]));
  const std::vector<std::string> mixed{"/Users/a/free.jpg", previews[0]};
  CHECK(mv::shell::any_write_protected(mixed));
  mv::shell::set_read_only_paths({});
  CHECK_FALSE(write_protected(previews[0]));
  CHECK_FALSE(mv::shell::any_write_protected(mixed));
}
