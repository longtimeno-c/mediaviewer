// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 7 verify: "an iPhone Live Photo is one entry and `;` plays the motion."
//
// tests/test_pairing.cpp proves the pairing arithmetic on hand-made dir_entry
// structs, and tests/test_folder.cpp proves the ABI carries a pair through —
// with renamed BMPs standing in for everything. Neither answers the question a
// person with an iPhone is actually being asked to settle, which is whether a
// camera roll as the phone writes it comes out as the stops we claim.
//
// So this file uses a real directory with the names an iPhone and an iCloud
// download really produce (IMG_0001.HEIC + .MOV, the "Most Compatible" JPG
// pair, the IMG_E#### edited copy, the .AAE edit sidecar, an iCloud "(1)"
// duplicate, the .HEIC whose motion half never came down), real HEIC bytes for
// the stills, and — when the corpus is present — a real, playable clip as the
// motion half, opened through the same ABI calls the `;` command makes.
//
// What is still a human check afterwards: that a file straight off a phone
// carries the names assumed here. That is one look at a DCIM folder, not a
// judgement about whether the pairing logic is right.
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "abi/native.h"
#include "corpus.h"
#include "gfx/device.h"
#include "io/dir.h"
#include "io/pairing.h"
#include "io/paths.h"
#include "mediaviewer/mediaviewer.h"

using namespace std::chrono_literals;

namespace {

std::wstring live_temp_dir() {
  wchar_t root[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, root) > 0);
  static std::atomic<unsigned> counter{0};
  std::wstring made;
  for (unsigned attempt = 0; made.empty() && attempt < 512; ++attempt) {
    wchar_t path[MAX_PATH]{};
    std::swprintf(path, MAX_PATH, L"%smvlive%lu_%u", root,
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  counter.fetch_add(1, std::memory_order_relaxed));
    if (::CreateDirectoryW(path, nullptr)) made = path;
    else REQUIRE(::GetLastError() == ERROR_ALREADY_EXISTS);
  }
  REQUIRE_FALSE(made.empty());
  return made;
}

