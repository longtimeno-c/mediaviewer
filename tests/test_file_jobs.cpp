// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "shell/file_jobs.h"
#include "shell/settings.h"

namespace fs = std::filesystem;
using namespace mv::shell;

namespace {

std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::unique_ptr<file_job_result> wait_for_result(HWND hwnd) {
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  MSG msg{};
  while (::GetTickCount64() < deadline) {
    if (::PeekMessageW(&msg, hwnd, kFileJobDoneMessage, kFileJobDoneMessage, PM_REMOVE)) {
      return std::unique_ptr<file_job_result>(reinterpret_cast<file_job_result*>(msg.lParam));
    }
    ::Sleep(5);
  }
  return nullptr;
}

}  // namespace

TEST_CASE("file jobs run on their worker and post exactly one result", "[shell][files]") {
  const fs::path root = fs::temp_directory_path() /
                        ("mv_file_jobs_" + std::to_string(::GetCurrentProcessId()) + "_" +
                         std::to_string(::GetTickCount64()));
  fs::create_directories(root / "src");
  fs::create_directories(root / "dest");
  { std::ofstream(root / "src" / "one.jpg", std::ios::binary) << "1"; }

  HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                ::GetModuleHandleW(nullptr), nullptr);
  REQUIRE(hwnd != nullptr);

  file_jobs jobs;
  REQUIRE(jobs.start());
  // Refused up front: nothing to do, or no destination for a copy.
  REQUIRE_FALSE(jobs.submit(hwnd, file_job_kind::copy, {}, utf8(root / "dest"), 1));
  REQUIRE_FALSE(jobs.submit(hwnd, file_job_kind::copy, {utf8(root / "src" / "one.jpg")}, "", 1));

  REQUIRE(jobs.submit(hwnd, file_job_kind::copy,
                      {utf8(root / "src" / "one.jpg"), utf8(root / "src" / "missing.jpg")},
                      utf8(root / "dest"), 42));
  auto result = wait_for_result(hwnd);
  REQUIRE(result);
  REQUIRE(result->kind == file_job_kind::copy);
  REQUIRE(result->token == 42);
  REQUIRE(result->items.size() == 2);
  REQUIRE(result->succeeded() == 1);
  REQUIRE(result->failed() == 1);
  REQUIRE(result->refused() == 0);
  REQUIRE(fs::exists(root / "dest" / "one.jpg"));
  REQUIRE(fs::exists(root / "src" / "one.jpg"));  // a copy leaves the original

  REQUIRE(jobs.submit(hwnd, file_job_kind::move, {utf8(root / "src" / "one.jpg")},
                      utf8(root / "dest"), 43));
  result = wait_for_result(hwnd);
  REQUIRE(result);
  REQUIRE(result->succeeded() == 1);
  REQUIRE(fs::path(result->items[0].dest).filename() == "one (2).jpg");
  REQUIRE_FALSE(fs::exists(root / "src" / "one.jpg"));

  jobs.stop();
  ::DestroyWindow(hwnd);
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("recent destinations: newest first, no duplicates, at most five", "[shell][files]") {
  std::vector<std::string> list;
  list = push_destination(list, "D:\\Cull");
  list = push_destination(list, "E:\\Keep\\");
  REQUIRE(list == std::vector<std::string>{"E:\\Keep", "D:\\Cull"});
  list = push_destination(list, "d:/cull");  // same folder, different spelling
  REQUIRE(list.size() == 2);
  REQUIRE(list[0] == "d:/cull");
  for (int i = 0; i < 10; ++i) list = push_destination(list, "F:\\" + std::to_string(i));
  REQUIRE(list.size() == kMaxDestinations);
  REQUIRE(list[0] == "F:\\9");
  REQUIRE(push_destination(list, "").size() == kMaxDestinations);
  REQUIRE(push_destination({}, "C:\\").front() == "C:\\");  // a root keeps its separator
}
