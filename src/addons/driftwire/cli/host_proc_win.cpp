// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// host_proc.h on Windows: CreateProcessW with an anonymous pipe for stdin and
// no window of its own.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "addons/driftwire/cli/autostart.h"
#include "addons/driftwire/cli/host_proc.h"

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

// An inheritable copy of `h` for the child, or NUL when there is none (a
// console let go): never a handle value the child would not own.
HANDLE for_child(HANDLE h) {
  HANDLE out = nullptr;
  if (h && h != INVALID_HANDLE_VALUE &&
      ::DuplicateHandle(::GetCurrentProcess(), h, ::GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    return out;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  out = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  return out == INVALID_HANDLE_VALUE ? nullptr : out;
}

class win_child final : public child_process {
 public:
  win_child(HANDLE process, DWORD pid, HANDLE to_stdin) : process_(process), pid_(pid), to_stdin_(to_stdin) {}
  ~win_child() override {
    if (!done_) {
      kill();
      (void)wait(5000);
    }
    close_pipe();
    ::CloseHandle(process_);
  }

  bool wait(int ms) override {
    if (done_) return true;
    if (::WaitForSingleObject(process_, static_cast<DWORD>(ms < 0 ? 0 : ms)) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    (void)::GetExitCodeProcess(process_, &code);
    done_ = true;
    // An exception's code (0xC0000005 ...) is a crash, not an exit.
    how_.crashed = (code & 0xC0000000u) == 0xC0000000u;
    how_.code = static_cast<int>(code);
    return true;
  }
  ended how() const override { return how_; }
  long pid() const override { return static_cast<long>(pid_); }
  void ask_to_stop() override { close_pipe(); }
  void kill() override {
    if (!done_) (void)::TerminateProcess(process_, 1);
  }

 private:
  void close_pipe() {
    if (to_stdin_) ::CloseHandle(to_stdin_);
    to_stdin_ = nullptr;
  }
  HANDLE process_;
  DWORD pid_;
  HANDLE to_stdin_;
  bool done_ = false;
  ended how_;
};

}  // namespace

std::unique_ptr<child_process> child_process::start(const std::string& program, const std::vector<std::string>& args) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return nullptr;
  (void)::SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);  // only the read end goes to the child
  std::vector<std::string> argv{program};
  argv.insert(argv.end(), args.begin(), args.end());
  const std::wstring exe = wide(program);
  std::wstring line = wide(windows_command_line(argv));
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = read_end;
  HANDLE out = for_child(::GetStdHandle(STD_OUTPUT_HANDLE));
  HANDLE err = for_child(::GetStdHandle(STD_ERROR_HANDLE));
  si.StartupInfo.hStdOutput = out;
  si.StartupInfo.hStdError = err;
  // Only these are inherited, never another handle of the supervisor's (its lock).
  HANDLE inherit[3] = {read_end, out, err};
  DWORD count = 1;
  if (out) inherit[count++] = out;
  if (err && err != out) inherit[count++] = err;
  SIZE_T size = 0;
  (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::string attrs(size, '\0');
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
  bool listed = ::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) != 0;
  listed = listed && ::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                 count * sizeof(HANDLE), nullptr, nullptr) != 0;
  PROCESS_INFORMATION pi{};
  const BOOL made = listed && ::CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                               CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                               &si.StartupInfo, &pi);
  if (listed) ::DeleteProcThreadAttributeList(si.lpAttributeList);
  ::CloseHandle(read_end);
  if (out) ::CloseHandle(out);
  if (err) ::CloseHandle(err);
  if (!made) {
    ::CloseHandle(write_end);
    return nullptr;
  }
  ::CloseHandle(pi.hThread);
  return std::make_unique<win_child>(pi.hProcess, pi.dwProcessId, write_end);
}

void wait_for_stdin_end() {
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  if (!in || in == INVALID_HANDLE_VALUE) return;
  char buf[256];
  for (;;) {
    DWORD n = 0;
    if (!::ReadFile(in, buf, sizeof buf, &n, nullptr) || n == 0) return;  // broken pipe: the supervisor is gone
  }
}

void leave_own_console() {
  DWORD ids[2] = {};
  // Only this process on its console: it was made for us (no terminal started us).
  if (::GetConsoleProcessList(ids, 2) == 1) (void)::FreeConsole();
}

}  // namespace mv::driftwire::cli
