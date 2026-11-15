// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host start-at-login on|off` (docs/plans/self-hosted.md §3): the
// host starts when this user logs in, with no administrator, and is started
// again when it ends with anything but 0 (a crash of the supervisor itself,
// or 75: an update was installed, and the new program is the one to run).
// A port (D9), one file per OS:
//
//   Windows  autostart_win.cpp    a Scheduled Task of this user's "at log on",
//                                 least privilege, no time limit, tried again
//                                 every 5 minutes while it is not running
//                                 (Task Scheduler's own "restart on failure"
//                                 is for a start that fails, not an exit), via
//                                 schtasks.exe and an XML file of its own.
//   macOS    autostart_mac.cpp    a LaunchAgent in ~/Library/LaunchAgents,
//                                 KeepAlive unless it exited 0, loaded with
//                                 launchctl bootstrap gui/<uid>.
//   Linux    autostart_linux.cpp  a systemd --user unit, Restart=on-failure,
//                                 enabled with systemctl --user. It runs after
//                                 a reboot before anyone logs in only with
//                                 linger, which needs the person's sudo: the
//                                 command is printed, never run.
//
// The texts are pure (here, autostart.cpp) and tested on every OS; the ports
// write them and run the OS's own tool. Nothing is a service or a system unit
// (owner's call 6), and nothing here needs the person's password.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mv::driftwire::cli {

inline constexpr const char* kAutostartLabel = "io.github.longtimeno-c.driftwire.host";  // LaunchAgent
inline constexpr const char* kAutostartUnit = "driftwire-host.service";                   // systemd --user
inline constexpr const char* kAutostartTask = "Driftwire host";                           // Task Scheduler
// How often Windows tries again to start a host that is not running.
inline constexpr int kAutostartRetryMinutes = 5;

// What the entry runs: the program and its arguments ("host", and "--data DIR"
// when one was given), UTF-8.
struct autostart_command {
  std::string program;
  std::vector<std::string> args;
};

// ---- pure ----------------------------------------------------------------------------------

// The program to put in an entry: a Homebrew Cellar's versioned path becomes
// its "opt" link (which an upgrade moves to the new version); anything else
// as it is. `exists` tells whether a path is there.
[[nodiscard]] std::string stable_program(const std::string& exe, const std::function<bool(const std::string&)>& exists);

[[nodiscard]] std::string launch_agent_plist(const autostart_command& c);
[[nodiscard]] std::string systemd_user_unit(const autostart_command& c);
// The task's XML (written as UTF-16 by the port). `user`: DOMAIN\name or a SID;
// "" for whoever registers it.
[[nodiscard]] std::string scheduled_task_xml(const autostart_command& c, const std::string& user);
// The Windows command line for `c` (each argument quoted as CommandLineToArgvW reads it).
[[nodiscard]] std::string windows_command_line(const std::vector<std::string>& argv);
// The line printed on Linux for running with nobody logged in.
[[nodiscard]] std::string linger_hint(const std::string& user);

// ---- the port ------------------------------------------------------------------------------

struct autostart_place {
  // macOS and Linux: the user's home folder, where the entry goes. Windows:
  // the host's own folder, where the task's XML is kept while it is on.
  std::string home;
  bool apply = true;  // false (the tests): write or remove the file only, never run the OS's tool
};

struct autostart_result {
  bool ok = false;
  std::string note;  // printed as it is: what was done, or what the person may want to do
};

[[nodiscard]] autostart_result set_autostart(bool on, const autostart_command& c, const autostart_place& where);
// Whether the entry is there now.
[[nodiscard]] bool autostart_is_on(const autostart_place& where);

}  // namespace mv::driftwire::cli
