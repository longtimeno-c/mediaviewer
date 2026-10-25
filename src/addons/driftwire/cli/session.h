// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire session (docs/design/28 "From a terminal"): what the window
// is, in a terminal. It owns the engine for as long as it runs and is the
// only thread that calls it (cli-engine-map §2, §4): the engine's callbacks
// only wake it (host.h), and on each wake it takes the engine's snapshot,
// compares it with the last one, prints what finished into the transcript
// and draws the live region again.
//
// Opening it is opening the window: screen_opened(""), so this computer is
// visible and its name is on the beacon (#501) while the session runs, and
// never after. Closing it (/quit, Ctrl+C twice, Ctrl+D on an empty prompt,
// SIGTERM or SIGHUP, the console closing) stops everything: the engine is
// destroyed here, on this thread, before the caller restores the terminal
// (a receive cut off keeps what arrived, #530).
//
// Interactive only: nothing is accepted, sent or remembered without the
// person at this terminal answering (a key, or a line in plain mode).
//
// Two presenters over one controller: the rich one (an inline live region
// of cards, Nearby and the prompt, inline_view.h) and plain mode (--plain,
// or TERM=dumb: lines and line questions only, no colour and no cursor
// movement, for screen readers and dumb terminals).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/paths_arg.h"
#include "addons/driftwire/cli/theme.h"
#include "addons/driftwire/engine.h"

namespace mv::driftwire::cli {

class terminal;
class cli_host;

// A card that asks for consent (an offer, the check to compare, Remember)
// takes an answer only once keys have stopped for this long with it on
// screen: what was being typed at the prompt or into a field as it appeared
// ("y" of "~/my-trip", "1" of an address) is not an answer. Esc, the safe
// answer, always counts.
inline constexpr int kAnswerSettleMs = 750;

struct session_options {
  command cmd = command::session;     // send / receive / code are one-shots (receive with --once)
  bool once = false;
  std::vector<std::string> send_paths;  // absolute, existing: the files (or one folder) to send at once
  std::string to;                     // send to this peer as soon as it is heard
  bool plain = false;
  bool echo = true;      // plain mode: echo typed characters (the terminal is in raw mode)
  bool verbose = false;
  std::string version;
  std::string data_dir;  // "" : none (shown in /settings)
  std::string name_note; // "this run" when --name was given
  path_options paths;    // the current folder, home, the platform's rules
  // --save-to: the Save to kept before this run, put back at the end unless
  // the person chose a folder on purpose (the engine saves its whole view
  // whenever any setting changes).
  std::optional<std::string> restore_destination;
  // The tests and the test build: each machine-readable line ("ready",
  // "code <text>", "in connecting 123 456", "out done", "quit 0", ...).
  std::function<void(const std::string& line)> events;
};

class session {
 public:
  session(terminal& term, cli_host& host, engine::options engine_options, session_options o, const theme& th);
  ~session();
  session(const session&) = delete;
  session& operator=(const session&) = delete;

  // Runs until the person (or a signal) closes it; the exit code (args.h).
  // The engine is made at the start and destroyed before this returns.
  int run();

 private:
  struct impl;
  std::unique_ptr<impl> p_;
};

}  // namespace mv::driftwire::cli
