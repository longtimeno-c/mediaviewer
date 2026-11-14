// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_control.h"

#include <chrono>
#include <thread>

#include "addons/driftwire/cli/data_dir.h"

namespace mv::driftwire::cli {

namespace {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int set_paused(const host_layout& l, bool paused) {
  host_settings s = host_files::settings(l);
  s.paused = paused;
  if (!host_files::save_settings(l, s)) {
    print("driftwire host: host.json could not be written.\n", true);
    return 1;
  }
  const bool running = host_files::running(l);
  if (paused) {
    print(running ? "Paused: phones cannot reach this computer until driftwire host resume.\n"
                  : "Paused: the host starts paused until driftwire host resume.\n");
  } else {
    print(running ? "Resumed: phones can reach this computer again.\n" : "Resumed: the host serves when it starts.\n");
  }
  return 0;
}

int quit(const host_layout& l) {
  if (!host_files::running(l)) {
    print("driftwire host: not running.\n");
    return 0;
  }
  host_settings s = host_files::settings(l);
  s.quit_stamp = now_ms();
  if (!host_files::save_settings(l, s)) {
    print("driftwire host: host.json could not be written.\n", true);
    return 1;
  }
  // The host reads host.json every 2 s, then stops its keepers (the router's mapping) and services.
  bool stopped = false;
  for (int i = 0; i < 100 && !stopped; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopped = !host_files::running(l);
  }
  if (stopped) {
    // Cleared, so a clock set back never stops a host started later.
    host_settings after = host_files::settings(l);
    if (after.quit_stamp == s.quit_stamp) {
      after.quit_stamp = 0;
      (void)host_files::save_settings(l, after);
    }
    print("Stopped hosting. What starts at login is as it was (driftwire host status).\n");
    return 0;
  }
  print("driftwire host: asked to quit; it stops within a few seconds.\n");
  return 0;
}

}  // namespace

bool parse_host_control(const std::vector<std::string>& words, args& a) {
  if (words.size() != 1) return false;
  if (words[0] == "pause") {
    a.host = host_action::pause;
  } else if (words[0] == "resume") {
    a.host = host_action::resume;
  } else if (words[0] == "quit") {
    a.host = host_action::quit;
  } else {
    return false;
  }
  return true;
}

int run_host_control(const args& a, const host_layout& l) {
  switch (a.host) {
    case host_action::pause:
      return set_paused(l, true);
    case host_action::resume:
      return set_paused(l, false);
    case host_action::quit:
      return quit(l);
    default:
      return 2;
  }
}

}  // namespace mv::driftwire::cli
