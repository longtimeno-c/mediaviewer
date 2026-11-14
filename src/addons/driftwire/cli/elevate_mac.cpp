// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// macOS: sudo at a terminal, the authorization dialog from the app (elevate.h).
#include "addons/driftwire/cli/elevate.h"

#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sodium.h>

#include "addons/driftwire/host_smb.h"

extern char** environ;

namespace mv::driftwire::cli {

namespace {

constexpr std::size_t kMaxLine = host_smb::kMaxRequestBytes + 2;

bool write_all(int fd, std::string_view s) {
  while (!s.empty()) {
    const ssize_t n = ::write(fd, s.data(), s.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    s.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

std::string read_line_fd(int fd) {
  std::string out;
  char c = 0;
  while (out.size() < kMaxLine) {
    const ssize_t n = ::read(fd, &c, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || c == '\n') break;
    out.push_back(c);
  }
  return out;
}

elevated_run with_sudo(const std::string& program, std::string_view request) {
  elevated_run r;
  int in[2] = {-1, -1}, out[2] = {-1, -1};
  if (pipe(in) != 0) return r;
  if (pipe(out) != 0) {
    close(in[0]);
    close(in[1]);
    return r;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_addclose(&fa, in[1]);
  posix_spawn_file_actions_addclose(&fa, out[0]);
  // -k: ask now, whatever an earlier sudo left; the prompt goes to the terminal.
  const char* argv[] = {"/usr/bin/sudo", "-k", "-p", "Your Mac's administrator password (for sudo): ", program.c_str(),
                        "host", "smb-apply", nullptr};
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, "/usr/bin/sudo", &fa, nullptr, const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(in[0]);
  close(out[1]);
  if (rc != 0) {
    close(in[1]);
    close(out[0]);
    r.why = "could not start sudo";
    return r;
  }
  std::string line(request);
  line.push_back('\n');
  (void)write_all(in[1], line);
  sodium_memzero(line.data(), line.size());
  close(in[1]);
  r.answer = read_line_fd(out[0]);
  close(out[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  r.started = !r.answer.empty();
  r.declined = !r.started;
  if (r.declined) r.why = "no administrator was given (sudo said no)";
  return r;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
elevated_run with_dialog(const std::string& program, std::string_view request) {
  elevated_run r;
  AuthorizationRef auth = nullptr;
  if (AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, &auth) != errAuthorizationSuccess) {
    r.why = "could not ask for an administrator";
    return r;
  }
  AuthorizationItem item{kAuthorizationRightExecute, 0, nullptr, 0};
  AuthorizationRights rights{1, &item};
  const AuthorizationFlags flags = kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights |
                                   kAuthorizationFlagPreAuthorize;
  const OSStatus got = AuthorizationCopyRights(auth, &rights, kAuthorizationEmptyEnvironment, flags, nullptr);
  if (got != errAuthorizationSuccess) {
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    r.declined = true;
    r.why = got == errAuthorizationCanceled ? "the administrator's approval was cancelled" : "no administrator was given";
    return r;
  }
  char* argv[] = {const_cast<char*>("host"), const_cast<char*>("smb-apply"), nullptr};
  FILE* pipe = nullptr;
  const OSStatus ran = AuthorizationExecuteWithPrivileges(auth, program.c_str(), kAuthorizationFlagDefaults, argv, &pipe);
  AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
  if (ran != errAuthorizationSuccess || !pipe) {
    r.why = "the elevated step could not start";
    return r;
  }
  std::string line(request);
  line.push_back('\n');
  (void)write_all(fileno(pipe), line);
  sodium_memzero(line.data(), line.size());
  r.answer = read_line_fd(fileno(pipe));
  std::fclose(pipe);
  // The step's process is reaped here: it ends right after its answer.
  int status = 0;
  (void)waitpid(-1, &status, WNOHANG);
  r.started = !r.answer.empty();
  if (!r.started) r.why = "the elevated step gave no answer";
  return r;
}
#pragma clang diagnostic pop

}  // namespace

elevated_run run_elevated(const std::string& program, std::string_view request, bool terminal) {
  return terminal ? with_sudo(program, request) : with_dialog(program, request);
}

bool helper_read(const std::string&, std::string& line) {
  line = read_line_fd(0);
  return !line.empty();
}

bool helper_write(const std::string&, const std::string& line) {
  std::string l = line + "\n";
  return write_all(1, l);
}

}  // namespace mv::driftwire::cli
