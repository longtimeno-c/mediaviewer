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
  settings.wrap =
      ::GetPrivateProfileIntW(kSection, L"wrap", settings.wrap ? 1 : 0, path.c_str()) != 0;
  settings.sticky_zoom =
      ::GetPrivateProfileIntW(kSection, L"sticky_zoom", settings.sticky_zoom ? 1 : 0, path.c_str()) !=
      0;
  const int bg =
      ::GetPrivateProfileIntW(kSection, L"background", settings.background, path.c_str());
  settings.background = static_cast<std::uint8_t>(bg < 0 ? 0 : bg > 3 ? 3 : bg);
  return settings;
}

namespace {

constexpr wchar_t kDestinations[] = L"destinations";

std::wstring wide_from_utf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string utf8_from_wide(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::string_view trim_separator(std::string_view p) noexcept {
  while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.remove_suffix(1);
  return p;
}

bool same_folder(std::string_view a, std::string_view b) noexcept {
  a = trim_separator(a);
  b = trim_separator(b);
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x == '/') x = '\\';
    if (y == '/') y = '\\';
    if (x != y) return false;
  }
  return true;
}

}  // namespace

std::vector<std::string> push_destination(std::vector<std::string> list, std::string_view utf8_dir,
                                          std::size_t max) {
  const std::string_view dir = trim_separator(utf8_dir);
  if (dir.empty() || max == 0) return list;
  std::vector<std::string> out;
  out.reserve(std::min(list.size() + 1, max));
  out.emplace_back(dir);
  for (auto& d : list) {
    if (out.size() >= max) break;
    if (d.empty() || same_folder(d, dir)) continue;
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<std::string> load_destinations() noexcept {
  try {
    std::vector<std::string> out;
    const std::wstring path = settings_path();
    if (path.empty()) return out;
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
      wchar_t key[8]{};
      (void)::swprintf_s(key, L"d%zu", i);
      wchar_t value[1024]{};
      ::GetPrivateProfileStringW(kDestinations, key, L"", value, 1024, path.c_str());
      if (value[0] != L'\0') out.push_back(utf8_from_wide(value));
    }
    return out;
  } catch (...) {
    return {};
  }
}

void save_destinations(const std::vector<std::string>& list) noexcept {
  try {
    const std::wstring path = settings_path();
    if (path.empty()) return;
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
      wchar_t key[8]{};
      (void)::swprintf_s(key, L"d%zu", i);
      const std::wstring value = i < list.size() ? wide_from_utf8(list[i]) : std::wstring{};
      ::WritePrivateProfileStringW(kDestinations, key, value.empty() ? nullptr : value.c_str(),
                                   path.c_str());
    }
  } catch (...) {
  }
}

void save_view_settings(const view_settings& settings) noexcept {
  const std::wstring path = settings_path();
  if (path.empty()) return;
  ::WritePrivateProfileStringW(kSection, L"filmstrip_for_folder",
                               settings.filmstrip_for_folder ? L"1" : L"0", path.c_str());
  ::WritePrivateProfileStringW(kSection, L"filmstrip_for_image",
                               settings.filmstrip_for_image ? L"1" : L"0", path.c_str());
  ::WritePrivateProfileStringW(kSection, L"wrap", settings.wrap ? L"1" : L"0", path.c_str());
  ::WritePrivateProfileStringW(kSection, L"sticky_zoom", settings.sticky_zoom ? L"1" : L"0",
                               path.c_str());
  wchar_t bg[4]{};
  (void)::swprintf_s(bg, L"%u", static_cast<unsigned>(settings.background));
  ::WritePrivateProfileStringW(kSection, L"background", bg, path.c_str());
}

namespace {

constexpr wchar_t kKeys[] = L"keys";

}  // namespace

std::vector<key_override> load_key_overrides() noexcept {
  try {
    std::vector<key_override> out;
    const std::wstring path = settings_path();
    if (path.empty()) return out;
    const int n = ::GetPrivateProfileIntW(kKeys, L"n", 0, path.c_str());
    for (int i = 0; i < n && i < 255; ++i) {
      wchar_t key[16]{};
      (void)::swprintf_s(key, L"r%d", i);
      wchar_t value[64]{};
      ::GetPrivateProfileStringW(kKeys, key, L"", value, 64, path.c_str());
      if (value[0] == L'\0') continue;
      int row = 0, k = 0, mods = 0;
      if (::swscanf_s(value, L"%d,%d,%d", &row, &k, &mods) != 3) continue;
      if (row < 0 || k <= 0 || mods < 0 || mods >= 8) continue;
      out.push_back(key_override{row, static_cast<std::uint16_t>(k),
                                 static_cast<std::uint8_t>(mods)});
    }
    return out;
  } catch (...) {
    return {};
  }
}

void save_key_overrides(std::span<const key_override> list) noexcept {
  try {
    const std::wstring path = settings_path();
    if (path.empty()) return;
    wchar_t nbuf[8]{};
    (void)::swprintf_s(nbuf, L"%zu", list.size());
    ::WritePrivateProfileStringW(kKeys, L"n", nbuf, path.c_str());
    for (int i = 0; i < 255; ++i) {
      wchar_t key[16]{};
      (void)::swprintf_s(key, L"r%d", i);
      if (static_cast<std::size_t>(i) < list.size()) {
        wchar_t value[64]{};
        (void)::swprintf_s(value, L"%d,%u,%u", list[static_cast<std::size_t>(i)].row,
                           static_cast<unsigned>(list[static_cast<std::size_t>(i)].k),
                           static_cast<unsigned>(list[static_cast<std::size_t>(i)].mods));
        ::WritePrivateProfileStringW(kKeys, key, value, path.c_str());
      } else {
        ::WritePrivateProfileStringW(kKeys, key, nullptr, path.c_str());
      }
    }
  } catch (...) {
  }
}

namespace {

constexpr wchar_t kCrash[] = L"crash";

}  // namespace

crash_settings load_crash_settings() noexcept {
  try {
    crash_settings out;
    const std::wstring path = settings_path();
    if (path.empty()) return out;
    const int c = ::GetPrivateProfileIntW(kCrash, L"consent", -1, path.c_str());
    out.consent = c < 0 ? -1 : (c > 0 ? 1 : 0);
    wchar_t url[512]{};
    ::GetPrivateProfileStringW(kCrash, L"upload_url", L"", url, 512, path.c_str());
    out.upload_url = utf8_from_wide(url);
    return out;
  } catch (...) {
    return {};
  }
}

void save_crash_consent(bool accepted) noexcept {
  const std::wstring path = settings_path();
  if (path.empty()) return;
  ::WritePrivateProfileStringW(kCrash, L"consent", accepted ? L"1" : L"0", path.c_str());
}

}  // namespace mv::shell
