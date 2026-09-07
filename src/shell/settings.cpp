// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <string>

namespace mv::shell {
namespace {

constexpr wchar_t kSection[] = L"view";

// Empty on failure. Callers treat that as "no persistence this run" rather
// than an error the user has to see — a viewer that will not start because
// %LocalAppData% is unwritable is worse than one that forgets a toggle.
std::wstring settings_path() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                                local))) {
    return {};
  }
  std::wstring dir = local;
  dir += L"\\MediaViewer";
  if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
    return {};
  }
  return dir + L"\\settings.ini";
}

}  // namespace

view_settings load_view_settings() noexcept {
  view_settings settings;
  const std::wstring path = settings_path();
  if (path.empty()) return settings;
  settings.filmstrip_for_folder =
      ::GetPrivateProfileIntW(kSection, L"filmstrip_for_folder",
                              settings.filmstrip_for_folder ? 1 : 0, path.c_str()) != 0;
  settings.filmstrip_for_image =
      ::GetPrivateProfileIntW(kSection, L"filmstrip_for_image",
                              settings.filmstrip_for_image ? 1 : 0, path.c_str()) != 0;
  return settings;
}

void save_view_settings(const view_settings& settings) noexcept {
  const std::wstring path = settings_path();
  if (path.empty()) return;
  ::WritePrivateProfileStringW(kSection, L"filmstrip_for_folder",
                               settings.filmstrip_for_folder ? L"1" : L"0", path.c_str());
  ::WritePrivateProfileStringW(kSection, L"filmstrip_for_image",
                               settings.filmstrip_for_image ? L"1" : L"0", path.c_str());
}

}  // namespace mv::shell
