// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// service.h feature_dir on Windows: io::phone_dir's place (io/paths_win.cpp),
// kept so a library set up by the app before the add-on is found where it is.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdlib>
#include <string>

#include "addons/phone/service.h"

namespace mv::phone {

result<std::string> feature_dir() {
  // The tests' own folder (tests/test_phone_addon.cpp), never the person's.
  if (const char* t = std::getenv("MV_PHONE_ACCESS_DIR"); t && t[0]) return std::string(t);
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
    return err(status::io);
  }
  const std::wstring dir = std::wstring(local) + L"\\MediaViewer\\phone-access";
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return err(status::io);
  std::string out(static_cast<std::size_t>(n), '\0');
  if (::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), out.data(), n, nullptr, nullptr) != n) {
    return err(status::io);
  }
  return out;
}

}  // namespace mv::phone
