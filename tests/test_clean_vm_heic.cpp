// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 7 verify, first clause: "iPhone HEIC opens on a clean VM with no Store
// packs."
//
// A person still has to open one on a clean VM once. What is testable here is
// the thing that would make that person's pass a lie: a HEIC that decoded
// through a Store-provided HEVC codec on THIS machine while the test believed
// it was exercising the bundled path. MV_OS_CODEC=0 is meant to stand in for a
// clean VM, and nothing but this file has ever checked that it does.
//
// So: decode a HEIC with MV_OS_CODEC=0 and then look at what the process has
// loaded. libheif and libde265 must be in, because that is who did the work
// (they are dynamic-link by licence — plan/11 — so they are visible as
// modules). Media Foundation, WIC's codec extensions, and anything out of
// C:\Program Files\WindowsApps must be absent: those are exactly what a clean
// VM does not have, and any one of them appearing means the decode had help
// this repo does not ship.
//
// WHY ITS OWN EXECUTABLE. The assertion is about the whole process, and it is
// one-way: once mv_tests has opened a clip through FFmpeg or probed for an HEVC
// MFT in some earlier case, mfplat.dll stays loaded for the rest of the run and
// the check can never be made again. A dedicated process has loaded nothing
// else, so "absent" means absent.
#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "codec/os_decode.h"

using namespace mv::codec;

namespace {

std::wstring lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; });
  return s;
}

// Every module currently mapped into this process, as a lower-case full path.
std::vector<std::wstring> loaded_modules() {
  std::vector<HMODULE> handles(512);
  DWORD needed = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (!::EnumProcessModules(::GetCurrentProcess(), handles.data(),
                              static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) {
      return {};
    }
    if (needed <= handles.size() * sizeof(HMODULE)) break;
    handles.resize(needed / sizeof(HMODULE) + 32);
  }
  handles.resize(needed / sizeof(HMODULE));
  std::vector<std::wstring> out;
  out.reserve(handles.size());
  for (HMODULE h : handles) {
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameExW(::GetCurrentProcess(), h, path, MAX_PATH) > 0) {
      out.push_back(lower(path));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// For failure messages only: these are module paths and needles, all ASCII.
std::string narrow(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

bool any_contains(const std::vector<std::wstring>& modules, const wchar_t* needle) {
  return std::any_of(modules.begin(), modules.end(),
                     [needle](const std::wstring& m) { return m.find(needle) != std::wstring::npos; });
}

// What a clean VM does not have, and what the bundled path must therefore not
// need. `\windowsapps\` covers every Store extension by location rather than by
// a name list that would go stale; the rest are the Media Foundation and WIC
// pieces the OS-codec path in codec/os_decode_win.cpp reaches for.
constexpr const wchar_t* kMustNotLoad[] = {
    L"\\windowsapps\\",      // any Store codec extension, HEVC or HEIF
    L"\\mfplat.dll",         // the MFTEnumEx probe
    L"\\mfreadwrite.dll",
    L"\\mfcore.dll",
    L"\\msmpeg2vdec.dll",    // the in-box HEVC/MPEG-2 decoder MFT
    L"\\windowscodecsext.dll",
};

// What must be there, because it is what decoded the file.
constexpr const wchar_t* kMustLoad[] = {L"heif", L"de265"};

std::filesystem::path data_dir() { return std::filesystem::path(MV_TEST_DATA_DIR); }

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void check_no_os_codec(const std::vector<std::wstring>& modules) {
  for (const wchar_t* banned : kMustNotLoad) {
    const bool present = any_contains(modules, banned);
    if (present) {
      // Name the module in the failure: "something loaded" is not actionable.
      for (const auto& m : modules) {
        if (m.find(banned) != std::wstring::npos) {
          UNSCOPED_INFO("OS codec module loaded: " << narrow(m));
        }
      }
    }
    CHECK_FALSE(present);
  }
}

}  // namespace

TEST_CASE("MV_OS_CODEC=0 does not even probe for an OS codec", "[codec][heif][cleanvm]") {
  // The probe itself loads mfplat.dll. If the disabled path reached it, the
  // "bundled" decode below would be running on a machine whose module list no
  // longer resembles a clean VM.
  _putenv_s("MV_OS_CODEC", "0");
  REQUIRE_FALSE(os_codec_enabled());

  const auto before = loaded_modules();
  check_no_os_codec(before);

  const auto bytes = read_file(data_dir() / "heif" / "iphone_like.heic");
  REQUIRE(bytes.size() > 16);
  auto os = try_os_decode(bytes);
  REQUIRE_FALSE(os);
  CHECK(os.error() == mv::status::unsupported_format);

  const auto after = loaded_modules();
  check_no_os_codec(after);
  CHECK(before == after);  // the disabled probe loaded nothing at all
}

TEST_CASE("a HEIC decodes with nothing a clean VM lacks", "[codec][heif][cleanvm]") {
  _putenv_s("MV_OS_CODEC", "0");
  const auto bytes = read_file(data_dir() / "heif" / "iphone_like.heic");
  REQUIRE(bytes.size() > 16);
  REQUIRE(probe(bytes) == format_family::heic);

  auto r = decode(bytes);
  REQUIRE(r);
  const raster& img = r.value();
  CHECK(img.width == 48);  // irot applied by libheif: 64x48 stored
  CHECK(img.height == 64);
  REQUIRE(img.rgba.size() == static_cast<std::size_t>(img.width) * img.height * 4);

  const auto modules = loaded_modules();
  check_no_os_codec(modules);
  for (const wchar_t* required : kMustLoad) {
    const bool present = any_contains(modules, required);
    UNSCOPED_INFO("expected a bundled decoder module matching: " << narrow(required));
    CHECK(present);
  }
}

TEST_CASE("the fetched real-world HEIC decodes the same way", "[codec][heif][cleanvm]") {
  // tools/testmedia/fetch-heif.ps1 downloads it; absent, this case says so
  // rather than passing quietly (plan/09, tests/corpus.h).
  const std::filesystem::path path =
      std::filesystem::path(MV_TESTMEDIA_DIR) / "heif" / "libheif-example.heic";
  if (!std::filesystem::exists(path)) {
    if (std::getenv("MV_REQUIRE_CORPUS")) {
      FAIL("MV_REQUIRE_CORPUS=1 and " << path.string()
                                      << " is missing — run tools/testmedia/fetch-heif.ps1");
    }
    SKIP("SKIPPED: " + path.string() + " (run tools/testmedia/fetch-heif.ps1)");
  }
  _putenv_s("MV_OS_CODEC", "0");
  const auto bytes = read_file(path);
  REQUIRE(bytes.size() > 16);
  auto r = decode(bytes);
  REQUIRE(r);
  CHECK(r.value().width > 0);
  check_no_os_codec(loaded_modules());
}
