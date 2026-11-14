// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/args.h"

#include <cstdlib>

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
         "  driftwire send PATH...       open it with these files (or one folder) ready,\n"
         "         [--to NAME]           closing when that send ends; --to sends to NAME as\n"
         "                               soon as it is heard (the code is still typed)\n"
         "  driftwire receive [--once]   open it to receive; --once closes after the first\n"
         "                               offer, accepted or declined\n"
         "  driftwire code               open it with a code to receive from anywhere,\n"
         "                               closing when that receive ends\n"
         "  driftwire devices            list the devices this computer remembers\n"
         "  driftwire update             update driftwire the way it was installed\n"
         "  driftwire serve              run the NAS service (no terminal needed), serving the\n"
         "         [--data DIR]          folders in DIR/serve.json to paired devices; serve init\n"
         "                               makes its keys, serve devices|add|revoke|pair-code its pairings,\n"
         "                               serve disown forgets the desktop that claimed it,\n"
         "                               serve status shows its identity and passphrase, serve passphrase\n"
         "                               set|reset changes or resets the owner's passphrase (here only);\n"
         "                               add, pair-code --for 24h|7d|... make a timed phone, serve grant\n"
         "                               NAME-OR-KEY --for D|--forever|--role view|change changes one\n"
         "  driftwire host               host the service and a relay on this computer, from\n"
         "         [--data DIR]          its own folder (DIR/host), until stopped: what starts at\n"
         "                               login; host share PATH [--label L] [--backup] gives phones\n"
         "                               a folder (--backup: they may back up into it), host unshare\n"
         "                               LABEL takes one back, host status shows it, host\n"
         "                               start-at-login on|off and host stay-awake on|off change\n"
         "                               how it runs (no administrator needed); host setup walks\n"
         "                               through it all (a DuckDNS name, a folder, the router, a\n"
         "                               phone, the VPN), host name duckdns|none sets the name (the\n"
         "                               token is typed), host pair shows a phone's pairing QR, host\n"
         "                               vpn on [--tls-port 443]|off makes it the laptop's VPN exit;\n"
         "                               host users [add NAME [--role view|change|admin] [--folders\n"
         "                               L1,L2] | role NAME ROLE | password NAME | remove NAME | apply]\n"
         "                               keeps who may open its folders from other computers at home\n"
         "                               (the admin passphrase and passwords are typed; an administrator\n"
         "                               approves each change to this computer's file sharing)\n"
         "                               host drives lists the drives, host drives choices DRIVE...\n"
         "                               shows Separate, Mirror, Span, Stripe and Parity for them, host\n"
         "                               drives combine CHOICE DRIVE... --label L erases and combines\n"
         "                               them (each name typed; an administrator once), host drives\n"
         "                               replace SET DRIVE puts a new drive in a mirror\n"
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
         "Once a day it asks its site whether a newer driftwire is out, and says so; /updates\n"
         "off, or DRIFTWIRE_NO_UPDATE_CHECK=1, stops that.\n"
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
  bool want_label = false, want_backup = false;
  bool want_users_opts = false;  // --folders, --input-stdin (host users only)
  bool want_role = false;        // --role: host users, or a device's role (serve grant)
  bool want_drives_flag = false;  // --json, --confirm-stdin: host drives only

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
      } else if (t.name == "--data") {
        if (!value(a.data, "a folder")) return out;
      } else if (t.name == "--for") {
        if (!value(a.for_duration, "a duration (24h, 7d, 90m)")) return out;
      } else if (t.name == "--forever" && flag) {
        a.forever = true;
      } else if (t.name == "--passphrase-stdin" && flag) {
        a.passphrase_stdin = true;
      } else if (t.name == "--no-passphrase" && flag) {
        a.no_passphrase = true;
      } else if (t.name == "--label") {
        if (!value(a.label, "a label")) return out;
        want_label = true;
      } else if (t.name == "--tls-port") {
        std::string v;
        if (!value(v, "a port")) return out;
        char* end = nullptr;
        const long p = std::strtol(v.c_str(), &end, 10);
        if (v.empty() || *end != '\0' || p < 1 || p > 65535) {
          out.error = "--tls-port takes a port, 1 to 65535";
          return out;
        }
        a.tls_port = static_cast<std::uint16_t>(p);
      } else if (t.name == "--json" && flag) {  // host drives (host_drives.h)
        a.json = true;
        want_drives_flag = true;
      } else if (t.name == "--confirm-stdin" && flag) {
        a.confirm_stdin = true;
        want_drives_flag = true;
      } else if (t.name == "--backup" && flag) {
        a.backup = true;
        want_backup = true;
      } else if (t.name == "--role") {  // ---- host users (self-host-users-smb.md) ----
        if (!value(a.role, "a role (view, change or admin)")) return out;
        want_role = true;
      } else if (t.name == "--folders") {
        if (!value(a.folders, "folder labels (Photos,Videos) or all")) return out;
        want_users_opts = true;
      } else if (t.name == "--input-stdin" && flag) {
        a.input_stdin = true;
        want_users_opts = true;
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
      } else if (test_hooks && t.name == "--test-quick" && flag) {
        a.test_quick = true;
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
      } else if (s == "update") {
        a.cmd = command::update;
      } else if (s == "serve") {
        a.cmd = command::serve;
      } else if (s == "host") {
        a.cmd = command::host;
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
        out.error = "code takes no arguments (a code someone gave you is pasted into /send-to in Driftwire)";
        return out;
      }
      break;
    case command::receive:
    case command::devices:
    case command::update:
      if (!words.empty()) {
        out.error = std::string(a.cmd == command::receive  ? "receive"
                                : a.cmd == command::devices ? "devices"
                                                            : "update") +
                    " takes no arguments";
        return out;
      }
      break;
    case command::serve: {
      if (words.empty()) break;
      const std::string& what = words.front();
      if (what == "init" && words.size() == 1) {
        a.serve = serve_action::init;
      } else if (what == "devices" && words.size() == 1) {
        a.serve = serve_action::devices;
      } else if (what == "pair-code" && words.size() <= 2) {
        a.serve = serve_action::pair_code;
        if (words.size() == 2) a.serve_key = words[1];
      } else if (what == "add" && words.size() == 3) {
        a.serve = serve_action::add;
        a.name = words[1];
        a.serve_key = words[2];
      } else if (what == "revoke" && words.size() == 2) {
        a.serve = serve_action::revoke;
        a.serve_key = words[1];
      } else if (what == "disown" && words.size() == 1) {
        a.serve = serve_action::disown;
      } else if (what == "grant" && words.size() == 2) {
        a.serve = serve_action::grant;
        a.serve_key = words[1];
      } else if (what == "status" && words.size() == 1) {
        a.serve = serve_action::status;
      } else if (what == "self-test" && words.size() == 1) {
        a.serve = serve_action::self_test;
      } else if (what == "passphrase" && words.size() == 2 && words[1] == "set") {
        a.serve = serve_action::passphrase_set;
      } else if (what == "passphrase" && words.size() == 2 && words[1] == "reset") {
        a.serve = serve_action::passphrase_reset;
      } else {
        out.error = "serve takes init, status, devices, add NAME KEY, revoke NAME-OR-KEY, pair-code [SECONDS], "
                    "grant NAME-OR-KEY, disown, self-test or passphrase set|reset";
      }
      if (out.error.empty()) {
        const bool timed = a.serve == serve_action::add || a.serve == serve_action::pair_code || a.serve == serve_action::grant;
        if ((!a.for_duration.empty() || a.forever) && !timed) {
          out.error = "--for and --forever go with serve add, pair-code or grant";
        } else if (a.serve == serve_action::grant && !a.for_duration.empty() && a.forever) {
          out.error = "serve grant takes --for DURATION or --forever, one of them";
        } else if (a.serve == serve_action::grant && a.for_duration.empty() && !a.forever && !want_role) {
          out.error = "serve grant takes --for DURATION, --forever or --role view|change";
        } else if (want_role && (a.serve != serve_action::grant || (a.role != "view" && a.role != "change"))) {
          out.error = "--role goes with serve grant (view or change) or host users";
        } else if (a.forever && a.serve != serve_action::grant) {
          out.error = "--forever goes with serve grant";
        }
      }
      if (out.error.empty() && (a.passphrase_stdin || a.no_passphrase)) {
        const bool asks = a.serve == serve_action::init || a.serve == serve_action::passphrase_set ||
                          a.serve == serve_action::passphrase_reset;
        if (!asks || (a.passphrase_stdin && a.no_passphrase) ||
            (a.no_passphrase && a.serve != serve_action::init)) {
          out.error = "--passphrase-stdin goes with serve init or serve passphrase, --no-passphrase with serve init alone";
        }
      }
      return out;
    }
    case command::host: {
      const auto on_off = [&](host_action act) {
        if (words.size() == 2 && (words[1] == "on" || words[1] == "off")) {
          a.host = act;
          a.on = words[1] == "on";
          return true;
        }
        return false;
      };
      const std::string what = words.empty() ? std::string() : words.front();
      if (words.empty()) {
        a.host = host_action::run;
      } else if (what == "child" && words.size() == 1) {
        a.host = host_action::child;
      } else if (what == "share" && words.size() == 2) {
        a.host = host_action::share;
        a.host_path = words[1];
      } else if (what == "unshare" && words.size() == 2 && !want_label) {
        a.host = host_action::unshare;
        a.label = words[1];
      } else if (what == "status" && words.size() == 1) {
        a.host = host_action::status;
      } else if (what == "start-at-login" && on_off(host_action::start_at_login)) {
      } else if (what == "stay-awake" && on_off(host_action::stay_awake)) {
      } else if (what == "vpn" && on_off(host_action::vpn)) {
      } else if (what == "setup" && words.size() == 1) {
        a.host = host_action::setup;
      } else if (what == "pair" && words.size() == 1) {
        a.host = host_action::pair;
      } else if (what == "users") {
        // ---- host users (self-host-users-smb.md): host_users.cpp checks the words. ----
        a.host = host_action::users;
        a.host_words.assign(words.begin() + 1, words.end());
      } else if (what == "smb-apply" && words.size() <= 2) {
        a.host = host_action::smb_apply;
        a.host_words.assign(words.begin() + 1, words.end());
      } else if (what == "drives") {
        // host drives …: its own words, read by parse_drives_words (host_drives.h).
        a.host = host_action::drives;
        a.paths.assign(words.begin() + 1, words.end());
      } else if (what == "name" && words.size() == 2 && (words[1] == "duckdns" || words[1] == "none")) {
        // Only the provider: the subdomain and the token are typed or pasted, never arguments.
        a.host = host_action::name;
        a.on = words[1] == "duckdns";
      } else {
        out.error = "host takes setup, share PATH [--label L] [--backup], unshare LABEL, status, pair, "
                    "name duckdns|none, vpn on [--tls-port N]|off, start-at-login on|off, stay-awake on|off, "
                    "users … or drives …";
        return out;
      }
      if (a.tls_port != 0 && !(a.host == host_action::vpn && a.on)) {
        out.error = "--tls-port goes with host vpn on";
        return out;
      }
      if (want_drives_flag && a.host != host_action::drives) {
        out.error = "--json and --confirm-stdin go with host drives";
        return out;
      }
      if (want_label && a.host == host_action::drives && !want_backup) break;  // the combined drive's name
      if ((want_label || want_backup) && a.host != host_action::share) {
        out.error = "--label and --backup go with host share";
        return out;
      }
      if ((want_users_opts || want_role) && a.host != host_action::users) {
        out.error = "--role, --folders and --input-stdin go with host users";
        return out;
      }
      break;
    }
    case command::session:
    case command::help:
    case command::version:
      break;
  }
  if (a.tls_port != 0 && a.cmd != command::host) {
    out.error = "--tls-port goes with host vpn on";
    return out;
  }
  if ((want_label || want_backup) && a.cmd != command::host) {
    out.error = "--label and --backup go with host share";
    return out;
  }
  if ((want_users_opts || (want_role && a.cmd != command::serve)) && a.cmd != command::host) {
    out.error = "--role, --folders and --input-stdin go with host users";
    return out;
  }
  if (want_drives_flag && a.cmd != command::host) {
    out.error = "--json and --confirm-stdin go with host drives";
    return out;
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
