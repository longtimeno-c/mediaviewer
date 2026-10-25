// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// driftwire: Driftwire in a terminal (docs/design/28 "From a terminal"), on
// Linux, Windows and macOS. A host of the same engine as the add-on, with an
// identity, settings and a data folder of its own.
//
// The steps, each with its way out: the arguments (2 on a usage error);
// --help, --version, `devices` and `update`, which need no terminal; then a terminal
// on both ends (2 without one: it asks before anything is sent or saved),
// the files to send, one session per user (2 when another terminal has it),
// and the session itself, whose exit code is the program's.
//
// The test build (mv_driftwire_cli_test, MV_DRIFTWIRE_CLI_TEST_HOOKS, never
// packaged) also runs without a terminal and takes --test-* options
// (args.h) for the end-to-end test.
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
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
#include "addons/driftwire/cli/update.h"
#include "addons/driftwire/engine.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/power.h"
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

// DRIFTWIRE_NO_UPDATE_CHECK, set to anything: no check (update.h).
bool update_check_off() {
  const char* off = std::getenv("DRIFTWIRE_NO_UPDATE_CHECK");  // NOLINT(concurrency-mt-unsafe): read at start
  return off && *off;
}

std::string site() {
  return cli::site_url(std::getenv("DRIFTWIRE_REPO_URL"));  // NOLINT(concurrency-mt-unsafe): read at start
}

// `driftwire update`: this copy's own installer again, when the site has a
// newer version or cannot say. Its output is the installer's own.
int run_update() {
  const cli::install_kind kind = cli::this_install();
  const std::string from = site();
  const std::string version = MV_DRIFTWIRE_CLI_VERSION;
  if (kind == cli::install_kind::none) {
    cli::print("driftwire " + version + " was not installed by one of Driftwire's installers, so it cannot update "
               "itself.\nInstallers for Linux, macOS and Windows: " + from + "/\n");
    return 1;
  }
  const cli::http_answer a = cli::https_get(from + "/latest.json", 4096, cli::kCheckTimeoutMs);
  const std::optional<std::string> latest = a.status == 200 ? cli::parse_latest(a.body) : std::nullopt;
  if (latest) {
    const std::string data = cli::data_dir();
    cli::update_state st = cli::load_update_state(data);
    st.checked = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    st.latest = *latest;
    (void)cli::save_update_state(data, st);
    if (!cli::newer_version(*latest, version)) {
      cli::print("driftwire " + version + " is up to date.\n");
      return 0;
    }
    cli::print("Updating driftwire " + version + " to " + *latest + ": " + cli::update_command_shown(kind, from) +
               "\n");
  } else {
    // The installer knows better than an answer that did not come.
    cli::print("Could not ask " + from + " for the newest version; running the installer anyway: " +
               cli::update_command_shown(kind, from) + "\n");
  }
  const std::string cmd = cli::update_command_run(kind, from);
  if (cmd.empty()) return fail("the site's address cannot be used in a command: " + from);
  const int r = std::system(cmd.c_str());  // NOLINT(concurrency-mt-unsafe, cert-env33-c): our own fixed command
  if (r != 0) {
    cli::print("driftwire: the update did not finish (the installer said why, above).\n", true);
    return 1;
  }
  cli::print("Done. The next driftwire you start is the new version.\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // The reason the OS shows while a transfer keeps the machine awake
  // ("Driftwire is receiving files"): this process is Driftwire, not the app.
  dw::power::set_owner("Driftwire");
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
#if !defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  if (a.cmd == cli::command::update) return run_update();
#else
  if (a.cmd == cli::command::update) return fail("the test build does not update");
#endif

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
  const std::string& home = paths.home;  // UTF-8 (home_dir())

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
  const bool dumb = term_env && std::string(term_env) == "dumb";
  bool plain = a.plain || dumb;

  cli::console::options co;
  // A dumb terminal would print them; --plain promises lines only (a screen
  // reader, a log), so no bracketed paste either: a paste is then told from
  // typing by its newlines (keys.cpp).
  co.sequences = !dumb && !a.plain;
  cli::console con(co);
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
  so.mouse = co.sequences && !plain;  // clicks while there is something to click (session.h)
  so.version = MV_DRIFTWIRE_CLI_VERSION;
  so.data_dir = data;
  if (!a.name.empty()) so.name_note = "this run";
  so.paths = paths;
  // Is a newer driftwire out? Only for a copy an installer put here (a build
  // of one's own has nothing to update with), never in the test build, and
  // never once turned off. It never holds the session up (update.h).
  cli::update_check updates;
#if !defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  const cli::install_kind kind = cli::this_install();
  if (kind == cli::install_kind::none) {
    so.no_updates_why = "this copy was not put here by an installer";
  } else if (update_check_off()) {
    so.no_updates_why = "DRIFTWIRE_NO_UPDATE_CHECK is set";
  } else if (data.empty()) {
    so.no_updates_why = "it has no data folder to keep the answer in";
  } else {
    updates.start(data, MV_DRIFTWIRE_CLI_VERSION, site(), [&con] { con.wake(); });
    so.updates = &updates;
  }
#else
  so.no_updates_why = "the test build never checks";
#endif
#if defined(MV_DRIFTWIRE_CLI_TEST_HOOKS)
  if (a.test_events) {
    so.events = [](const std::string& line) { cli::print("@driftwire " + line + "\n", true); };
  }
#endif

  int code = 0;
  try {
    cli::session s(con, host, std::move(eo), std::move(so), th);
    code = s.run();  // the engine is gone when it returns
  } catch (...) {
    // A bug, never a way out (std::filesystem throws on a path it cannot
    // convert, for one): not an abort that leaves the terminal raw and a
    // transfer unwound by nobody. Unwinding has stopped the engine.
    updates.stop();
    con.leave_raw();
    return fail("something went wrong and the session closed.");
  }
  updates.stop();  // a check still out calls nobody
  con.leave_raw();
  return code;
}
