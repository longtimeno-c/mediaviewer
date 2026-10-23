// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/args.h"

#include <string_view>

namespace mv::driftwire::cli {

namespace {

// "--to=Ann" -> ("--to", "Ann"); "--to" -> ("--to", none).
struct option_token {
  std::string_view name;
  std::string_view value;
  bool has_value = false;
};

option_token split_option(std::string_view s) {
  option_token t;
  if (const auto eq = s.find('='); s.rfind("--", 0) == 0 && eq != std::string_view::npos) {
    t.name = s.substr(0, eq);
    t.value = s.substr(eq + 1);
    t.has_value = true;
  } else {
    t.name = s;
  }
  return t;
}

}  // namespace

std::string usage_line() { return "Try 'driftwire --help'."; }

std::string help_text() {
  return "Driftwire sends files to a computer nearby, or anywhere with a code. It needs a\n"
         "terminal: it asks before anything is sent or saved.\n"
         "\n"
         "Usage:\n"
         "  driftwire                    open Driftwire: computers nearby can see this one\n"
         "                               and send to it while it is open; send from it\n"
         "  driftwire send PATH...       open it with these files (or one folder) ready\n"
         "         [--to NAME]           and send to NAME as soon as it is heard (the code\n"
         "                               is still typed)\n"
         "  driftwire receive [--once]   open it to receive; --once closes after one transfer\n"
         "  driftwire code               open it with a code to receive from anywhere\n"
         "  driftwire devices            list the devices this computer remembers\n"
         "  driftwire --help | --version\n"
         "\n"
         "Options:\n"
         "  --save-to DIR   save received files in DIR (this run only)\n"
         "  --name NAME     the name other computers see (this run only)\n"
         "  --plain         lines only: no colour and no cursor movement (also when TERM=dumb)\n"
         "  --no-color      no colour (also when NO_COLOR is set)\n"
         "  --verbose       show what Driftwire logs (never a file name or an address)\n"
         "\n"
         "Codes, check digits and relay keys are never arguments: they are typed or pasted\n"
         "in Driftwire, where /help lists its commands and keys.\n"
         "\n"
         "Exit status: 0 when it ends normally; 1 when the transfer of send, receive --once\n"
         "or code did not go through; 2 for a usage error, no terminal, or when it could\n"
         "not start (Driftwire already open in another terminal).\n";
}

parsed_args parse_args(std::span<const std::string> argv, bool test_hooks) {
  parsed_args out;
  args& a = out.a;
  bool help = false, version = false;
  bool have_command = false, options_done = false;
  std::vector<std::string> words;  // after the command
  std::string first_word;          // a word that is not a command, for the error
  bool want_to = false, want_once = false;

  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view s = argv[i];
    if (!options_done && s == "--") {
      options_done = true;
      continue;
    }
    if (!options_done && s.size() > 1 && s[0] == '-') {
      const option_token t = split_option(s);
      // The option's value: after "=", or the next argument.
      const auto value = [&](std::string& into, const char* what) -> bool {
        if (t.has_value) {
          into = std::string(t.value);
        } else if (i + 1 < argv.size()) {
          into = argv[++i];
        } else {
          out.error = std::string(t.name) + " needs " + what;
          return false;
        }
        if (into.empty()) {
          out.error = std::string(t.name) + " needs " + what;
          return false;
        }
        return true;
      };
      const bool flag = !t.has_value;
      if ((t.name == "--help" || t.name == "-h") && flag) {
        help = true;
      } else if ((t.name == "--version" || t.name == "-V") && flag) {
        version = true;
      } else if (t.name == "--to") {
        if (!value(a.to, "a name")) return out;
        want_to = true;
      } else if (t.name == "--once" && flag) {
        a.once = true;
        want_once = true;
      } else if (t.name == "--save-to") {
        if (!value(a.save_to, "a folder")) return out;
      } else if (t.name == "--name") {
        if (!value(a.name, "a name")) return out;
      } else if (t.name == "--plain" && flag) {
        a.plain = true;
      } else if ((t.name == "--no-color" || t.name == "--no-colour") && flag) {
        a.no_color = true;
      } else if (t.name == "--verbose" && flag) {
        a.verbose = true;
      } else if (test_hooks && t.name == "--test-loopback" && flag) {
        a.test_loopback = true;
      } else if (test_hooks && t.name == "--test-data") {
        if (!value(a.test_data, "a folder")) return out;
      } else if (test_hooks && t.name == "--test-events" && flag) {
        a.test_events = true;
      } else {
        out.error = "unknown option " + std::string(s.substr(0, s.find('=')));
        return out;
      }
      continue;
    }
    if (!have_command) {
      have_command = true;
      if (s == "send") {
        a.cmd = command::send;
      } else if (s == "receive") {
        a.cmd = command::receive;
      } else if (s == "code") {
        a.cmd = command::code;
      } else if (s == "devices") {
        a.cmd = command::devices;
      } else if (s == "help") {
        help = true;
      } else {
        first_word = std::string(s);
      }
      continue;
    }
    words.emplace_back(s);
  }

  if (help) {
    a.cmd = command::help;
    return out;
  }
  if (version) {
    a.cmd = command::version;
    return out;
  }
  if (!first_word.empty()) {
    out.error = "\"" + first_word + "\" is not a command (to send it: driftwire send \"" + first_word + "\")";
    return out;
  }
  switch (a.cmd) {
    case command::send:
      if (words.empty()) {
        out.error = "send needs the files to send, or one folder";
        return out;
      }
      a.paths = std::move(words);
      break;
    case command::code:
      if (!words.empty()) {
        // Never a code on the command line: it would sit in the history and `ps`.
        out.error = "code takes no arguments (a code someone gave you is pasted into /to-code in Driftwire)";
        return out;
      }
      break;
    case command::receive:
    case command::devices:
      if (!words.empty()) {
        out.error = std::string(a.cmd == command::receive ? "receive" : "devices") + " takes no arguments";
        return out;
      }
      break;
    case command::session:
    case command::help:
    case command::version:
      break;
  }
  if (want_to && a.cmd != command::send) {
    out.error = "--to goes with send (driftwire send PATH... --to NAME)";
    return out;
  }
  if (want_once && a.cmd != command::receive) {
    out.error = "--once goes with receive";
    return out;
  }
  return out;
}

}  // namespace mv::driftwire::cli
