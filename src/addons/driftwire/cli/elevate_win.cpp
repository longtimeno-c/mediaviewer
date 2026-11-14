// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: UAC ("runas") and a named pipe only this user can open (elevate.h).
// The pipe is made first (FILE_FLAG_FIRST_PIPE_INSTANCE: nobody else holds its
// name), its DACL this user's SID and the Administrators group, and
// the client's process id must be the one ShellExecuteEx started.
#include "addons/driftwire/cli/elevate.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <sodium.h>

#include <array>
#include <cstdint>
#include <string>

#include "addons/driftwire/host_smb.h"

namespace mv::driftwire::cli {

namespace {

constexpr std::size_t kMaxLine = host_smb::kMaxRequestBytes + 2;

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

bool write_all(HANDLE h, std::string_view s) {
  while (!s.empty()) {
    DWORD n = 0;
    if (!WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr) || n == 0) return false;
    s.remove_prefix(n);
  }
  return true;
}

std::string read_line_h(HANDLE h) {
  std::string out;
  char c = 0;
  while (out.size() < kMaxLine) {
    DWORD n = 0;
    if (!ReadFile(h, &c, 1, &n, nullptr) || n == 0 || c == '\n') break;
    out.push_back(c);
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return out;
}

// "D:P(A;;GA;;;<this user's SID>)(A;;GA;;;BA)": this user, and an elevated
// administrator (UAC may ask for another account's password); the client's
// process id is checked besides.
std::wstring own_sddl() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD n = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &n);
  std::wstring out;
  if (n > 0) {
    std::string buf(n, '\0');
    if (GetTokenInformation(token, TokenUser, buf.data(), n, &n)) {
      LPWSTR sid = nullptr;
      if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
        out = std::wstring(L"D:P(A;;GA;;;") + sid + L")(A;;GA;;;BA)";
        LocalFree(sid);
      }
    }
  }
  CloseHandle(token);
  return out;
}

HANDLE g_helper_pipe = INVALID_HANDLE_VALUE;

}  // namespace

elevated_run run_elevated(const std::string& program, std::string_view request, bool) {
  elevated_run r;
  std::array<std::uint8_t, 16> raw{};
  randombytes_buf(raw.data(), raw.size());
  char hex[33] = {};
  sodium_bin2hex(hex, sizeof hex, raw.data(), raw.size());
  const std::string name = std::string("mediaviewer-smb-") + hex;
  const std::wstring path = L"\\\\.\\pipe\\" + wide(name);
  const std::wstring sddl = own_sddl();
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (sddl.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    r.why = "could not make the channel to the elevated step";
    return r;
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
  HANDLE pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                                 64 * 1024, 0, &sa);
  LocalFree(sd);
  if (pipe == INVALID_HANDLE_VALUE) {
    r.why = "could not make the channel to the elevated step";
    return r;
  }
  const std::wstring exe = wide(program);
  const std::wstring params = L"host smb-apply " + wide(name);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.lpParameters = params.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) {
    const DWORD e = GetLastError();
    CloseHandle(pipe);
    r.declined = e == ERROR_CANCELLED;
    r.why = r.declined ? "the administrator's approval was cancelled" : "the elevated step could not start";
    return r;
  }
  const DWORD started = GetProcessId(sei.hProcess);
  const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
  ULONG client = 0;
  if (connected && GetNamedPipeClientProcessId(pipe, &client) && client == started) {
    std::string line(request);
    line.push_back('\n');
    (void)write_all(pipe, line);
    sodium_memzero(line.data(), line.size());
    r.answer = read_line_h(pipe);
  } else {
    r.why = "the elevated step was not the one started";
  }
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  WaitForSingleObject(sei.hProcess, 60000);
  CloseHandle(sei.hProcess);
  r.started = !r.answer.empty();
  if (!r.started && r.why.empty()) r.why = "the elevated step gave no answer";
  return r;
}

bool helper_read(const std::string& channel, std::string& line) {
  if (channel.rfind("mediaviewer-smb-", 0) != 0 || channel.size() != 16 + 32) return false;
  const std::wstring path = L"\\\\.\\pipe\\" + wide(channel);
  if (!WaitNamedPipeW(path.c_str(), 10000)) return false;
  g_helper_pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (g_helper_pipe == INVALID_HANDLE_VALUE) return false;
  line = read_line_h(g_helper_pipe);
  return !line.empty();
}

bool helper_write(const std::string&, const std::string& line) {
  if (g_helper_pipe == INVALID_HANDLE_VALUE) return false;
  const bool ok = write_all(g_helper_pipe, line + "\n");
  FlushFileBuffers(g_helper_pipe);
  CloseHandle(g_helper_pipe);
  g_helper_pipe = INVALID_HANDLE_VALUE;
  return ok;
}

}  // namespace mv::driftwire::cli
