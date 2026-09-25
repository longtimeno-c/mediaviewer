// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/shellext_install.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/trace.h"
#include "shell/os_integration.h"

namespace mv::shell {
namespace {

namespace fs = std::filesystem;

// The shell's thumbnail handler slot (IThumbnailProvider).
constexpr wchar_t kThumbnailShellEx[] = L"{e357fccd-a995-4576-b01f-234630154e96}";

// True when the value was written: it was missing or different.
bool write_reg_string(const std::wstring& subkey, const wchar_t* name, const std::wstring& value,
                      bool& failed) {
  wchar_t buf[2 * MAX_PATH]{};
  DWORD bytes = sizeof(buf);
  if (::RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &bytes) ==
          ERROR_SUCCESS &&
      value == buf) {
    return false;
  }
  const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  if (::RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), name, REG_SZ, value.c_str(), size) !=
      ERROR_SUCCESS) {
    failed = true;
    return false;
  }
  return true;
}

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

fs::path module_dir() {
  wchar_t exe[2 * MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  if (n == 0 || n >= std::size(exe)) return {};
  return fs::path(exe).parent_path();
}

// Copies every listed file from `from` into `to` through a staging folder, so
// a half-copied version is never registered. True when `to` is complete.
bool stage_version(const fs::path& from, const fs::path& to, const std::vector<std::string>& files) {
  std::error_code ec;
  bool complete = fs::is_directory(to, ec);
  for (const std::string& f : files) {
    if (!complete) break;
    complete = fs::is_regular_file(to / widen(f), ec);
  }
  if (complete) return true;
  fs::path staging = to;
  staging += L".partial";
  fs::remove_all(staging, ec);
  if (!fs::create_directories(staging, ec) && ec) return false;
  for (const std::string& f : files) {
    const std::wstring name = widen(f);
    if (!fs::copy_file(from / name, staging / name, fs::copy_options::overwrite_existing, ec)) {
      MV_LOG_WARN("shellext: a handler file could not be staged");
      fs::remove_all(staging, ec);
      return false;
    }
  }
  fs::remove_all(to, ec);  // an incomplete earlier attempt
  fs::rename(staging, to, ec);
  return !ec;
}

}  // namespace

void install_thumbnail_handler(const std::wstring& root, const std::string& version) noexcept {
  try {
    if (root.empty() || version.empty()) return;
    const fs::path current = module_dir();
    if (current.empty()) return;
    std::ifstream list_file(current / L"MediaViewerThumbs.files", std::ios::binary);
    if (!list_file) {
      MV_LOG_WARN("shellext: no MediaViewerThumbs.files beside the app; handler not installed");
      return;
    }
    const std::string text{std::istreambuf_iterator<char>(list_file), std::istreambuf_iterator<char>()};
    const std::vector<std::string> files = parse_shellext_file_list(text);
    if (files.empty() || files.front() != "MediaViewerThumbs.dll") return;

    const fs::path base = fs::path(root) / L"shellext";
    std::vector<std::string> existing;
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec)) existing.push_back(it->path().filename().string());
    }
    const shellext_plan plan = plan_shellext_install(version, existing);
    if (plan.version_dir.empty()) return;
    const fs::path target = base / widen(plan.version_dir);
    if (!stage_version(current, target, files)) return;

    const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + kThumbHandlerClsid;
    const std::wstring dll = (target / L"MediaViewerThumbs.dll").wstring();
    bool failed = false;
    bool changed = false;
    changed |= write_reg_string(clsid_key, nullptr, L"MediaViewer thumbnail handler", failed);
    changed |= write_reg_string(clsid_key, L"AppID", kThumbHandlerAppId, failed);
    changed |= write_reg_string(clsid_key + L"\\InprocServer32", nullptr, dll, failed);
    changed |= write_reg_string(clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment", failed);
    // The handler's own surrogate: an empty DllSurrogate is the system dllhost.
    changed |= write_reg_string(std::wstring(L"Software\\Classes\\AppID\\") + kThumbHandlerAppId,
                                L"DllSurrogate", L"", failed);
    changed |= write_reg_string(std::wstring(L"Software\\Classes\\MediaViewer.Image\\ShellEx\\") +
                                    kThumbnailShellEx,
                                nullptr, kThumbHandlerClsid, failed);
    if (failed) MV_LOG_WARN("shellext: a registry write failed; thumbnails may not show");
    if (changed) ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    // Older versions: removed once no surrogate holds them. A folder that is
    // still in use stays until a later start.
    for (const std::string& old : plan.prune) {
      const fs::path dir = base / widen(old);
      if (dir == target) continue;
      fs::remove_all(dir, ec);
    }
  } catch (...) {
    MV_LOG_WARN("shellext: handler install failed");
  }
}

}  // namespace mv::shell
