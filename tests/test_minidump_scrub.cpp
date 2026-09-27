// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// plan/13 Part 2 scrub, on a synthetic minidump shaped like Crashpad's.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "codec/crash_test_hook.h"
#include "core/crash_context.h"
#include "shell/minidump_scrub.h"
#include "shell/settings.h"

TEST_CASE("crash consent is asked only with a report, an endpoint, and no answer", "[crash]") {
  mv::shell::crash_settings s;
  CHECK_FALSE(mv::shell::should_ask_crash_consent(s, 3));  // PR 7: no URL
  s.upload_url = "https://example.invalid/submit";
  CHECK_FALSE(mv::shell::should_ask_crash_consent(s, 0));
  CHECK(mv::shell::should_ask_crash_consent(s, 1));
  s.consent = 0;
  CHECK_FALSE(mv::shell::should_ask_crash_consent(s, 1));
  s.consent = 1;
  CHECK_FALSE(mv::shell::should_ask_crash_consent(s, 1));
}

namespace {

struct dump_builder {
  std::vector<std::uint8_t> b;
  void u32(std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  void u64(std::size_t o, std::uint64_t v) {
    u32(o, static_cast<std::uint32_t>(v));
    u32(o + 4, static_cast<std::uint32_t>(v >> 32));
  }
  void ascii(std::size_t o, std::string_view s) { std::memcpy(b.data() + o, s.data(), s.size()); }
  void wide(std::size_t o, std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
      b[o + 2 * i] = static_cast<std::uint8_t>(s[i]);
      b[o + 2 * i + 1] = 0;
    }
  }
};

bool contains(const std::vector<std::uint8_t>& hay, std::string_view needle, bool wide) {
  std::vector<std::uint8_t> n;
  for (char c : needle) {
    n.push_back(static_cast<std::uint8_t>(c));
    if (wide) n.push_back(0);
  }
  return std::search(hay.begin(), hay.end(), n.begin(), n.end()) != hay.end();
}

// Layout: header | dir(3) | threadlist | memorylist | module-ish stream |
//         stack bytes (VA 0x10000) | PEB bytes (VA 0x90000) | context
std::vector<std::uint8_t> make_dump() {
  dump_builder d;
  d.b.assign(0x2000, 0);
  d.u32(0, 0x504D444D);
  d.u32(4, 42899);
  d.u32(8, 3);     // streams
  d.u32(12, 32);   // dir rva
  // dir
  d.u32(32, 3);  d.u32(36, 4 + 48); d.u32(40, 0x100);   // thread list
  d.u32(44, 5);  d.u32(48, 4 + 32); d.u32(52, 0x200);   // memory list
  d.u32(56, 4);  d.u32(60, 0x100);  d.u32(64, 0x300);   // module list (strings only here)
  // thread: stack VA 0x10000 size 0x400 rva 0x800, context 0x40 @ 0x1F00
  d.u32(0x100, 1);
  d.u64(0x104 + 24, 0x10000);
  d.u32(0x104 + 32, 0x400);
  d.u32(0x104 + 36, 0x800);
  d.u32(0x104 + 40, 0x40);
  d.u32(0x104 + 44, 0x1F00);
  // memory list: stack, then PEB
  d.u32(0x200, 2);
  d.u64(0x204, 0x10000); d.u32(0x204 + 8, 0x400); d.u32(0x204 + 12, 0x800);
  d.u64(0x214, 0x90000); d.u32(0x214 + 8, 0x400); d.u32(0x214 + 12, 0xC00);
  // module path, UTF-16
  d.wide(0x304, "C:\\Users\\alice\\AppData\\Local\\MediaViewer\\mediaviewer_core.dll");
  // stack: UTF-8 path, UTF-16 bare filename, identity token
  d.ascii(0x820, "D:\\PRIVATE_FOLDER_canary\\SECRET_FILENAME_canary_7Q3.dng");
  d.wide(0x901, "IMG_SECRET_4242.NEF");  // odd parity on purpose
  d.ascii(0xA00, "hello alice-pc done");
  // PEB: command line + a pixel run
  d.wide(0xC10, "mediaviewer_lab.exe \"E:\\photos\\x.cr2\"");
  for (int i = 0; i < 64; ++i) d.b[0xD00 + i] = static_cast<std::uint8_t>(0xA0 + (i % 7));
  // context bytes that look like text must survive
  d.ascii(0x1F00, "C:\\ctx");
  return d.b;
}

}  // namespace

