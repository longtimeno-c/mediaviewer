// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// platform.h on Windows. Paths are UTF-8 at the line and wide here.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

#include "addons/agent/platform.h"

namespace mv::agent::platform {
namespace {

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string known_folder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = narrow(p);
  ::CoTaskMemFree(p);
  return out;
}

bool exists(const std::string& p) {
  const DWORD a = ::GetFileAttributesW(wide(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// One argument quoted for CreateProcess's command line (the MSVC rules).
std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : a) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}

}  // namespace

std::string self_executable() {
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0) return {};
  buf.resize(n);
  return narrow(buf);
}

bool run(const std::string& exe, const std::vector<std::string>& args, std::string& output, int& exit_code,
         int timeout_ms) {
  output.clear();
  exit_code = -1;
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return false;
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  std::wstring cmd;
  // A .cmd shim (npm's claude.cmd) runs through cmd.exe.
  const bool script = exe.size() > 4 && (_stricmp(exe.c_str() + exe.size() - 4, ".cmd") == 0 ||
                                         _stricmp(exe.c_str() + exe.size() - 4, ".bat") == 0);
  if (script) cmd = L"cmd.exe /d /s /c \"";
  cmd += quote_arg(wide(exe));
  for (const std::string& a : args) cmd += L" " + quote_arg(wide(a));
  if (script) cmd += L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
  ::CloseHandle(write_end);
  if (!ok) {
    ::CloseHandle(read_end);
    return false;
  }
  const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  char buf[4096];
  for (;;) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr)) break;  // closed: the child is done
    if (avail > 0) {
      DWORD got = 0;
      if (!::ReadFile(read_end, buf, sizeof buf, &got, nullptr) || got == 0) break;
      if (output.size() < 65536) output.append(buf, got);
      continue;
    }
    if (::WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
      // Drain what is left.
      while (::PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD got = 0;
        if (!::ReadFile(read_end, buf, sizeof buf, &got, nullptr) || got == 0) break;
        if (output.size() < 65536) output.append(buf, got);
      }
      break;
    }
    if (::GetTickCount64() > deadline) {
      ::TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  ::WaitForSingleObject(pi.hProcess, 5000);
  DWORD code = 1;
  const bool exited = ::GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE;
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(read_end);
  if (!exited) return false;
  exit_code = static_cast<int>(code);
  return true;
}

std::string module_dir() {
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_dir), &self)) {
    return {};
  }
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  const std::size_t cut = buf.find_last_of(L"\\/");
  return cut == std::wstring::npos ? std::string() : narrow(std::wstring_view(buf).substr(0, cut));
}

std::string claude_skills_dir() { return known_folder(FOLDERID_Profile) + "\\.claude\\skills"; }

std::string find_claude() {
  const std::string profile = known_folder(FOLDERID_Profile);
  const std::string roaming = known_folder(FOLDERID_RoamingAppData);
  for (const std::string& p : {profile + "\\.local\\bin\\claude.exe", profile + "\\.claude\\local\\claude.exe",
                               roaming + "\\npm\\claude.cmd"}) {
    if (exists(p)) return p;
  }
  std::wstring found(MAX_PATH, L'\0');
  const DWORD n = ::SearchPathW(nullptr, L"claude", L".exe", static_cast<DWORD>(found.size()), found.data(), nullptr);
  if (n > 0 && n < found.size()) {
    found.resize(n);
    return narrow(found);
  }
  return {};
}

std::string desktop_config_path() { return known_folder(FOLDERID_RoamingAppData) + "\\Claude\\claude_desktop_config.json"; }

bool desktop_installed() {
  const std::string local = known_folder(FOLDERID_LocalAppData);
  return exists(local + "\\AnthropicClaude\\claude.exe") || exists(local + "\\Programs\\Claude\\Claude.exe");
}

bool open_in_final_cut(const std::string& fcpxml, std::string* why) {
  (void)fcpxml;
  if (why) {
    *why = "Final Cut Pro is Mac-only: import the file into DaVinci Resolve (File > Import > Timeline) or "
           "Premiere Pro (File > Import)";
  }
  return false;
}

bool read_line(std::intptr_t in, std::string& pending, std::string& line) {
  const HANDLE h = reinterpret_cast<HANDLE>(in);
  for (;;) {
    const std::size_t nl = pending.find('\n');
    if (nl != std::string::npos) {
      line.assign(pending, 0, nl);
      pending.erase(0, nl + 1);
      return true;
    }
    if (pending.size() > (64u << 20)) return false;
    char buf[65536];
    DWORD got = 0;
    if (!::ReadFile(h, buf, sizeof buf, &got, nullptr) || got == 0) {
      if (!pending.empty()) {
        line.swap(pending);
        pending.clear();
        return true;
      }
      return false;
    }
    pending.append(buf, got);
  }
}

bool write_all(std::intptr_t out, std::string_view data) {
  const HANDLE h = reinterpret_cast<HANDLE>(out);
  while (!data.empty()) {
    DWORD put = 0;
    const DWORD want = static_cast<DWORD>(data.size() > 1u << 20 ? 1u << 20 : data.size());
    if (!::WriteFile(h, data.data(), want, &put, nullptr) || put == 0) return false;
    data.remove_prefix(put);
  }
  ::FlushFileBuffers(h);
  return true;
}

bool read_file(const std::string& path, std::string& out) {
  const HANDLE h = ::CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.clear();
  char buf[65536];
  DWORD got = 0;
  while (::ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
  ::CloseHandle(h);
  return true;
}

bool write_file_atomic(const std::string& path, const std::string& text) {
  const std::wstring tmp = wide(path + ".mvtmp");
  const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
  ::CloseHandle(h);
  if (!ok || !::MoveFileExW(tmp.c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace mv::agent::platform
