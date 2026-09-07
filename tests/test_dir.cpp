// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "fixtures.h"
#include "io/dir.h"

namespace {

std::wstring temp_dir() {
  wchar_t root[MAX_PATH]{};
  REQUIRE(::GetTempPathW(MAX_PATH, root) > 0);
  wchar_t path[MAX_PATH]{};
  REQUIRE(::GetTempFileNameW(root, L"mvd", 0, path) != 0);
  ::DeleteFileW(path);
  REQUIRE(::CreateDirectoryW(path, nullptr));
  return path;
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
  std::wstring path = dir + L"\\" + name;
  std::ofstream f(path, std::ios::binary);
  REQUIRE(f.good());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("list_still_files returns sorted jpeg/png/bmp names") {
  const auto dir = temp_dir();
  write_bmp(dir, L"b.bmp");
  write_bmp(dir, L"a.bmp");
  {
    std::ofstream skip(dir + L"\\notes.txt");
    skip << "no";
  }

  auto listed = mv::io::list_still_files(utf8(dir));
  REQUIRE(listed);
  REQUIRE(listed->size() == 2);
  REQUIRE(listed.value()[0].name_utf8 == "a.bmp");
  REQUIRE(listed.value()[1].name_utf8 == "b.bmp");

  auto is_dir = mv::io::is_directory(utf8(dir));
  REQUIRE(is_dir);
  REQUIRE(is_dir.value());

  auto parent = mv::io::containing_dir(listed.value()[0].path_utf8);
  REQUIRE(parent);
}
