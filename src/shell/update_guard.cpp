// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/update_guard.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "core/trace.h"

namespace mv::shell::update {

bool is_velopack_hook(std::wstring_view a) noexcept {
  return a.starts_with(L"--veloapp-") || a.starts_with(L"--squirrel-");
}

start_decision decide_start(const trial_record& trial, std::string_view running) noexcept {
  if (trial.version.empty()) return {start_action::normal, 0};
  // Update.exe never applied it (still on the prior version, or rolled back
  // already): the record is stale.
  if (running.empty() || trial.version != running) return {start_action::abandon, 0};
  if (trial.attempts >= kMaxFailedStarts) {
    return {trial.prior_package.empty() ? start_action::abandon : start_action::rollback,
            trial.attempts};
  }
  return {start_action::counted, trial.attempts < 0 ? 1 : trial.attempts + 1};
}

std::string parse_manifest_version(std::string_view xml) {
  constexpr std::string_view open = "<version>";
  constexpr std::string_view close = "</version>";
  const auto a = xml.find(open);
  if (a == std::string_view::npos) return {};
  const auto b = xml.find(close, a + open.size());
  if (b == std::string_view::npos) return {};
  std::string_view v = xml.substr(a + open.size(), b - a - open.size());
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n'))
    v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
    v.remove_suffix(1);
  for (const char c : v) {
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' ||
          c == '-' || c == '+')) {
      return {};
    }
  }
  return std::string(v);
}

std::string append_version_list(std::string_view list, std::string_view version) {
  if (version.empty()) return std::string(list);
  std::string_view rest = list;
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    if (rest.substr(0, comma) == version) return std::string(list);
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  std::string out(list);
  if (!out.empty()) out += ',';
  out += version;
  return out;
}

std::vector<std::wstring> restart_arguments(const view_restore& view) {
  std::vector<std::wstring> args;
  if (view.zoom_percent > 0 && view.zoom_percent <= 6400) {
    args.emplace_back(L"--restore-zoom");
    args.emplace_back(std::to_wstring(view.zoom_percent));
  }
  if (view.fullscreen) args.emplace_back(L"--restore-fullscreen");
  if (view.gallery) args.emplace_back(L"--restore-gallery");
  // A path that starts with '-' would parse as an option; the host takes
  // positional paths only, so such a path is not restored.
  if (!view.path.empty() && view.path.front() != L'-') args.push_back(view.path);
  return args;
}

std::wstring join_arguments(const std::vector<std::wstring>& args) {
  std::wstring blob;
  for (const auto& a : args) {
    blob += a;
    blob.push_back(L'\0');
  }
  return blob;
}

// ---- Win32 -----------------------------------------------------------------

namespace {

constexpr wchar_t kTrial[] = L"trial";
constexpr wchar_t kFailed[] = L"failed";

std::wstring parent_dir(std::wstring_view p) {
  const auto slash = p.find_last_of(L"\\/");
  return slash == std::wstring_view::npos ? std::wstring{} : std::wstring(p.substr(0, slash));
}

bool file_exists(const std::wstring& p) noexcept {
  const DWORD attr = ::GetFileAttributesW(p.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string read_small_file(const std::wstring& p) {
  HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::string out(16 * 1024, '\0');
  DWORD n = 0;
  const BOOL ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
  ::CloseHandle(h);
  if (!ok) return {};
  out.resize(n);
  return out;
}

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                          nullptr);
  }
  return out;
}

std::string read_key(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
  wchar_t buf[512]{};
  ::GetPrivateProfileStringW(section, key, L"", buf, 512, file.c_str());
  return narrow(buf);
}

void write_key(const std::wstring& file, const wchar_t* section, const wchar_t* key,
               const std::string& value) noexcept {
  try {
    const std::wstring w = wide(value);
    ::WritePrivateProfileStringW(section, key, value.empty() ? nullptr : w.c_str(), file.c_str());
  } catch (...) {
  }
}

// The package name came from our own record, but it is still a path segment:
// refuse anything that could leave the rollback folder.
bool safe_file_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name) {
    if (c == '\\' || c == '/' || c == ':') return false;
  }
  return name.ends_with(".nupkg");
}

}  // namespace

install_layout locate_install() noexcept {
  try {
    wchar_t exe[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) return {};
    const std::wstring current = parent_dir(exe);
    const std::wstring root = parent_dir(current);
    if (current.empty() || root.empty()) return {};
    install_layout out;
    out.update_exe = root + L"\\Update.exe";
    if (!file_exists(out.update_exe)) return {};
    out.version = parse_manifest_version(read_small_file(current + L"\\sq.version"));
    if (out.version.empty()) return {};
    out.root = root;
    return out;
  } catch (...) {
    return {};
  }
}

std::wstring trial_file(const install_layout& layout) {
  return layout.root + L"\\updater\\trial.ini";
}

trial_record load_trial(const install_layout& layout) noexcept {
  trial_record t;
  if (!layout.installed()) return t;
  try {
    const std::wstring file = trial_file(layout);
    if (!file_exists(file)) return t;
    t.version = read_key(file, kTrial, L"version");
    t.prior_version = read_key(file, kTrial, L"prior_version");
    t.prior_package = read_key(file, kTrial, L"prior_package");
    t.attempts = static_cast<int>(::GetPrivateProfileIntW(kTrial, L"attempts", 0, file.c_str()));
  } catch (...) {
    return {};
  }
  return t;
}