TEST_CASE("scrub zeroes captured memory outside thread stacks", "[crash][scrub]") {
  auto dump = make_dump();
  const auto r = mv::shell::scrub_minidump(dump, {{"alice", "alice-pc"}});
  REQUIRE(r.valid);
  CHECK(r.memory_ranges == 2);
  CHECK(r.zeroed_bytes > 0);
  for (std::size_t i = 0xC00; i < 0x1000; ++i) REQUIRE(dump[i] == 0);
  CHECK_FALSE(contains(dump, "photos", true));
}

TEST_CASE("scrub masks paths, media names and identities; keeps module layout", "[crash][scrub]") {
  auto dump = make_dump();
  (void)mv::shell::scrub_minidump(dump, {{"alice", "alice-pc"}});
  CHECK_FALSE(contains(dump, "PRIVATE_FOLDER_canary", false));
  CHECK_FALSE(contains(dump, "SECRET_FILENAME_canary_7Q3", false));
  CHECK_FALSE(contains(dump, "IMG_SECRET_4242", true));
  CHECK_FALSE(contains(dump, "alice", true));
  CHECK_FALSE(contains(dump, "alice", false));
  CHECK(contains(dump, "\\AppData\\Local\\MediaViewer\\mediaviewer_core.dll", true));
  CHECK(contains(dump, "C:\\Users\\", true));
  CHECK(contains(dump, "C:\\ctx", false));  // thread CONTEXT is structural
  CHECK(mv::shell::minidump_is_scrubbed(dump));
}

TEST_CASE("scrub is idempotent and rejects non-minidumps", "[crash][scrub]") {
  auto dump = make_dump();
  (void)mv::shell::scrub_minidump(dump, {{"alice"}});
  const auto once = dump;
  const auto again = mv::shell::scrub_minidump(dump, {{"alice"}});
  CHECK(again.already_scrubbed);
  CHECK(dump == once);

  std::vector<std::uint8_t> junk(64, 'x');
  const auto before = junk;
  CHECK_FALSE(mv::shell::scrub_minidump(junk, {}).valid);
  CHECK(junk == before);
}

TEST_CASE("scrub_text strips paths and filenames from a managed message", "[crash][scrub]") {
  const std::string s = mv::shell::scrub_text(
      "Could not open C:\\Users\\bob\\Pictures\\trip\\IMG_0001.HEIC (0x80070002) as bob", {{"bob"}});
  CHECK(s.find("Pictures") == std::string::npos);
  CHECK(s.find("IMG_0001") == std::string::npos);
  CHECK(s.find("bob") == std::string::npos);
  CHECK(s.find("0x80070002") != std::string::npos);
}

// ---- PR 11: the same scrub on a macOS dump (plan/13: "the same Crashpad
// handler and the same scrub") --------------------------------------------------

namespace {

// Same layout as make_dump(), with what a Mac Crashpad dump carries instead:
// POSIX paths in the module list (a bundle binary with no extension, a dylib
// under the user's home), the canary folder and filename on the stack, the
// Swift runtime's crash-info message, an SD card under /Volumes, and the
// short and full user names.
std::vector<std::uint8_t> make_mac_dump() {
  auto dump = make_dump();
  dump_builder d;
  d.b = std::move(dump);
  std::fill(d.b.begin() + 0x300, d.b.begin() + 0x400, std::uint8_t{0});
  std::fill(d.b.begin() + 0x800, d.b.begin() + 0xC00, std::uint8_t{0});
  d.ascii(0x304, "/Users/alice/Applications/MediaViewer.app/Contents/MacOS/MediaViewer");
  d.ascii(0x360, "/Users/alice/Library/Frameworks/libraw_r.23.dylib");
  d.ascii(0x3A0, "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit");
  d.ascii(0x820, "open /Users/alice/Pictures/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.cr2 failed");
  // A rooted path whose prefix a later frame overwrote. No drive, no /Users.
  // 32 bytes, ending exactly at the next string. The wide twin sits further down the stack.
  d.ascii(0x880, "pad/canary/PRIVATE_FOLDER_canary");
  d.ascii(0x8A0, "card=/Volumes/EOS_DIGITAL/DCIM/100CANON/IMG_0042.CR3");
  d.ascii(0x900, "tmp /private/var/folders/xy/T/MediaViewer/thumbs.sqlite");
  d.ascii(0x960, "Fatal error: Index out of range (Alice Smith on alices-macbook)");
  d.ascii(0x9C0, "see https://example.com/a/b and ./rel/path/");
  d.wide(0xB00, "pad/canary/PRIVATE_FOLDER_canary");
  return d.b;
}

}  // namespace