std::string to_utf8(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::filesystem::path test_data_dir() {
  std::filesystem::path dir = std::filesystem::current_path();
  for (int up = 0; up < 8; ++up) {
    const std::filesystem::path candidate = dir / "tests" / "data";
    if (std::filesystem::is_directory(candidate / "heif")) return candidate;
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::wstring& dir, const wchar_t* name,
                 const std::vector<std::uint8_t>& bytes) {
  std::ofstream f(std::filesystem::path(dir) / name, std::ios::binary);
  REQUIRE(f.good());
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> stop_names(const std::wstring& dir) {
  auto listed = mv::io::list_still_files(to_utf8(dir));
  REQUIRE(listed);
  std::vector<std::string> out;
  for (const auto& item : mv::io::pair_listing(std::move(listed).value())) {
    out.push_back(item.primary.name_utf8);
  }
  return out;
}

std::vector<mv::io::listed_item> stops(const std::wstring& dir) {
  auto listed = mv::io::list_still_files(to_utf8(dir));
  REQUIRE(listed);
  return mv::io::pair_listing(std::move(listed).value());
}

bool wait_for(mv_session_t s, mv_completion_kind kind) {
  HANDLE wait = static_cast<HANDLE>(mv_completion_wait_handle(s));
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  mv_completion buf[16]{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (::WaitForSingleObject(wait, 200) == WAIT_OBJECT_0) {
      const uint32_t n = mv_completion_drain(s, buf, 16);
      for (uint32_t i = 0; i < n; ++i) {
        if (buf[i].kind == static_cast<uint32_t>(kind)) return true;
      }
    }
  }
  return false;
}

std::string item_path_string(mv_session_t s, uint32_t index,
                             mv_status(MV_CALL* fn)(mv_session_t, uint32_t, char*, uint32_t,
                                                    uint32_t*)) {
  char buf[1024]{};
  uint32_t bytes = 0;
  REQUIRE(fn(s, index, buf, sizeof(buf), &bytes) == MV_OK);
  return std::string(buf);
}

std::string base_of(const std::string& path) {
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

TEST_CASE("a camera roll as an iPhone writes it comes out as the right stops",
          "[io][pairing][livephoto]") {
  const auto dir = live_temp_dir();
  const auto data = test_data_dir();
  REQUIRE_FALSE(data.empty());
  const auto heic = read_bytes(data / "heif" / "iphone_like.heic");
  REQUIRE(heic.size() > 16);
  // Real HEIC bytes for the stills; the motion halves only have to exist for
  // pairing, which is by name (plan/04). The playable-motion half is the next
  // test.
  const std::vector<std::uint8_t> stub(64, 0);

  // A Live Photo the way iOS writes it.
  write_bytes(dir, L"IMG_0001.HEIC", heic);
  write_bytes(dir, L"IMG_0001.MOV", stub);
  // "Most Compatible" (JPEG capture), with the .AAE edit sidecar beside it.
  write_bytes(dir, L"IMG_0002.JPG", heic);
  write_bytes(dir, L"IMG_0002.MOV", stub);
  write_bytes(dir, L"IMG_0002.AAE", stub);
  // An edited copy. iOS keeps the original and adds IMG_E####; the edited
  // still is its own file and must be its own stop, with the original's pair
  // left intact.
  write_bytes(dir, L"IMG_0003.HEIC", heic);
  write_bytes(dir, L"IMG_0003.MOV", stub);
  write_bytes(dir, L"IMG_E0003.HEIC", heic);
  // A still whose motion half never came down from iCloud.
  write_bytes(dir, L"IMG_0004.HEIC", heic);
  // A plain video, no still: a video stop, not half a pair.
  write_bytes(dir, L"IMG_0005.MOV", stub);
  // An iCloud download collision. Two separate stems, so two stops; the "(1)"
  // pair is a pair in its own right.
  write_bytes(dir, L"IMG_0006.HEIC", heic);
  write_bytes(dir, L"IMG_0006.MOV", stub);
  write_bytes(dir, L"IMG_0006 (1).HEIC", heic);
  write_bytes(dir, L"IMG_0006 (1).MOV", stub);

  const auto listed = stops(dir);
  const auto names = stop_names(dir);
  INFO("stops: " << names.size());
  // 0001, 0002, 0003, E0003, 0004, 0005, 0006, 0006 (1) — eight arrow stops
  // out of fourteen files (the .AAE is never listed at all).
  REQUIRE(listed.size() == 8);

  struct expectation {
    const char* primary;
    const char* secondary;  // "" when the stop is a single file
    mv::io::pair_kind kind;
  };
  // Name order, as list_still_files returns it (CompareStringOrdinal, case
  // insensitive): the space in "IMG_0006 (1)" is below '.', so the iCloud
  // duplicate sits just in front of the file it duplicates.
  const expectation expected[] = {
      {"IMG_0001.HEIC", "IMG_0001.MOV", mv::io::pair_kind::live_photo},
      {"IMG_0002.JPG", "IMG_0002.MOV", mv::io::pair_kind::live_photo},
      {"IMG_0003.HEIC", "IMG_0003.MOV", mv::io::pair_kind::live_photo},
      {"IMG_0004.HEIC", "", mv::io::pair_kind::none},
      {"IMG_0005.MOV", "", mv::io::pair_kind::none},
      {"IMG_0006 (1).HEIC", "IMG_0006 (1).MOV", mv::io::pair_kind::live_photo},
      {"IMG_0006.HEIC", "IMG_0006.MOV", mv::io::pair_kind::live_photo},
      {"IMG_E0003.HEIC", "", mv::io::pair_kind::none},
  };
  for (std::size_t i = 0; i < listed.size(); ++i) {
    INFO("stop " << i << ": " << listed[i].primary.name_utf8);
    CHECK(listed[i].primary.name_utf8 == expected[i].primary);
    CHECK(listed[i].secondary.name_utf8 == expected[i].secondary);
    CHECK(listed[i].kind == expected[i].kind);
  }

  // The .AAE never appears, as a stop or as anyone's secondary.
  for (const auto& item : listed) {
    CHECK(item.primary.name_utf8.find(".AAE") == std::string::npos);
    CHECK(item.secondary.name_utf8.find(".AAE") == std::string::npos);
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);  // the watcher may still hold it; not the test's point
}

TEST_CASE("a hidden motion half is not silently attached to a still",
          "[io][pairing][livephoto]") {
  // iCloud placeholders and some import tools leave the MOV hidden. A hidden
  // file is not listed (plan/04 companion hiding), so the still must stand on
  // its own rather than pairing with a file the user cannot see.
  const auto dir = live_temp_dir();
  const auto data = test_data_dir();
  const auto heic = read_bytes(data / "heif" / "iphone_like.heic");
  write_bytes(dir, L"IMG_0010.HEIC", heic);
  write_bytes(dir, L"IMG_0010.MOV", std::vector<std::uint8_t>(64, 0));
  REQUIRE(::SetFileAttributesW((std::filesystem::path(dir) / L"IMG_0010.MOV").c_str(),
                               FILE_ATTRIBUTE_HIDDEN));

  const auto listed = stops(dir);
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].primary.name_utf8 == "IMG_0010.HEIC");
  CHECK(listed[0].kind == mv::io::pair_kind::none);
  CHECK(listed[0].secondary.path_utf8.empty());

  ::SetFileAttributesW((std::filesystem::path(dir) / L"IMG_0010.MOV").c_str(),
                       FILE_ATTRIBUTE_NORMAL);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);  // the watcher may still hold it; not the test's point
}

TEST_CASE("`;` plays the motion of a Live Photo whose halves are real files",
          "[abi][video][livephoto][integration]") {
  // The motion half is a real clip. av_transport.mp4 is ISO base media, which
  // is what a Live Photo's .MOV is too, and both the probe and FFmpeg go by
  // content rather than extension — so renaming it is a fair stand-in for the
  // container, though NOT for an iPhone's HEVC+PCM motion track. That last
  // step is the human check.
  MV_REQUIRE_CLIP(clip, "av_transport.mp4");
  const auto data = test_data_dir();
  REQUIRE_FALSE(data.empty());

  const auto dir = live_temp_dir();
  write_bytes(dir, L"IMG_0042.HEIC", read_bytes(data / "heif" / "iphone_like.heic"));
  write_bytes(dir, L"IMG_0042.MOV", read_bytes(clip));
  mv::io::set_thumb_cache_dir_override(to_utf8(dir + L"\\thumbs"));
  REQUIRE(::CreateDirectoryW((dir + L"\\thumbs").c_str(), nullptr));

  mv::gfx::device device;
  REQUIRE(device.create(nullptr));
  mv_session_t session = nullptr;
  REQUIRE(mv_session_create(nullptr, &session) == MV_OK);
  struct release { mv_session_t s; ~release() { (void)mv_session_release(s); } } cleanup{session};
  REQUIRE(mv::abi::attach_device(session, device.d3d()) == mv::status::ok);

  uint64_t job = 0;
  REQUIRE(mv_folder_open(session, to_utf8(dir).c_str(), nullptr, &job) == MV_OK);
  REQUIRE(wait_for(session, MV_COMPLETION_FOLDER_READY));

  // One entry, one arrow stop, the still in front.
  uint32_t count = 0;
  REQUIRE(mv_folder_count(session, &count) == MV_OK);
  REQUIRE(count == 1);
  mv_folder_item item{};
  REQUIRE(mv_folder_item_at(session, 0, &item) == MV_OK);
  REQUIRE(item.pair_kind == MV_PAIR_LIVE_PHOTO);
  CHECK(base_of(item_path_string(session, 0, &mv_folder_item_path)) == "IMG_0042.HEIC");
  const std::string motion = item_path_string(session, 0, &mv_folder_item_pair_path);
  REQUIRE(base_of(motion) == "IMG_0042.MOV");

  // What shell/main.cpp's start_motion does with that path.
  REQUIRE(mv_video_open(session, motion.c_str(), &job) == MV_OK);
  mv::player::video_frame frame;
  bool active = false;
  bool shown = false;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline && !shown) {
    shown = mv::abi::poll_video(session, 16'666'667, frame, active);
    std::this_thread::sleep_for(2ms);
  }
  REQUIRE(shown);       // the motion is on the canvas
  REQUIRE(active);
  CHECK(frame.width > 0);

  // ...and stop_motion returns to the still, leaving no clip behind.
  REQUIRE(mv_video_close(session) == MV_OK);
  (void)mv::abi::poll_video(session, 16'666'667, frame, active);
  REQUIRE_FALSE(active);
  REQUIRE(mv_folder_select(session, 0, &job) == MV_OK);

  REQUIRE(mv_folder_close(session) == MV_OK);
  mv::abi::detach_device(session);
  mv::io::set_thumb_cache_dir_override({});
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);  // the watcher may still hold it; not the test's point
}
