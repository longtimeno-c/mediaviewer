// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host's tray (docs/plans/self-hosted.md "Tray", issue #686 §4): the menu
// built from what the host's files say (phone/host_tray.h), the registry's
// entry the hosts draw (slots.h set_shell_keep), the watcher over a folder of
// the test's own (a held host.lock, a state.json written here), and the
// host's own `pause`, `resume` and `quit` taken by the service in-process on
// loopback. Never a real router, duckdns.org or the person's host.
#include "catch_compat.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/slots.h"
#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "core/json.h"
#include "import_fixture.h"
#include "phone/host_tray.h"
#include "phone/self_host.h"
#include "wire/net.h"
#include "wire/serve_wire.h"

using namespace mv::test;
namespace ht = mv::phone::host_tray;
namespace sh = mv::phone::self_host;
namespace cli = mv::driftwire::cli;
namespace serve = mv::driftwire::serve;
namespace net = mv::driftwire::net;

namespace {

cli::parsed_args parse(std::vector<std::string> argv) { return cli::parse_args(argv, true); }

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

template <typename F>
bool eventually(F pred, int ms = 10000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

sh::running_state serving(std::size_t folders = 2) {
  sh::running_state s;
  s.service_port = 7878;
  s.relay_port = 47614;
  s.folders = folders;
  return s;
}

ht::input running(sh::running_state s, std::string duck = "home") {
  ht::input in;
  in.running = true;
  in.state = std::move(s);
  in.duckdns = std::move(duck);
  return in;
}

std::vector<std::string> labels(const ht::model& m) {
  std::vector<std::string> out;
  for (const auto& r : m.rows) out.push_back(r.label);
  return out;
}

bool has(const ht::model& m, const std::string& label) {
  for (const auto& r : m.rows) {
    if (r.label == label) return true;
  }
  return false;
}

const ht::row* button(const ht::model& m, const std::string& id) {
  for (const auto& r : m.rows) {
    if (r.kind == "button" && r.id == id) return &r;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("host tray: nothing shows while the host is not running", "[host-tray]") {
  ht::input in;
  auto m = ht::build(in);
  CHECK_FALSE(m.shown);
  CHECK(m.rows.empty());
  CHECK_FALSE(m.login);
  // Its start at login is on: no icon, but the app's login item is wanted, so
  // the app is there to show one when the host starts.
  in.at_login = true;
  m = ht::build(in);
  CHECK_FALSE(m.shown);
  CHECK(m.login);
  // A state.json a crash left, with no lock held: still nothing.
  in.state = serving();
  CHECK_FALSE(ht::build(in).shown);
  // Running but state.json unreadable: nothing to say yet.
  in.running = true;
  in.state.reset();
  CHECK_FALSE(ht::build(in).shown);
}

TEST_CASE("host tray: serving, with the DuckDNS name and the phones connected", "[host-tray]") {
  auto s = serving(3);
  auto m = ht::build(running(s));
  REQUIRE(m.shown);
  CHECK(m.what == ht::status::serving);
  CHECK(m.label == "Hosting as home.duckdns.org");
  CHECK(has(m, "Serving phones"));
  CHECK(has(m, "All 3 folders online"));
  CHECK(has(m, "No phones connected"));
  REQUIRE(button(m, "pause"));
  CHECK(button(m, "pause")->label == "Pause hosting");
  REQUIRE(button(m, "quit"));
  CHECK(button(m, "quit")->label == "Quit hosting");
  CHECK_FALSE(button(m, "resume"));
  CHECK(m.rows.back().id == "quit");

  // No DuckDNS name.
  CHECK(ht::build(running(s, "")).label == "Hosting from this computer");

  s.phones = {"Ann's iPhone"};
  CHECK(has(ht::build(running(s)), "Ann's iPhone connected"));
  s.phones = {"A", "B", "C", "D", "E", "F"};
  m = ht::build(running(s));
  CHECK(has(m, "6 phones connected"));
  for (const char* n : {"A", "B", "C", "D"}) CHECK(has(m, n));
  CHECK_FALSE(has(m, "E"));
  CHECK(has(m, "and 2 more"));
  // A long name is cut where a character starts.
  s.phones = {std::string(70, 'x') + "\xc3\xa9"};
  m = ht::build(running(s));
  bool cut = false;
  for (const auto& l : labels(m)) cut = cut || (l.size() < 80 && l.find("\xe2\x80\xa6") != std::string::npos);
  CHECK(cut);
}

TEST_CASE("host tray: paused, a problem, nothing shared", "[host-tray]") {
  auto s = serving();
  s.paused = true;
  s.service_port = 0;
  s.relay_port = 0;
  s.phones = {"Ann's iPhone"};
  auto m = ht::build(running(s));
  REQUIRE(m.shown);
  CHECK(m.what == ht::status::paused);
  CHECK(has(m, "Paused: phones can’t reach this computer"));
  CHECK(button(m, "resume"));
  CHECK_FALSE(button(m, "pause"));
  // Paused says nothing of ports or phones.
  CHECK_FALSE(has(m, "Not serving: its port is taken"));
  CHECK_FALSE(has(m, "Ann's iPhone connected"));

  s = serving();
  s.service_port = 0;
  m = ht::build(running(s));
  CHECK(m.what == ht::status::problem);
  CHECK(has(m, "Not serving: its port is taken"));
  s = serving();
  s.relay_port = 0;
  CHECK(ht::build(running(s)).what == ht::status::problem);
  s = serving(3);
  s.offline = 1;
  m = ht::build(running(s));
  CHECK(m.what == ht::status::problem);
  CHECK(has(m, "1 of 3 folders offline"));
  s = serving(1);
  CHECK(has(ht::build(running(s)), "Its folder is online"));

  s = serving(0);
  m = ht::build(running(s));
  CHECK(m.what == ht::status::nothing_shared);
  CHECK(has(m, "Nothing shared yet"));
  CHECK(button(m, "quit"));
}

TEST_CASE("host tray: combined drives' health, once state.json has it", "[host-tray]") {
  // The hook issue #686 §3 fills: state.json "drives" [{"label","health"}].
  const auto st = sh::parse_state(
      R"({"service_port":7878,"relay_port":47614,"folders":1,"offline":0,"paused":false,)"
      R"("phones":["Ann's iPhone",""],"drives":[{"label":"Mirror","health":"degraded"},{"health":"x"}],"keepers":[]})");
  REQUIRE(st);
  CHECK(st->phones == std::vector<std::string>{"Ann's iPhone"});
  REQUIRE(st->drives.size() == 1);
  auto m = ht::build(running(*st));
  CHECK(has(m, "Mirror: lost a drive"));
  CHECK(m.what == ht::status::problem);
  auto s = *st;
  s.drives = {{"Mirror", "healthy"}, {"Spare", "rebuilding"}, {"Third", "failed"}};
  m = ht::build(running(s));
  CHECK(has(m, "Mirror: healthy"));
  CHECK(has(m, "Spare: rebuilding"));
  CHECK_FALSE(has(m, "Third: failed"));  // two lines at most
  CHECK(m.what == ht::status::serving);
}

TEST_CASE("host tray: the menu fits the registry's limit and never says a path or port", "[host-tray]") {
  auto s = serving(9);
  s.offline = 2;
  s.service_port = 0;
  s.relay_port = 0;
  s.tls_failed = true;
  s.drives = {{"A", "healthy"}, {"B", "degraded"}};
  s.phones = {"P1", "P2", "P3", "P4", "P5"};
  const auto m = ht::build(running(s));
  CHECK(m.rows.size() <= ht::kMaxRows);
  CHECK(button(m, "pause"));
  CHECK(button(m, "quit"));
  for (const auto& l : labels(m)) {
    CHECK(l.find("7878") == std::string::npos);
    CHECK(l.find('/') == std::string::npos);
    CHECK(l.find('\\') == std::string::npos);
  }
  // keep_menu takes it as it is.
  mv::addon::slot_registry r;
  mv::addon::slot_registry::shell_keep k;
  k.kept = true;
  k.screen = ht::kScreen;
  k.title = "Phone access settings";
  k.label = m.label;
  for (const auto& row : m.rows) k.menu.push_back({row.kind, row.id, row.label, false});
  CHECK(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
  const auto doc = mv::json::parse(ht::menu_json(m), 4);
  REQUIRE(doc);
  REQUIRE(doc->find("items"));
  CHECK(doc->find("items")->a.size() == m.rows.size());
}

TEST_CASE("host tray: the registry's shell keep is drawn, keeps the process and takes the picks", "[host-tray]") {
  mv::addon::slot_registry r;
  std::atomic<int> changes{0};
  r.set_listener([&](std::uint32_t bits) {
    if (bits & mv::addon::slot_changed_notices) ++changes;
  });
  CHECK_FALSE(r.keeping_alive());
  CHECK_FALSE(r.wants_login_item());

  std::mutex m;
  std::vector<std::string> picked;
  mv::addon::slot_registry::shell_keep k;
  k.kept = true;
  k.login = true;
  k.screen = ht::kScreen;
  k.title = "Phone access settings";
  k.label = "Hosting as home.duckdns.org";
  k.menu = {{"text", "", "Serving phones", false}, {"separator", "", "", false}, {"button", "pause", "Pause hosting", false}};
  k.on_menu = [&](const std::string& id, const std::string& value) {
    std::lock_guard lock(m);
    picked.push_back(id + "=" + value);
  };
  REQUIRE(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
  CHECK(changes.load() == 1);
  CHECK(r.keeping_alive());
  CHECK(r.wants_login_item());
  // The same again: nothing for the hosts to redraw.
  REQUIRE(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
  CHECK(changes.load() == 1);

  const auto doc = mv::json::parse(r.notices_json(), 8);
  REQUIRE(doc);
  CHECK(doc->boolean("login").value_or(false));
  const auto* keep = doc->find("keep");
  REQUIRE(keep);
  REQUIRE(keep->a.size() == 1);
  const auto& e = keep->a[0];
  CHECK(*e.str("addon") == ht::kKeepId);
  CHECK(*e.str("screen") == ht::kScreen);
  CHECK(*e.str("title") == "Phone access settings");
  CHECK(e.boolean("shell").value_or(false));
  REQUIRE(e.find("menu"));
  CHECK(e.find("menu")->a.size() == 3);

  CHECK(r.menu_action(ht::kKeepId, "pause", "") == mv::status::ok);
  CHECK(r.menu_action(ht::kKeepId, "nope", "") == mv::status::not_found);
  {
    std::lock_guard lock(m);
    CHECK(picked == std::vector<std::string>{"pause=null"});
  }

  // Not running but starting at login: no entry, nothing kept, the login item still wanted.
  k.kept = false;
  REQUIRE(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
  CHECK_FALSE(r.keeping_alive());
  CHECK(r.wants_login_item());
  CHECK(mv::json::parse(r.notices_json(), 8)->find("keep")->a.empty());
  CHECK(r.menu_action(ht::kKeepId, "pause", "") == mv::status::not_found);

  REQUIRE(r.set_shell_keep(ht::kKeepId, std::nullopt) == mv::status::ok);
  CHECK_FALSE(r.wants_login_item());
  CHECK_FALSE(mv::json::parse(r.notices_json(), 8)->boolean("login").value_or(true));

  // A bad row is refused, as keep_menu refuses one.
  k.kept = true;
  k.menu = {{"files", "f", "Files", false}};
  CHECK(r.set_shell_keep(ht::kKeepId, k) == mv::status::invalid_arg);
  k.menu = {{"button", "Bad Id", "x", false}};
  CHECK(r.set_shell_keep(ht::kKeepId, k) == mv::status::invalid_arg);
  CHECK(r.set_shell_keep("", k) == mv::status::invalid_arg);
}

TEST_CASE("host tray: the watcher follows the host's lock and state.json, and runs the verbs", "[host-tray]") {
  scratch_dir s("host_tray_watch");
  const fs::path host = s / "data/host";
  fs::create_directories(host);
  const fs::path entry = s / "login-entry.plist";

  std::mutex m;
  std::vector<ht::model> seen;
  std::vector<std::string> ran;
  ht::options o;
  o.where = [&] {
    sh::where w;
    w.data_dir = utf8(s / "data");
    w.login_entry = utf8(entry);
    return w;
  };
  o.poll_ms = 50;
  o.login_every_ms = 50;
  o.run = [&](const sh::where&, const std::string& verb) {
    std::lock_guard lock(m);
    ran.push_back(verb);
    return true;
  };
  o.changed = [&](const ht::model& mod) {
    std::lock_guard lock(m);
    seen.push_back(mod);
  };
  const auto last = [&] {
    std::lock_guard lock(m);
    return seen.empty() ? ht::model{} : seen.back();
  };
  const auto count = [&] {
    std::lock_guard lock(m);
    return seen.size();
  };
  auto w = ht::watcher::start(o);
  // The first look is always told, even with nothing there.
  REQUIRE(eventually([&] { return count() == 1; }));
  CHECK_FALSE(last().shown);

  // Unchanged files: nothing more is told.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(count() == 1);

  // A running host: its lock held, its state written.
  auto lock = net::try_lock_instance(utf8(host / "host.lock"));
  REQUIRE(lock);
  write_text(host / "host.json", R"({"relay_port":47614,"vpn":false,"stay_awake":true,"ddns":"home"})");
  write_text(host / "state.json",
             R"({"pid":1,"service_port":7878,"relay_port":47614,"folders":1,"offline":0,"phones":["Ann's iPhone"],"keepers":[]})");
  REQUIRE(eventually([&] { return last().shown; }));
  CHECK(last().label == "Hosting as home.duckdns.org");
  CHECK(has(last(), "Ann's iPhone connected"));
  CHECK(w->current() == last());

  // Paused: state.json says so on the host's next write.
  write_text(host / "state.json",
             R"({"pid":1,"service_port":0,"relay_port":0,"folders":1,"offline":0,"paused":true,"keepers":[]})");
  REQUIRE(eventually([&] { return last().what == ht::status::paused; }));

  // A pick runs the verb on the watcher's thread; a made-up one never does.
  w->pick("resume");
  w->pick("rm -rf");
  REQUIRE(eventually([&] {
    std::lock_guard g(m);
    return !ran.empty();
  }));
  {
    std::lock_guard g(m);
    CHECK(ran == std::vector<std::string>{"resume"});
  }

  // The start-at-login entry appears.
  write_text(entry, "<plist/>");
  REQUIRE(eventually([&] { return last().login; }));

  // The host ends: its lock is released (state.json left as a crash would leave it).
  lock.reset();
  REQUIRE(eventually([&] { return !last().shown; }));
  CHECK(last().login);
  w->stop();
}

TEST_CASE("host tray: host pause, resume and quit, taken by the running service", "[host-tray]") {
  REQUIRE(sodium_init() >= 0);
  {
    auto p = parse({"host", "pause"});
    CHECK(p.error.empty());
    CHECK(p.a.host == cli::host_action::pause);
    p = parse({"host", "resume"});
    CHECK(p.a.host == cli::host_action::resume);
    p = parse({"host", "quit"});
    CHECK(p.a.host == cli::host_action::quit);
    CHECK_FALSE(parse({"host", "pause", "now"}).error.empty());
  }
  {
    cli::child_process::ended e;
    e.code = cli::kHostChildQuit;
    CHECK(cli::decide_after_child(e, false) == cli::after_child::stop);
    e.crashed = true;  // a crash that happened to end 4 is still a crash
    CHECK(cli::decide_after_child(e, false) == cli::after_child::restart);
  }
  {
    cli::host_settings hs;
    hs.paused = true;
    hs.quit_stamp = 1234;
    const auto back = cli::parse_host_settings(cli::host_settings_json(hs));
    REQUIRE(back);
    CHECK(back->paused);
    CHECK(back->quit_stamp == 1234);
    CHECK_FALSE(cli::parse_host_settings(R"({"paused":"yes"})"));
  }

  scratch_dir s("host_tray_service");
  const fs::path data = s / "data";
  fs::create_directories(s / "Photos");
  const cli::host_layout l = cli::host_layout_of(utf8(data));
  REQUIRE(cli::make_host_folders(l));
  {
    serve::config c;
    c.port = 0;
    c.name = "Test PC";
    serve::root_config r;
    r.label = "Photos";
    r.path = utf8(s / "Photos");
    r.id = "0123456789abcdef";
    c.roots.push_back(r);
    write_text(l.serve_config, serve::config_json(c));
  }
  cli::host_service_options o;
  o.where = l;
  o.loopback_only = true;
  o.any_port = true;
  o.config_every_ms = 50;
  o.roots_every_ms = 100000;
  o.retry_ms = 200;
  o.power = false;
  o.present = [](const serve::root_config&) { return true; };
  o.locate = [](const serve::root_config& r) -> std::optional<std::string> { return r.path; };
  std::string why;
  auto hs = cli::host_service::start(o, why);
  REQUIRE(hs);
  REQUIRE(eventually([&] { return hs->snapshot().service_port != 0 && hs->snapshot().relay_port != 0; }));
  CHECK_FALSE(hs->quit_asked());

  // The verbs as a person types them: `host pause` with this data folder.
  const auto verb = [&](cli::host_action act) {
    cli::args a;
    a.cmd = cli::command::host;
    a.host = act;
    a.data = utf8(data);
    return cli::run_host(a, true, "0.0.0");
  };
  REQUIRE(verb(cli::host_action::pause) == 0);
  REQUIRE(eventually([&] {
    const auto snap = hs->snapshot();
    return snap.paused && snap.service_port == 0 && snap.relay_port == 0;
  }));
  REQUIRE(eventually([&] {
    const auto st = sh::parse_state(read_text(l.state));
    return st && st->paused;
  }));
  // Paused, the retries leave it so.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  hs->recheck_now();
  CHECK(hs->snapshot().service_port == 0);
  CHECK(hs->snapshot().relay_port == 0);

  REQUIRE(verb(cli::host_action::resume) == 0);
  REQUIRE(eventually([&] {
    const auto snap = hs->snapshot();
    return !snap.paused && snap.service_port != 0 && snap.relay_port != 0;
  }));
  CHECK(cli::parse_host_settings(read_text(l.settings))->paused == false);

  // `host quit` stamps host.json; this service started before it, so it is asked.
  // (The verb itself waits for the lock, which only the supervisor holds: no
  // supervisor here, so the stamp is written directly, as the verb does.)
  cli::host_settings q = *cli::parse_host_settings(read_text(l.settings));
  q.quit_stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
  write_text(l.settings, cli::host_settings_json(q));
  REQUIRE(eventually([&] { return hs->quit_asked(); }));
  hs.reset();  // its serve.lock with it

  // A service started after that stamp is not asked.
  auto again = cli::host_service::start(o, why);
  REQUIRE(again);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  again->recheck_now();
  CHECK_FALSE(again->quit_asked());
  again->stop();
}
