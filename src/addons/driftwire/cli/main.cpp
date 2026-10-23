// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// driftwire: Driftwire in a terminal (docs/design/28 "From a terminal"), on
// Linux, Windows and macOS. A host of the same engine as the add-on, with an
// identity, settings and a data folder of its own.
//
// The steps, each with its way out: the arguments (2 on a usage error);
// --help, --version and `devices`, which need no terminal; then a terminal
// on both ends (2 without one: it asks before anything is sent or saved),
// the files to send, one session per user (2 when another terminal has it),
// and the session itself, whose exit code is the program's.
//
// The test build (mv_driftwire_cli_test, MV_DRIFTWIRE_CLI_TEST_HOOKS, never
// packaged) also runs without a terminal and takes --test-* options
// (args.h) for the end-to-end test.
#include <sodium.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host.h"
#include "addons/driftwire/cli/paths_arg.h"
#include "addons/driftwire/cli/session.h"
#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/theme.h"
#include "addons/driftwire/engine.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/prefs.h"
#include "addons/driftwire/secrets.h"

#ifndef MV_DRIFTWIRE_CLI_VERSION
#define MV_DRIFTWIRE_CLI_VERSION "0.0.0"
#endif

namespace cli = mv::driftwire::cli;
namespace dw = mv::driftwire;
namespace fs = std::filesystem;

namespace {

int fail(const std::string& why, bool usage = false) {
  cli::print("driftwire: " + why + "\n" + (usage ? cli::usage_line() + "\n" : std::string()), true);
  return 2;
}

fs::path to_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// An argument as an absolute path: "~/" (a shell leaves it alone after
// "--save-to=") from the home folder, the rest from the current folder.
std::string absolute_of(const std::string& arg, const std::string& home) {
  std::string s = arg;
  if (!home.empty() && (s == "~" || s.rfind("~/", 0) == 0 || s.rfind("~\\", 0) == 0)) s = home + s.substr(1);
  std::error_code ec;
  fs::path p = fs::absolute(to_path(s), ec);
  if (ec) p = to_path(s);
  return to_utf8(p.lexically_normal());
}

#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
// What the test build's engine hears nearby: nothing (it reaches the other
// process by an internet code over loopback).
class no_discovery final : public dw::discovery {
 public:
  bool start(const dw::beacon&, seen_fn, tick_fn) override { return true; }
  void stop() noexcept override {}
};
#endif

int list_devices() {
  if (sodium_init() < 0) return fail("could not start its cryptography");
#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  std::unique_ptr<dw::secrets::store> store;  // the test build keeps no identity
#else
  std::unique_ptr<dw::secrets::store> store = dw::secrets::os_store(dw::secrets::kCliService);
#endif
  if (!store) {
    cli::print("This computer remembers no devices: there is no secret store here to keep its identity in.\n");
    return 0;
  }
  const auto devices = dw::known::load(*store);
  if (devices.empty()) {
    cli::print("No remembered devices yet. After a transfer, Driftwire asks whether to remember the other "
               "computer.\n");
    return 0;
  }
  std::string out;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    out += std::to_string(i + 1) + ". " + devices[i].name + (devices[i].own ? " (my computer)" : "") + "\n";
  }
  cli::print(out);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> argv8 = cli::utf8_args(argc, argv);
  #if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  const cli::parsed_args parsed = cli::parse_args(argv8, true);
#else
  const cli::parsed_args parsed = cli::parse_args(argv8, false);
#endif
  if (!parsed.error.empty()) return fail(parsed.error, true);
  const cli::args& a = parsed.a;
  if (a.cmd == cli::command::help) {
    cli::print(cli::help_text());
    return 0;
  }
  if (a.cmd == cli::command::version) {
    cli::print(std::string("driftwire ") + MV_DRIFTWIRE_CLI_VERSION + "\n");
    return 0;
  }

  if (a.cmd == cli::command::devices) return list_devices();

#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  std::string data = a.test_data;
  if (!data.empty()) {
    std::error_code ec;
    fs::create_directories(to_path(data), ec);
  } else {
    data = cli::data_dir();
  }
#else
  // Interactive only (the owner's call): a person at a terminal answers
  // every offer, so there is no way to run it from a pipe or a script.
  if (!cli::console::is_terminal()) return fail("this needs a terminal (it asks before anything is sent or saved).");
  const std::string data = cli::data_dir();
#endif

