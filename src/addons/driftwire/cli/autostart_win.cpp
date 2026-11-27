// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// autostart.h on Windows: a Scheduled Task of this user's at log on, made
// with schtasks.exe from an XML file kept in the host's folder (UTF-16, as
// schtasks reads it). A task that runs as its own user at least privilege is
// one an ordinary user may make: no administrator. The window: the program is
// a console one, so the host frees the console it was given when nobody
// started it from a terminal (host_cmd.cpp); a window may show for a moment
// at login.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "addons/driftwire/cli/autostart.h"

namespace mv::driftwire::cli {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  (void)::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  (void)::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::filesystem::path u8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string xml_path(const autostart_place& where) { return where.home + "\\autostart-task.xml"; }

// schtasks.exe ARGS, no window, its output dropped; its exit code (-1: not run).
int schtasks(const std::vector<std::string>& args) {
  wchar_t system[MAX_PATH] = {};
  const UINT n = ::GetSystemDirectoryW(system, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return -1;
  const std::wstring exe = std::wstring(system) + L"\\schtasks.exe";
  std::vector<std::string> argv{narrow(exe)};
  argv.insert(argv.end(), args.begin(), args.end());
  std::wstring line = wide(windows_command_line(argv));
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!::CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
    return -1;
  }
  ::CloseHandle(pi.hThread);
  DWORD code = 1;
  if (::WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
    ::TerminateProcess(pi.hProcess, 1);
  } else {
    (void)::GetExitCodeProcess(pi.hProcess, &code);
  }
  ::CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

// DOMAIN\name of this user, from the environment the logon gave it.
std::string this_user() {
  const wchar_t* dom = _wgetenv(L"USERDOMAIN");  // NOLINT(concurrency-mt-unsafe): read once here
  const wchar_t* name = _wgetenv(L"USERNAME");   // NOLINT(concurrency-mt-unsafe)
  if (!name || !*name) return {};
  return dom && *dom ? narrow(std::wstring(dom) + L"\\" + name) : narrow(name);
}

}  // namespace

autostart_result set_autostart(bool on, const autostart_command& c, const autostart_place& where) {
  autostart_result out;
  if (where.home.empty()) {
    out.note = "no folder to keep the task's description in.";
    return out;
  }
  const std::string path = xml_path(where);
  std::error_code ec;
  if (!on) {
    if (where.apply) {
      (void)schtasks({"/End", "/TN", kAutostartTask});
      (void)schtasks({"/Delete", "/TN", kAutostartTask, "/F"});
    }
    std::filesystem::remove(u8(path), ec);
    out.ok = true;
    out.note = "It no longer starts at login.";
    return out;
  }
  const std::wstring text = wide(scheduled_task_xml(c, this_user()));
  {
    std::ofstream f(u8(path), std::ios::binary | std::ios::trunc);
    const unsigned char bom[2] = {0xFF, 0xFE};  // UTF-16LE
    f.write(reinterpret_cast<const char*>(bom), 2);
    f.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    if (!f.flush()) {
      out.note = "the task's description could not be written.";
      return out;
    }
  }
  if (where.apply) {
    if (schtasks({"/Create", "/TN", kAutostartTask, "/XML", path, "/F"}) != 0) {
      std::filesystem::remove(u8(path), ec);
      out.note = "Task Scheduler did not take the task (schtasks /Create failed).";
      return out;
    }
    (void)schtasks({"/Run", "/TN", kAutostartTask});
  }
  out.ok = true;
  out.note = "It starts when you log in (a Scheduled Task of yours, no administrator), and runs now.";
  return out;
}

bool autostart_is_on(const autostart_place& where) {
  std::error_code ec;
  return !where.home.empty() && std::filesystem::is_regular_file(u8(xml_path(where)), ec);
}

}  // namespace mv::driftwire::cli
