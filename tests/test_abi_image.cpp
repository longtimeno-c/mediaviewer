// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.h"
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

std::wstring temp_bmp() {
  wchar_t dir[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, dir) > 0);
  wchar_t path[MAX_PATH]{};
  REQUIRE(::GetTempFileNameW(dir, L"mv", 0, path) != 0);
  const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 255,
                               0, 0, 255, 255, 255, 255, 0, 255};
  auto bytes = fixtures::bmp_rgba(2, 2, rgba);
  std::ofstream f(path, std::ios::binary);
  REQUIRE(f.good());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.close();
  return path;
}

std::string utf8(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

TEST_CASE("mv_image_open returns a job id immediately and completes with size",
          "[abi][image]") {
  session_guard session;
  const auto path = temp_bmp();
  const auto path8 = utf8(path);

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  uint64_t job_id = 0;
  REQUIRE(mv_image_open(session.handle, path8.c_str(), &job_id) == MV_OK);
  REQUIRE(job_id != 0);

  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);
  mv_completion c{};
  REQUIRE(mv_completion_drain(session.handle, &c, 1) == 1);
  REQUIRE(c.kind == MV_COMPLETION_IMAGE_OPENED);
  REQUIRE(c.status == MV_OK);
  REQUIRE(c.job_id == job_id);
  REQUIRE((c.payload >> 32) == 2);
  REQUIRE((c.payload & 0xffffffffll) == 2);

  mv_image_info info{};
  REQUIRE(mv_session_image_info(session.handle, &info) == MV_OK);
  REQUIRE(info.width == 2);
  REQUIRE(info.height == 2);
  REQUIRE(info.format == 3);  // BMP
  REQUIRE(info.transfer_intent == 0);

  ::DeleteFileW(path.c_str());
}

TEST_CASE("a newer open wins over a stale decode", "[abi][image]") {
  session_guard session;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(256) * 256 * 4, 10);
  auto png = fixtures::png_rgba(256, 256, rgba.data());
  REQUIRE_FALSE(png.empty());

  wchar_t dir[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, dir) > 0);
  wchar_t png_path[MAX_PATH]{};
  wchar_t bmp_path[MAX_PATH]{};
  REQUIRE(::GetTempFileNameW(dir, L"mv", 0, png_path) != 0);
  REQUIRE(::GetTempFileNameW(dir, L"mv", 0, bmp_path) != 0);
  {
    std::ofstream f(png_path, std::ios::binary);
    REQUIRE(f.good());
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  }
  {
    const std::uint8_t px[] = {255, 0, 0, 255, 0, 255, 0, 255,
                               0, 0, 255, 255, 255, 255, 0, 255};
    auto bmp = fixtures::bmp_rgba(2, 2, px);
    std::ofstream f(bmp_path, std::ios::binary);
    REQUIRE(f.good());
    f.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
  }

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  uint64_t slow = 0, fast = 0;
  REQUIRE(mv_image_open(session.handle, utf8(png_path).c_str(), &slow) == MV_OK);
  REQUIRE(mv_session_bump_generation(session.handle, nullptr) == MV_OK);
  REQUIRE(mv_image_open(session.handle, utf8(bmp_path).c_str(), &fast) == MV_OK);

  int seen = 0;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (seen < 2 && std::chrono::steady_clock::now() < deadline) {
    (void)::WaitForSingleObject(wait_handle, 1000);
    mv_completion buf[4]{};
    seen += static_cast<int>(mv_completion_drain(session.handle, buf, 4));
  }
  REQUIRE(seen >= 2);

  mv_image_info info{};
  REQUIRE(mv_session_image_info(session.handle, &info) == MV_OK);
  REQUIRE(info.width == 2);
  REQUIRE(info.height == 2);
  REQUIRE(info.format == 3);

  ::DeleteFileW(png_path);
  ::DeleteFileW(bmp_path);
}

TEST_CASE("mv_image_open of missing file completes with IO", "[abi][image]") {
  session_guard session;
  uint64_t job_id = 0;
  // A local missing path. A fictional drive letter can sit in the network
  // redirector for seconds, which is not the I/O error this test is for.
  wchar_t dir[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, dir) > 0);
  const std::wstring missing = std::wstring(dir) + L"mediaviewer-no-such-file-9f3c2e.dat";
  ::DeleteFileW(missing.c_str());
  REQUIRE(mv_image_open(session.handle, utf8(missing).c_str(), &job_id) == MV_OK);
  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);
  mv_completion c{};
  REQUIRE(mv_completion_drain(session.handle, &c, 1) == 1);
  REQUIRE(c.kind == MV_COMPLETION_IMAGE_OPENED);
  REQUIRE(c.status == MV_ERR_IO);
}
