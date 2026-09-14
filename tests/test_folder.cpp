// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <string>
#include <thread>

#include "fixtures.h"
#include "io/paths.h"
#include "mediaviewer/mediaviewer.h"

using namespace std::chrono_literals;

namespace {

struct session_guard {
  mv_session_t handle = nullptr;
  explicit session_guard(uint32_t workers = 2) {
    mv_session_config config{};
    config.worker_count = workers;
    config.enable_etw = 0;
    REQUIRE(mv_session_create(&config, &handle) == MV_OK);
  }
  ~session_guard() {
    if (handle) mv_session_release(handle);
  }
  session_guard(const session_guard&) = delete;
  session_guard& operator=(const session_guard&) = delete;
};

// GetTempFileNameW derives its name from the clock and only guarantees the
// name is free of a *file*. These tests leave their directories behind, so a
// later run drew a name whose directory (and its thumbs/ child) already
// existed and the CreateDirectoryW assert failed. Keep trying until we own a
// genuinely fresh directory.
std::wstring temp_dir() {
  wchar_t root[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, root) > 0);
  static std::atomic<unsigned> counter{0};
  // No statement after a FAIL: Debug /W4 /WX reports it unreachable (C4702).
  std::wstring made;
  for (unsigned attempt = 0; made.empty() && attempt < 512; ++attempt) {
    wchar_t path[MAX_PATH]{};
    std::swprintf(path, MAX_PATH, L"%smvf%lu_%u", root,
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  counter.fetch_add(1, std::memory_order_relaxed));
    if (::CreateDirectoryW(path, nullptr)) {
      made = path;
    } else {
      REQUIRE(::GetLastError() == ERROR_ALREADY_EXISTS);
    }
  }
  REQUIRE_FALSE(made.empty());  // could not create a unique temp directory
  return made;
}

std::string utf8(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

void write_bmp(const std::wstring& dir, const wchar_t* name) {
  const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 255,
                               0, 0, 255, 255, 255, 255, 0, 255};
  auto bytes = fixtures::bmp_rgba(2, 2, rgba);
  std::ofstream f(dir + L"\\" + name, std::ios::binary);
  REQUIRE(f.good());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

bool wait_kind(mv_session_t s, mv_completion_kind kind, mv_completion* out) {
  HANDLE wait = static_cast<HANDLE>(mv_completion_wait_handle(s));
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  mv_completion buf[16]{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (::WaitForSingleObject(wait, 200) == WAIT_OBJECT_0) {
      const uint32_t n = mv_completion_drain(s, buf, 16);
      for (uint32_t i = 0; i < n; ++i) {
        if (buf[i].kind == static_cast<uint32_t>(kind)) {
          if (out) *out = buf[i];
          return true;
        }
      }
    }
  }
  return false;
}

}  // namespace

TEST_CASE("mv_folder_open lists stills and serves item names", "[abi][folder]") {
  const auto dir = temp_dir();
  write_bmp(dir, L"one.bmp");
  write_bmp(dir, L"two.bmp");
  mv::io::set_thumb_cache_dir_override(utf8(dir + L"\\thumbs"));
  REQUIRE(::CreateDirectoryW((dir + L"\\thumbs").c_str(), nullptr));

  session_guard session;
  uint64_t job = 0;
  REQUIRE(mv_folder_open(session.handle, utf8(dir).c_str(), nullptr, &job) == MV_OK);
  REQUIRE(job != 0);

  mv_completion c{};
  REQUIRE(wait_kind(session.handle, MV_COMPLETION_FOLDER_READY, &c));
  REQUIRE(c.status == MV_OK);
  REQUIRE(c.payload == 2ll);

  uint32_t count = 0;
  REQUIRE(mv_folder_count(session.handle, &count) == MV_OK);
  REQUIRE(count == 2);

  char name[64]{};
  uint32_t bytes = 0;
  REQUIRE(mv_folder_item_name(session.handle, 0, name, sizeof(name), &bytes) == MV_OK);
  REQUIRE(std::string(name) == "one.bmp");

  REQUIRE(mv_folder_close(session.handle) == MV_OK);
  mv::io::set_thumb_cache_dir_override({});
}

