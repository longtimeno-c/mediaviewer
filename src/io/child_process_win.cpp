// SPDX-License-Identifier: GPL-2.0-or-later
// child_process on Windows: CreateProcessW with two anonymous pipes, no
// console window, and a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so
// a helper cannot outlive the viewer (a crash of the app kills its encode).
#include "io/child_process.h"

#include <windows.h>

#include <mutex>
#include <vector>

namespace mv::io {

struct child_process::impl {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE in_write = nullptr;  // our end of the child's stdin
  HANDLE out_read = nullptr;  // our end of the child's stdout
  bool reaped = false;
  int exit_code = -1;
  bool killed = false;
  std::string pending;
  std::mutex write_mu;
};

namespace {

void close_handle(HANDLE& h) noexcept {
  if (h != nullptr && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  h = nullptr;
}

std::wstring wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

// CommandLineToArgvW's quoting rules, so any path survives the round trip.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
  if (!cmd.empty()) cmd.push_back(L' ');
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd.push_back(L'"');
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      cmd.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      cmd.append(backslashes * 2 + 1, L'\\');
      cmd.push_back(L'"');
    } else {
      cmd.append(backslashes, L'\\');
      cmd.push_back(arg[i]);
    }
  }
  cmd.push_back(L'"');
}

}  // namespace

child_process::child_process() : impl_(std::make_unique<impl>()) {}

child_process::~child_process() {
  if (impl_->process != nullptr && !impl_->reaped) {
    kill();
    (void)wait();
  }
  close_handle(impl_->in_write);
  close_handle(impl_->out_read);
  close_handle(impl_->process);
  close_handle(impl_->job);
}

expected child_process::start(std::string_view exe_utf8, std::span<const std::string> args) {
  if (impl_->process != nullptr) return err(status::internal);
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
  if (!::CreatePipe(&in_read, &in_write, &sa, 0)) return err(status::io);
  if (!::CreatePipe(&out_read, &out_write, &sa, 0)) {
    ::CloseHandle(in_read);
    ::CloseHandle(in_write);
    return err(status::io);
  }
  // Only the child's ends are inherited.
  ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

  const std::wstring exe = wide(exe_utf8);
  std::wstring cmd;
  append_quoted(cmd, exe);
  for (const std::string& a : args) append_quoted(cmd, wide(a));

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
  ::CloseHandle(in_read);
  ::CloseHandle(out_write);
  if (!ok) {
    ::CloseHandle(in_write);
    ::CloseHandle(out_read);
    return err(status::io);
  }
  // In the job before it runs a single instruction.
  impl_->job = ::CreateJobObjectW(nullptr, nullptr);
  if (impl_->job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    ::SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    ::AssignProcessToJobObject(impl_->job, pi.hProcess);
  }
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  impl_->process = pi.hProcess;
  impl_->in_write = in_write;
  impl_->out_read = out_read;
  impl_->reaped = false;
  return {};
}

bool child_process::write_line(std::string_view line) noexcept {
  std::lock_guard lock(impl_->write_mu);
  if (impl_->in_write == nullptr) return false;
  std::string data(line);
  data.push_back('\n');
  DWORD done = 0;
  return ::WriteFile(impl_->in_write, data.data(), static_cast<DWORD>(data.size()), &done, nullptr) &&
         done == data.size();
}

void child_process::close_stdin() noexcept {
  std::lock_guard lock(impl_->write_mu);
  close_handle(impl_->in_write);
}

bool child_process::read_line(std::string& out) {
  for (;;) {
    const std::size_t nl = impl_->pending.find('\n');
    if (nl != std::string::npos) {
      out.assign(impl_->pending, 0, nl);
      impl_->pending.erase(0, nl + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (impl_->out_read == nullptr) return false;
    char buf[4096];
    DWORD n = 0;
    if (!::ReadFile(impl_->out_read, buf, sizeof(buf), &n, nullptr) || n == 0) {
      close_handle(impl_->out_read);
      if (impl_->pending.empty()) return false;
      out.swap(impl_->pending);
      impl_->pending.clear();
      return true;
    }
    impl_->pending.append(buf, n);
  }
}

void child_process::kill() noexcept {
  if (impl_->process != nullptr && !impl_->reaped) {
    impl_->killed = true;
    ::TerminateProcess(impl_->process, 1);
  }
}

int child_process::wait() noexcept {
  if (impl_->process == nullptr) return -1;
  if (impl_->reaped) return impl_->exit_code;
  ::WaitForSingleObject(impl_->process, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(impl_->process, &code);
  impl_->reaped = true;
  // A crash exits with an NTSTATUS (0xC0000005 and friends), never a small code.
  impl_->exit_code = impl_->killed || code >= 0xC0000000u ? -1 : static_cast<int>(code);
  return impl_->exit_code;
}

}  // namespace mv::io
