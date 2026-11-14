// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host pause | resume | quit` (docs/plans/self-hosted.md "Tray"):
// the menu-bar / notification-area icon's control of a running host, also
// typed. Owner-local only: each writes host.json in the host's own folder
// (this user's alone, 0700), which the running host reads every 2 s; nothing
// listens for them on a socket.
//
//   pause    `paused` set: the service and the relay stop (the router's
//            mapping goes with them) and the computer may sleep; the host
//            itself keeps running, so its icon stays and says Paused. Kept
//            until `resume`, across restarts.
//   resume   `paused` cleared: served again.
//   quit     `quit_stamp` set to now: a host started before it stops (the
//            child ends with kHostChildQuit, the supervisor with it), waited
//            for up to 10 s, then the stamp is cleared. What starts at login
//            stays as it was set (`host start-at-login`).
#pragma once

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_cmd.h"

namespace mv::driftwire::cli {

// The words after `host`: true (and a.host set) for one of these verbs.
[[nodiscard]] bool parse_host_control(const std::vector<std::string>& words, args& a);
[[nodiscard]] int run_host_control(const args& a, const host_layout& l);

}  // namespace mv::driftwire::cli