namespace {

std::string item_string(mv_session_t s, uint32_t index,
                        mv_status (MV_CALL* fn)(mv_session_t, uint32_t, char*, uint32_t,
                                                uint32_t*)) {
  char buf[1024]{};
  uint32_t bytes = 0;
  REQUIRE(fn(s, index, buf, sizeof(buf), &bytes) == MV_OK);
  return std::string(buf);
}

std::string base_name(const std::string& path) {
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

TEST_CASE("a RAW+JPEG pair and a Live Photo are one stop each over the ABI",
          "[abi][folder][pairing]") {
  // Renamed BMPs: listing and pairing go by name; nothing here needs a RAW or
  // HEIC decoder.
  const auto dir = temp_dir();
  write_bmp(dir, L"DSC_0001.JPG");
  write_bmp(dir, L"DSC_0001.NEF");
  write_bmp(dir, L"DSC_0002.ARW");
  write_bmp(dir, L"IMG_0003.JPG");
  write_bmp(dir, L"IMG_0003.MOV");
  write_bmp(dir, L"other.bmp");
  mv::io::set_thumb_cache_dir_override(utf8(dir + L"\\thumbs"));
  REQUIRE(::CreateDirectoryW((dir + L"\\thumbs").c_str(), nullptr));

  session_guard session;
  uint64_t job = 0;
  // Explorer opened the RAW half: the pair's stop is selected.
  const std::string nef = utf8(dir + L"\\DSC_0001.NEF");
  REQUIRE(mv_folder_open(session.handle, utf8(dir).c_str(), nef.c_str(), &job) == MV_OK);
  mv_completion c{};
  REQUIRE(wait_kind(session.handle, MV_COMPLETION_FOLDER_READY, &c));
  REQUIRE(c.payload == 4ll);  // stops, not files

  uint32_t count = 0;
  REQUIRE(mv_folder_count(session.handle, &count) == MV_OK);
  REQUIRE(count == 4);

  mv_folder_item item{};
  REQUIRE(mv_folder_item_at(session.handle, 0, &item) == MV_OK);
  REQUIRE(item.pair_kind == MV_PAIR_RAW_JPEG);
  REQUIRE((item.flags & 1u) == 1u);  // selected: the NEF asked for landed here
  REQUIRE((item.flags & 2u) == 0u);  // the primary is the JPEG
  REQUIRE(item_string(session.handle, 0, &mv_folder_item_name) == "DSC_0001.JPG");
  REQUIRE(base_name(item_string(session.handle, 0, &mv_folder_item_path)) == "DSC_0001.JPG");
  REQUIRE(base_name(item_string(session.handle, 0, &mv_folder_item_pair_path)) == "DSC_0001.NEF");
  uint32_t selected = 99;
  REQUIRE(mv_folder_selected(session.handle, &selected) == MV_OK);
  REQUIRE(selected == 0);

  REQUIRE(mv_folder_item_at(session.handle, 1, &item) == MV_OK);
  REQUIRE(item.pair_kind == MV_PAIR_NONE);
  REQUIRE((item.flags & 2u) == 2u);  // an unpaired RAW: the badge flag
  REQUIRE(item_string(session.handle, 1, &mv_folder_item_pair_path).empty());

  REQUIRE(mv_folder_item_at(session.handle, 2, &item) == MV_OK);
  REQUIRE(item.pair_kind == MV_PAIR_LIVE_PHOTO);
  REQUIRE(item_string(session.handle, 2, &mv_folder_item_name) == "IMG_0003.JPG");
  REQUIRE(base_name(item_string(session.handle, 2, &mv_folder_item_pair_path)) == "IMG_0003.MOV");

  // Arrow onto the Live Photo, then a file lands in front of it: the watcher
  // refresh keeps the same stop, now one index later.
  REQUIRE(mv_folder_select(session.handle, 2, &job) == MV_OK);
  write_bmp(dir, L"A_new.bmp");
  REQUIRE(wait_kind(session.handle, MV_COMPLETION_FOLDER_CHANGED, &c));
  // A copy can raise more than one notification; wait for the listing to hold it.
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (std::chrono::steady_clock::now() < deadline) {
    REQUIRE(mv_folder_count(session.handle, &count) == MV_OK);
    if (count == 5) break;
    std::this_thread::sleep_for(20ms);
  }
  REQUIRE(count == 5);
  REQUIRE(mv_folder_selected(session.handle, &selected) == MV_OK);
  REQUIRE(selected == 3);
  REQUIRE(item_string(session.handle, selected, &mv_folder_item_name) == "IMG_0003.JPG");

  REQUIRE(mv_folder_close(session.handle) == MV_OK);
  mv::io::set_thumb_cache_dir_override({});
}

TEST_CASE("scrubbing a folder abandons the decodes it passed", "[abi][folder][cancellation]") {
  // The regression: folder decodes were submitted at background_generation, so
  // ctx.cancelled() was always false and nothing a held arrow key queued could
  // ever be abandoned. Forty steps left forty full decodes (plus prefetch) to
  // finish after the key came up — plan/02's "chewing gum".
  const auto dir = temp_dir();
  constexpr int files = 40;
  for (int i = 0; i < files; ++i) {
    wchar_t name[32]{};
    std::swprintf(name, 32, L"img%02d.bmp", i);
    write_bmp(dir, name);
  }
  mv::io::set_thumb_cache_dir_override(utf8(dir + L"\\thumbs"));
  REQUIRE(::CreateDirectoryW((dir + L"\\thumbs").c_str(), nullptr));

  session_guard session;
  uint64_t job = 0;
  REQUIRE(mv_folder_open(session.handle, utf8(dir).c_str(), nullptr, &job) == MV_OK);
  REQUIRE(wait_kind(session.handle, MV_COMPLETION_FOLDER_READY, nullptr));

  mv_job_stats before{};
  REQUIRE(mv_session_job_stats(session.handle, &before) == MV_OK);

  // A held arrow key, as fast as the message loop can deliver it.
  for (uint32_t i = 1; i < files; ++i) {
    uint64_t select_job = 0;
    REQUIRE(mv_folder_select(session.handle, i, &select_job) == MV_OK);
  }

  mv_job_stats after{};
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  bool drained = false;
  while (std::chrono::steady_clock::now() < deadline) {
    REQUIRE(mv_session_job_stats(session.handle, &after) == MV_OK);
    if (after.queue_depth == 0 && after.completed + after.cancelled >= after.submitted) {
      drained = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(drained);

  // Something was thrown away. If the count is unchanged, the decodes went back
  // to being uncancellable background work.
  REQUIRE(after.cancelled > before.cancelled);

  REQUIRE(mv_folder_close(session.handle) == MV_OK);
  mv::io::set_thumb_cache_dir_override({});
}
