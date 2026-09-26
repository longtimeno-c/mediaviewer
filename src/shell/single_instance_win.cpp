// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/single_instance_win.h"

#include <sddl.h>

#include <new>

#include "core/trace.h"

namespace mv::shell {
namespace {

// A hand-off is a few paths; anything longer than this is not ours.
constexpr DWORD kMaxMessageBytes = 256 * 1024;
// How long a second start waits for a busy pipe: the first instance may still
// be creating its window, and several starts queue on one pipe instance.
constexpr DWORD kClientWaitMs = 10000;
constexpr DWORD kReadWaitMs = 2000;

std::wstring pipe_name() {
  std::wstring sid_text;
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD bytes = 0;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buf(bytes);
    if (bytes > 0 && ::GetTokenInformation(token, TokenUser, buf.data(), bytes, &bytes)) {
      LPWSTR text = nullptr;
      if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
        sid_text = text;
        ::LocalFree(text);
      }
    }
    ::CloseHandle(token);
  }
  DWORD session = 0;
  (void)::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
  return L"\\\\.\\pipe\\MediaViewer.Viewer." + std::to_wstring(session) + L"." + sid_text;
}

HANDLE create_pipe(const std::wstring& name, bool first) {
  return ::CreateNamedPipeW(name.c_str(),
                            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                                (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            1, 0, kMaxMessageBytes, 0, nullptr);
}

std::wstring absolute(const std::wstring& path) {
  const DWORD n = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (n == 0) return path;
  std::wstring out(n, L'\0');
  const DWORD got = ::GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  if (got == 0 || got >= n) return path;
  out.resize(got);
  return out;
}

// Waits for an overlapped operation or the stop event. True when it completed.
bool wait_io(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeout, DWORD& bytes) {
  const HANDLE events[] = {ov.hEvent, stop};
  const DWORD w = ::WaitForMultipleObjects(2, events, FALSE, timeout);
  if (w != WAIT_OBJECT_0) {
    ::CancelIoEx(pipe, &ov);
    (void)::GetOverlappedResult(pipe, &ov, &bytes, TRUE);
    return false;
  }
  return ::GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

}  // namespace

bool forward_to_running_instance(const std::vector<std::wstring>& paths) noexcept {
  try {
    const std::wstring name = pipe_name();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const ULONGLONG give_up = ::GetTickCount64() + kClientWaitMs;
    for (;;) {
      pipe = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (pipe != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_PIPE_BUSY) break;
      // Another start holds the one instance; the owner frees it once read.
      const ULONGLONG now = ::GetTickCount64();
      if (now >= give_up) break;
      (void)::WaitNamedPipeW(name.c_str(), static_cast<DWORD>(give_up - now));
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;  // nobody is listening: be the first
    std::wstring message;
    for (const std::wstring& p : paths) {
      if (p.empty()) continue;
      if (!message.empty()) message.push_back(L'\n');
      message += absolute(p);
    }
    // The running instance may bring its window forward: this process has
    // the foreground right now (the user just opened something), it does not.
    ULONG server = 0;
    if (::GetNamedPipeServerProcessId(pipe, &server)) (void)::AllowSetForegroundWindow(server);
    const DWORD bytes = static_cast<DWORD>(message.size() * sizeof(wchar_t));
    DWORD written = 0;
    // An empty message is still a message: "come to the front".
    const BOOL ok = bytes <= kMaxMessageBytes &&
                    ::WriteFile(pipe, message.empty() ? L"" : message.data(), bytes, &written, nullptr) &&
                    written == bytes;
    ::CloseHandle(pipe);
    return ok != FALSE;
  } catch (...) {
    return false;
  }
}

instance_claim::~instance_claim() {
  if (pipe_ != INVALID_HANDLE_VALUE) ::CloseHandle(pipe_);
}

bool instance_claim::claim() noexcept {
  if (claimed()) return true;
  try {
    pipe_ = create_pipe(pipe_name(), true);
  } catch (...) {
    pipe_ = INVALID_HANDLE_VALUE;
  }
  return claimed();  // INVALID_HANDLE_VALUE: another instance owns the name
}

bool instance_listener::start(HWND window, UINT message, instance_claim& claim) noexcept {
  if (thread_.joinable() || !window || !claim.claimed()) return false;
  try {
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) return false;
    window_ = window;
    message_ = message;
    HANDLE first = claim.pipe_;
    thread_ = std::thread([this, first] { run(first); });
    claim.pipe_ = INVALID_HANDLE_VALUE;  // the listener owns it now
    return true;
  } catch (...) {
    return false;
  }
}

void instance_listener::stop() noexcept {
  if (stop_event_) ::SetEvent(stop_event_);
  if (thread_.joinable()) thread_.join();
  if (stop_event_) ::CloseHandle(stop_event_);
  stop_event_ = nullptr;
}

void instance_listener::run(HANDLE pipe) noexcept {
  const std::wstring name = pipe_name();
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) {
    ::CloseHandle(pipe);
    return;
  }
  std::vector<wchar_t> buf(kMaxMessageBytes / sizeof(wchar_t));
  while (pipe != INVALID_HANDLE_VALUE) {
    ::ResetEvent(ov.hEvent);
    DWORD bytes = 0;
    bool connected = ::ConnectNamedPipe(pipe, &ov) != FALSE;
    if (!connected) {
      const DWORD e = ::GetLastError();
      // ERROR_NO_DATA: a start connected, wrote and closed before this
      // listener existed; its message is still in the buffer to read.
      if (e == ERROR_PIPE_CONNECTED || e == ERROR_NO_DATA) connected = true;
      else if (e == ERROR_IO_PENDING) connected = wait_io(pipe, ov, stop_event_, INFINITE, bytes);
    }
    if (::WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
    if (connected) {
      ::ResetEvent(ov.hEvent);
      bytes = 0;
      bool read = ::ReadFile(pipe, buf.data(), kMaxMessageBytes, &bytes, &ov) != FALSE;
      if (!read && ::GetLastError() == ERROR_IO_PENDING) {
        read = wait_io(pipe, ov, stop_event_, kReadWaitMs, bytes);
      }
      // A message too long for the buffer (ERROR_MORE_DATA) is not ours: dropped.
      if (read && bytes % sizeof(wchar_t) == 0) {
        auto* paths = new (std::nothrow) std::wstring(buf.data(), bytes / sizeof(wchar_t));
        if (paths && !::PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(paths))) delete paths;
      }
      ::DisconnectNamedPipe(pipe);
    } else {
      // A broken instance: make a fresh one (not "first": this process owns the name).
      ::CloseHandle(pipe);
      pipe = create_pipe(name, false);
    }
  }
  if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
  ::CloseHandle(ov.hEvent);
}

}  // namespace mv::shell