  const cli::path_options paths = cli::default_path_options();
  const std::string home = paths.home.empty() ? cli::home_dir() : paths.home;

  // The files to send: each there; a folder alone.
  std::vector<std::string> send;
  if (a.cmd == cli::command::send) {
    std::string missing;
    int folders = 0;
    for (const auto& p : a.paths) {
      const std::string abs = absolute_of(p, home);
      std::error_code ec;
      const auto st = fs::status(to_path(abs), ec);
      if (ec || !fs::exists(st)) {
        missing += (missing.empty() ? "" : ", ") + p;
        continue;
      }
      if (fs::is_directory(st)) ++folders;
      send.push_back(abs);
    }
    if (!missing.empty()) return fail("not found: " + missing);
    if (folders > 0 && send.size() > 1) return fail("a send is either files or one folder", true);
  }
  std::string save_to;
  if (!a.save_to.empty()) {
    save_to = absolute_of(a.save_to, home);
    std::error_code ec;
    if (!fs::is_directory(to_path(save_to), ec)) return fail("--save-to: " + a.save_to + " is not a folder");
  }

  // One session per user: two would be two tiles on every screen nearby,
  // and two writers of one prefs file.
  std::unique_ptr<dw::net::instance_lock> lock;
  if (!data.empty()) {
    lock = dw::net::try_lock_instance(data + "/" + std::string(dw::kPrefsFile) + ".lock");
    if (!lock) return fail("Driftwire is already open in another terminal.");
  }

  // The engine: its own prefs, its own identity, no Bluetooth (docs/design/28
  // "From a terminal"), and --save-to / --name for this run only (never an
  // on_event, which would save them).
  dw::engine::options eo;
  eo.my_name = a.name;
  eo.use_bluetooth = false;
  cli::session_options so;
  if (!data.empty()) {
    eo.prefs_path = data + "/" + std::string(dw::kPrefsFile);
    dw::prefs kept = dw::load_prefs(eo.prefs_path).value_or(dw::prefs{});
    kept.receiving = kept.background = false;  // no background receiving in a terminal
    if (!save_to.empty()) {
      so.restore_destination = kept.destination;
      kept.destination.clear();  // the default (default_destination) is this run's folder
    }
    eo.initial = kept;
  }
#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  // No identity kept (a key for this run), and with --test-loopback no
  // router, no beacon, no address but loopback.
  if (a.test_loopback) {
    eo.loopback_only = true;
    eo.internet_loopback = true;
    eo.internet_mapping = std::nullopt;
    eo.make_discovery = [] { return std::make_unique<no_discovery>(); };
  }
#else
  // Its own identity and remembered devices, never the app's (secrets.h).
  eo.make_secrets = [] { return dw::secrets::os_store(dw::secrets::kCliService); };
#endif

  const char* term_env = std::getenv("TERM");  // NOLINT(concurrency-mt-unsafe): read at start
  bool plain = a.plain || (term_env && std::string(term_env) == "dumb");

  cli::console con;
  const bool raw = con.enter_raw();
  // The live region needs raw mode (and VT on Windows): without, lines.
  if (!raw) plain = true;
  // After enter_raw: on Windows FTXUI's detection reads the console's VT mode.
  const cli::color_depth depth = plain ? cli::color_depth::none : cli::detect_color_depth(a.no_color);
  const cli::theme th = cli::make_theme(depth, cli::console::utf8());

  cli::cli_host host(con, save_to.empty() ? dw::net::downloads_folder() : save_to);
  so.cmd = a.cmd;
  so.once = a.once;
  so.send_paths = std::move(send);
  so.to = a.to;
  so.plain = plain;
#if defined(_WIN32)
  so.echo = true;  // the console's own echo is for line reads only
#else
  so.echo = raw;   // cooked (a pipe in the test build): the terminal echoes, or nobody
#endif
  so.verbose = a.verbose;
  so.version = MV_DRIFTWIRE_CLI_VERSION;
  so.data_dir = data;
  if (!a.name.empty()) so.name_note = "this run";
  so.paths = paths;
  if (so.paths.home.empty()) so.paths.home = home;
#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  if (a.test_events) {
    so.events = [](const std::string& line) { cli::print("@driftwire " + line + "\n", true); };
  }
#endif

  int code = 0;
  {
    cli::session s(con, host, std::move(eo), std::move(so), th);
    code = s.run();  // the engine is gone when it returns
  }
  con.leave_raw();
  return code;
}