TEST_CASE("scrub masks macOS paths and identities; keeps bundle and system module layout",
          "[crash][scrub][mac]") {
  auto dump = make_mac_dump();
  const auto r = mv::shell::scrub_minidump(dump, {{"alice", "Alice Smith", "alices-macbook"}});
  REQUIRE(r.valid);
  // The canaries of the verify (plan/10 PR 11, macOS crash reporting),
  // including the unrooted stack fragment in both encodings.
  CHECK_FALSE(contains(dump, "PRIVATE_FOLDER_canary", false));
  CHECK_FALSE(contains(dump, "PRIVATE_FOLDER_canary", true));
  CHECK_FALSE(contains(dump, "SECRET_FILENAME_canary_7Q3", false));
  CHECK_FALSE(contains(dump, "Pictures", false));
  CHECK_FALSE(contains(dump, "EOS_DIGITAL", false));
  CHECK_FALSE(contains(dump, "IMG_0042", false));
  CHECK_FALSE(contains(dump, "thumbs.sqlite", false));
  CHECK_FALSE(contains(dump, "alice", false));
  CHECK_FALSE(contains(dump, "Alice Smith", false));
  CHECK_FALSE(contains(dump, "alices-macbook", false));
  // Symbolication keeps its map: the bundle and dylib layout, the system frameworks.
  CHECK(contains(dump, "/Users/", false));
  CHECK(contains(dump, "/Applications/MediaViewer.app/Contents/MacOS/MediaViewer", false));
  CHECK(contains(dump, "/Library/Frameworks/libraw_r.23.dylib", false));
  CHECK(contains(dump, "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", false));
  // Not paths: a URL's authority and a relative path are left alone.
  CHECK(contains(dump, "https://example.com/a/b", false));
  CHECK(contains(dump, "./rel/path/", false));
  CHECK(contains(dump, "Fatal error: Index out of range", false));
}

TEST_CASE("scrub_text strips a macOS path from an NSException reason", "[crash][scrub][mac]") {
  const std::string s = mv::shell::scrub_text(
      "*** -[NSURL initFileURLWithPath:]: /Users/bob/Desktop/trip/IMG_0001.HEIC is gone (bob)",
      {{"bob"}});
  CHECK(s.find("Desktop") == std::string::npos);
  CHECK(s.find("IMG_0001") == std::string::npos);
  CHECK(s.find("bob") == std::string::npos);
  CHECK(s.find("initFileURLWithPath") != std::string::npos);
  CHECK(s.find("/Users/") != std::string::npos);
}

TEST_CASE("scrub_text masks an unrooted path fragment and keeps a url and a relative path",
          "[crash][scrub][mac]") {
  const std::string s = mv::shell::scrub_text(
      "see pad/canary/PRIVATE_FOLDER_canary and https://example.com/a/b and ./rel/path/", {});
  CHECK(s.find("PRIVATE_FOLDER_canary") == std::string::npos);
  CHECK(s.find("pad/canary") == std::string::npos);
  CHECK(s.find("https://example.com/a/b") != std::string::npos);
  CHECK(s.find("./rel/path/") != std::string::npos);
}

TEST_CASE("decode crash scope annotates without paths and is inert unarmed", "[crash]") {
  std::vector<std::uint8_t> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const std::string_view marker = mv::codec::kCrashTestMarker;
  tiff.insert(tiff.end(), marker.begin(), marker.end());
  CHECK(mv::codec::crash_test_marker_present(tiff));
  REQUIRE_FALSE(mv::codec::crash_test_armed());  // MV_CRASH_TEST must not be set for tests
  {
    const mv::crash_context::correlation_scope cid(77);
    const mv::codec::decode_crash_scope scope(tiff, nullptr);
    const std::string slot = mv::crash_context::this_thread_slot();
    CHECK(slot.find("fmt=TIFF") != std::string::npos);
    CHECK(slot.find("cid=77") != std::string::npos);
    mv::crash_context::note_geometry(6000, 4000, 14);
    CHECK(std::string(mv::crash_context::this_thread_slot()).find("6000x4000 14bit") !=
          std::string::npos);
  }
  CHECK(std::string(mv::crash_context::this_thread_slot()).empty());

  // Room for the whole marker past the scan window (+10 overran it by 9 bytes).
  std::vector<std::uint8_t> far(mv::codec::kCrashTestScanBytes + marker.size(), 0);
  std::memcpy(far.data() + mv::codec::kCrashTestScanBytes, marker.data(), marker.size());
  CHECK_FALSE(mv::codec::crash_test_marker_present(far));
}