namespace {

void clear_trial(const std::wstring& file) noexcept {
  ::WritePrivateProfileStringW(kTrial, nullptr, nullptr, file.c_str());
}

bool launch_rollback(const install_layout& layout, const trial_record& trial) noexcept {
  try {
    if (!safe_file_name(trial.prior_package)) return false;
    const std::wstring package = layout.root + L"\\updater\\rollback\\" + wide(trial.prior_package);
    if (!file_exists(package)) return false;
    // Same command UpdateManager.WaitExitThenApplyUpdates builds, with the kept
    // prior package. Update.exe applies whatever package it is given (no
    // version gate), waits for this pid, swaps current\, then starts the app.
    std::wstring cmd = L"\"" + layout.update_exe + L"\" --silent apply --package \"" + package +
                       L"\" --waitPid " + std::to_wstring(::GetCurrentProcessId()) +
                       L" --rootDir \"" + layout.root + L"\" --packageDir \"" + layout.root +
                       L"\\packages\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(layout.update_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, layout.root.c_str(), &si, &pi)) {
      return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

bool run_start_guard(const install_layout& layout) noexcept {
  if (!layout.installed()) {
    MV_LOG_INFO("updater: not a Velopack install; start guard inert");
    return false;
  }
  try {
    const trial_record trial = load_trial(layout);
    const start_decision d = decide_start(trial, layout.version);
    const std::wstring file = trial_file(layout);
    switch (d.action) {
      case start_action::normal:
        return false;
      case start_action::counted:
        write_key(file, kTrial, L"attempts", std::to_string(d.attempts));
        MV_LOG_INFO("updater: start %d of new version %s", d.attempts, layout.version.c_str());
        return false;
      case start_action::abandon:
        if (trial.version == layout.version && trial.attempts >= kMaxFailedStarts) {
          // Failed twice and nothing kept to go back to: say so, keep trying.
          write_key(file, kFailed, L"versions",
                    append_version_list(read_key(file, kFailed, L"versions"), trial.version));
          MV_LOG_WARN("updater: %s failed to start twice; no prior package to roll back to",
                      trial.version.c_str());
        }
        clear_trial(file);
        return false;
      case start_action::rollback: {
        // Record first: if the rollback itself dies, the next start must not
        // loop back into it.
        write_key(file, kFailed, L"versions",
                  append_version_list(read_key(file, kFailed, L"versions"), trial.version));
        write_key(file, kFailed, L"unreported", trial.version);
        clear_trial(file);
        if (!launch_rollback(layout, trial)) {
          MV_LOG_WARN("updater: rollback from %s could not start Update.exe", trial.version.c_str());
          return false;
        }
        MV_LOG_WARN("updater: %s failed to start %d times; rolling back to %s",
                    trial.version.c_str(), trial.attempts, trial.prior_version.c_str());
        return true;
      }
    }
  } catch (...) {
  }
  return false;
}

void confirm_started(const install_layout& layout) noexcept {
  if (!layout.installed()) return;
  try {
    const std::wstring file = trial_file(layout);
    if (!file_exists(file)) return;
    if (read_key(file, kTrial, L"version") != layout.version) return;
    clear_trial(file);
    MV_LOG_INFO("updater: version %s confirmed started", layout.version.c_str());
  } catch (...) {
  }
}

namespace {

[[nodiscard]] bool iequals(std::wstring_view a, std::wstring_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (::towlower(a[i]) != ::towlower(b[i])) return false;
  }
  return true;
}

constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MediaViewer";

}  // namespace

bool is_velopack_uninstall_string(std::wstring_view uninstall_string,
                                  std::wstring_view update_exe) noexcept {
  if (uninstall_string.empty() || update_exe.empty()) return false;
  // Velopack writes it quoted: "<root>\Update.exe" --uninstall
  std::wstring_view s = uninstall_string;
  if (s.front() == L'"') {
    s.remove_prefix(1);
    const std::size_t close = s.find(L'"');
    if (close == std::wstring_view::npos) return false;
    s = s.substr(0, close);
  } else {
    const std::size_t space = s.find(L' ');
    if (space != std::wstring_view::npos) s = s.substr(0, space);
  }
  return iequals(s, update_exe);
}

bool remove_velopack_uninstall_entry(const install_layout& layout) noexcept {
  if (!layout.installed() || layout.update_exe.empty()) return false;
  try {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
      return false;
    }
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS read = ::RegQueryValueExW(key, L"UninstallString", nullptr, &type,
                                            reinterpret_cast<BYTE*>(value), &bytes);
    ::RegCloseKey(key);
    if (read != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    if (!is_velopack_uninstall_string(value, layout.update_exe)) return false;

    if (::RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey) != ERROR_SUCCESS) {
      MV_LOG_WARN("updater: could not remove Velopack's duplicate uninstall entry");
      return false;
    }
    MV_LOG_INFO("updater: removed Velopack's duplicate Apps & features entry");
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace mv::shell::update
