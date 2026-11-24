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
  // docs/design/26: the item itself has no file; its previews and on-view downloads
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

TEST_CASE("a remote folder's keys and cache are read-only, with their own words", "[shell][write_guard][desktop-client]") {
  using namespace mv::shell;
  CHECK(write_protected("remote:0123456789abcdef/a1a1a1a1a1a1a1a1/IMG_0001.JPG"));
  CHECK(is_remote("remote:0123456789abcdef/a1a1a1a1a1a1a1a1/IMG_0001.JPG"));
  set_remote_cache_prefix("/Users/x/Library/Caches/MediaViewer/Remote");
  CHECK(write_protected("/Users/x/Library/Caches/MediaViewer/Remote/0123456789abcdef/h/IMG_0001.JPG"));
  CHECK_FALSE(write_protected("/Users/x/Library/Caches/MediaViewer/RemoteX/a.jpg"));
  CHECK(std::string(write_protected_notice("remote:0123456789abcdef/a1a1a1a1a1a1a1a1/a.jpg")) ==
        kRemoteWriteProtectedNotice);
  CHECK(std::string(write_protected_notice("photos:abc")) == kWriteProtectedNotice);
  set_remote_cache_prefix("");
  CHECK_FALSE(write_protected("/Users/x/Library/Caches/MediaViewer/Remote/0123456789abcdef/h/IMG_0001.JPG"));
}

TEST_CASE("a change device may add, make folders, move, rename and delete on writable remote keys, nothing else",
          "[shell][write_guard][desktop-client]") {
  using namespace mv::shell;
  const std::string writable = "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/Trips/IMG_0001.JPG";
  const std::string viewed = "remote:0123456789abcdef/b2b2b2b2b2b2b2b2/IMG_0002.JPG";
  set_remote_writable([](std::string_view key) { return key.find("/a1a1a1a1a1a1a1a1/") != std::string_view::npos; });
  for (const auto op : {file_change::add, file_change::make_folder, file_change::move, file_change::rename,
                        file_change::remove}) {
    CHECK(change_allowed(writable, op));
    CHECK_FALSE(change_allowed(viewed, op));  // that folder: view only
    CHECK(change_allowed("/Users/a/Pictures/IMG_3.jpg", op));  // a local file, as before
    CHECK_FALSE(change_allowed("photos:0A1B2C3D/L0/001", op));
  }
  // Ratings, metadata and edits ask write_protected, which a remote file stays.
  CHECK(write_protected(writable));
  // Its cached original is a file here that stands for it: never changed as if it were it.
  set_remote_cache_prefix("/c/Remote");
  CHECK_FALSE(change_allowed("/c/Remote/0123456789abcdef/h/IMG_0001.JPG", file_change::remove));
  set_remote_cache_prefix("");
  // No check registered (a host without the library): nothing remote may change.
  set_remote_writable(nullptr);
  CHECK_FALSE(change_allowed(writable, file_change::add));
}
