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
#include "addons/driftwire/cli/host_usage.h"
#include "core/json.h"
#include "import_fixture.h"
#include "addons/phone/host_tray.h"
#include "addons/phone/self_host.h"
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

sh::running_state::client phone(std::string name, std::string via = "home") {
  sh::running_state::client c;
  c.name = std::move(name);
  c.via = std::move(via);
  c.since = 1000;
  return c;
}

std::string fake_clock(std::int64_t) { return "14:02"; }

ht::input running_at(sh::running_state s, std::string duck = "home") {
  auto in = running(std::move(s), std::move(duck));
  in.clock = fake_clock;
  return in;
}

const ht::row* toggle(const ht::model& m) {
  for (const auto& r : m.rows) {
    if (r.kind == "toggle" && r.id == "hosting") return &r;
  }
  return nullptr;
}

TEST_CASE("host tray: serving, with the DuckDNS name, the Hosting toggle and the phones connected", "[host-tray]") {
  auto s = serving(3);
  auto m = ht::build(running_at(s));
  REQUIRE(m.shown);
  CHECK(m.what == ht::status::serving);
  CHECK(m.label == "Hosting as home.duckdns.org");
  // The toggle first, on while serving.
  REQUIRE(toggle(m));
  CHECK(m.rows.front().id == "hosting");
  CHECK(toggle(m)->label == "Hosting");
  CHECK(toggle(m)->value);
  CHECK(has(m, "Serving phones"));
  CHECK(has(m, "All 3 folders online"));
  CHECK(has(m, "No phones connected"));
  REQUIRE(button(m, "quit"));
  CHECK(button(m, "quit")->label == "Quit hosting");
  CHECK(m.rows.back().id == "quit");
  CHECK_FALSE(has(m, "Today: 0 B read, 0 B written"));  // nothing moved: no usage line

  CHECK(ht::build(running_at(s, "")).label == "Hosting from this computer");

  // Each phone: its paired name, how, since when, its bytes today, reading or writing now.
  auto a = phone("Ann's iPhone", "home");
  a.read = 1'200'000'000;
  a.written = 3'000'000;
  a.read_rate = 4'000'000;
  auto b = phone("Bob's iPad", "relay");
  b.write_rate = 10;
  auto c = phone("Cat's Pixel", "internet");
  s.clients = {a, b, c};
  s.day = "2026-11-14";
  s.read_today = 3'400'000'000;
  s.written_today = 120'000'000;
  s.read_rate = 12'000'000;
  m = ht::build(running_at(s));
  CHECK(has(m, "Ann's iPhone · home network since 14:02 · 1.2 GB read, 3.0 MB written · reading"));
  CHECK(has(m, "Bob's iPad · through your relay since 14:02 · writing"));
  CHECK(has(m, "Cat's Pixel · over the internet since 14:02"));
  CHECK(has(m, "Today: 3.4 GB read, 120 MB written · reading 12 MB/s"));
  s.write_rate = 2'000'000;
  CHECK(has(ht::build(running_at(s)), "Today: 3.4 GB read, 120 MB written · now 12 MB/s and 2.0 MB/s"));

  s.clients.clear();
  for (const char* n : {"A", "B", "C", "D", "E", "F"}) s.clients.push_back(phone(n));
  m = ht::build(running_at(s));
  CHECK(has(m, "A · home network since 14:02"));
  CHECK(has(m, "D · home network since 14:02"));
  CHECK_FALSE(has(m, "E · home network since 14:02"));
  CHECK(has(m, "and 2 more phones"));
  // A long name is cut where a character starts.
  s.clients = {phone(std::string(70, 'x') + "\xc3\xa9")};
  m = ht::build(running_at(s));
  bool cut = false;
  for (const auto& l : labels(m)) cut = cut || l.find("\xe2\x80\xa6") != std::string::npos;
  CHECK(cut);
}

TEST_CASE("host tray: bytes in words", "[host-tray]") {
  CHECK(ht::bytes_words(0) == "0 B");
  CHECK(ht::bytes_words(999) == "999 B");
  CHECK(ht::bytes_words(1500) == "1.5 KB");
  CHECK(ht::bytes_words(12'000'000) == "12 MB");
  CHECK(ht::bytes_words(3'400'000'000) == "3.4 GB");
}

TEST_CASE("host tray: off, a problem, nothing shared", "[host-tray]") {
  auto s = serving();
  s.paused = true;
  s.service_port = 0;
  s.relay_port = 0;
  s.clients = {phone("Ann's iPhone")};
  auto m = ht::build(running_at(s));
  REQUIRE(m.shown);
  CHECK(m.what == ht::status::paused);
  REQUIRE(toggle(m));
  CHECK_FALSE(toggle(m)->value);
  CHECK(has(m, "Off: phones can’t reach this computer"));
  CHECK(button(m, "quit"));
  // Off says nothing of ports or phones.
  CHECK_FALSE(has(m, "Not serving: its port is taken"));
  CHECK_FALSE(has(m, "No phones connected"));

  s = serving();
  s.service_port = 0;
  m = ht::build(running_at(s));
  CHECK(m.what == ht::status::problem);
  CHECK(has(m, "Not serving: its port is taken"));
  s = serving();
  s.relay_port = 0;
  CHECK(ht::build(running_at(s)).what == ht::status::problem);
  s = serving(3);
  s.offline = 1;
  m = ht::build(running_at(s));
  CHECK(m.what == ht::status::problem);
  CHECK(has(m, "1 of 3 folders offline"));
  s = serving(1);
  CHECK(has(ht::build(running_at(s)), "Its folder is online"));

  s = serving(0);
  m = ht::build(running_at(s));
  CHECK(m.what == ht::status::nothing_shared);
  CHECK(has(m, "Nothing shared yet"));
  CHECK(button(m, "quit"));
}

TEST_CASE("host tray: state.json's clients, usage and combined drives", "[host-tray]") {
  // PR #692's drives keeper: its line in "keepers", the worst set in "drives_health".
  const auto st = sh::parse_state(
      R"({"service_port":7878,"relay_port":47614,"folders":1,"offline":0,"paused":false,)"
      R"("clients":[{"name":"Ann's iPhone","via":"relay","since":5,"read":10,"written":2,"read_rate":3,"write_rate":0},)"
      R"({"name":""},{"name":"Odd","via":"carrier pigeon"}],)"
      R"("usage":{"day":"2026-11-14","read":100,"written":20,"read_rate":7,"write_rate":1,)"
      R"("devices":[{"name":"Ann's iPhone","read":100,"written":20}]},)"
      R"("drives_sets":2,"drives_health":2,)"
      R"("keepers":["drives      Mirror (mirror): a drive is missing or failed; replace it: driftwire host drives replace \"Mirror\"\n            Spare (span): healthy"]})");
  REQUIRE(st);
  REQUIRE(st->clients.size() == 2);
  CHECK(st->clients[0].name == "Ann's iPhone");
  CHECK(st->clients[0].via == "relay");
  CHECK(st->clients[0].since == 5);
  CHECK(st->clients[0].read == 10);
  CHECK(st->clients[0].read_rate == 3);
  CHECK(st->clients[1].via == "internet");  // anything else is the internet
  CHECK(st->day == "2026-11-14");
  CHECK(st->read_today == 100);
  CHECK(st->written_today == 20);
  CHECK(st->read_rate == 7);
  REQUIRE(st->devices_today.size() == 1);
  CHECK(st->devices_today[0].written == 20);
  CHECK(st->drives_health == 2);
  REQUIRE(st->drive_lines.size() == 2);
  auto m = ht::build(running_at(*st));
  CHECK(has(m, "Mirror (mirror): a drive is missing or failed"));  // the advice after ';' is the card's
  CHECK(has(m, "Spare (span): healthy"));
  CHECK(m.what == ht::status::problem);
  auto s = *st;
  s.drives_health = 1;  // rebuilding: not a problem
  s.drive_lines = {"A (mirror): healthy", "B (mirror): rebuilding, 40% done", "C (span): healthy"};
  m = ht::build(running_at(s));
  CHECK(has(m, "B (mirror): rebuilding, 40% done"));
  CHECK_FALSE(has(m, "C (span): healthy"));  // two lines at most
  CHECK(m.what == ht::status::serving);
}

TEST_CASE("host tray: the menu fits the registry's limit and never says a path or port", "[host-tray]") {
  auto s = serving(9);
  s.offline = 2;
  s.service_port = 0;
  s.relay_port = 0;
  s.tls_failed = true;
  s.drive_lines = {"A (mirror): healthy", "B (mirror): offline: not all of its drives are there"};
  s.drives_health = 3;
  s.day = "2026-11-14";
  s.read_today = 5;
  for (const char* n : {"P1", "P2", "P3", "P4", "P5"}) s.clients.push_back(phone(n));
  const auto m = ht::build(running_at(s));
  CHECK(m.rows.size() <= ht::kMaxRows);
  CHECK(toggle(m));
  CHECK(button(m, "quit"));
  for (const auto& l : labels(m)) {
    CHECK(l.find("7878") == std::string::npos);
    const bool slash_ok = l.find("/") == std::string::npos || l.find("/s") != std::string::npos;
    CHECK(slash_ok);
    CHECK(l.find('\\') == std::string::npos);
  }
  // The registry takes it as it is.
  mv::addon::slot_registry r;
  mv::addon::slot_registry::shell_keep k;
  k.kept = true;
  k.screen = ht::kScreen;
  k.title = "Phone access settings";
  k.label = m.label;
  for (const auto& row : m.rows) k.menu.push_back({row.kind, row.id, row.label, row.value});
  CHECK(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
  const auto doc = mv::json::parse(ht::menu_json(m), 4);
  REQUIRE(doc);
  REQUIRE(doc->find("items"));
  CHECK(doc->find("items")->a.size() == m.rows.size());
  CHECK(doc->find("items")->a.front().boolean("value").value_or(false));  // the toggle: on (not paused)
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
  o.idle_poll_ms = 50;
  o.gone_grace_ms = 400;
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
             R"({"pid":1,"service_port":7878,"relay_port":47614,"folders":1,"offline":0,)"
             R"("clients":[{"name":"Ann's iPhone","via":"home","since":0}],"keepers":[]})");
  REQUIRE(eventually([&] { return last().shown; }));
  CHECK(last().label == "Hosting as home.duckdns.org");
  CHECK(has(last(), "Ann's iPhone \u00b7 home network"));
  CHECK(w->current() == last());

  // Paused: state.json says so on the host's next write.
  write_text(host / "state.json",
             R"({"pid":1,"service_port":0,"relay_port":0,"folders":1,"offline":0,"paused":true,"keepers":[]})");
  REQUIRE(eventually([&] { return last().what == ht::status::paused; }));

  // A pick runs the verb on the watcher's thread; a made-up one never does.
  w->pick("hosting", "true");  // the toggle turned on
  w->pick("rm -rf");
  w->pick("hosting", "maybe");
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
  // For a grace it stays (an update's restart), saying so; then it goes.
  lock.reset();
  REQUIRE(eventually([&] { return has(last(), "Not running just now: starting again\u2026"); }));
  CHECK(last().shown);
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

TEST_CASE("host tray: today's usage per device, across a service started again and a new day", "[host-tray]") {
  using clock = std::chrono::steady_clock;
  cli::usage_book b;
  serve::server::device_usage a;
  a.device[0] = 1;
  a.read = 1000;
  a.written = 10;
  const auto t0 = clock::now();
  b.take({a}, "2026-11-14", t0);
  CHECK(b.total.read == 1000);
  CHECK(b.total.written == 10);
  CHECK(b.total_rate.read == 0);  // no rate before a second pass
  // 2 s later it has read 4000 more: 2000 B/s.
  a.read = 5000;
  b.take({a}, "2026-11-14", t0 + std::chrono::seconds(2));
  CHECK(b.total.read == 5000);
  CHECK(b.today[a.device].read == 5000);
  CHECK(b.total_rate.read == 2000);
  CHECK(b.rate[a.device].read == 2000);
  // Nothing new: the rate is 0, the totals stay.
  b.take({a}, "2026-11-14", t0 + std::chrono::seconds(4));
  CHECK(b.total_rate.read == 0);
  CHECK(b.rate.empty());
  CHECK(b.total.read == 5000);
  // Paused (no service), then a new service counting from 0: what it reads is added.
  b.take({}, "2026-11-14", t0 + std::chrono::seconds(6));
  a.read = 300;
  a.written = 0;
  b.take({a}, "2026-11-14", t0 + std::chrono::seconds(8));
  CHECK(b.total.read == 5300);
  // Kept and read back (a restart, an update).
  const auto back = cli::usage_book::from_json(b.to_json());
  REQUIRE(back);
  CHECK(back->day == "2026-11-14");
  CHECK(back->total.read == 5300);
  CHECK(back->today.at(a.device).written == 10);
  CHECK(b.to_json().find("Phone") == std::string::npos);  // keys only, no names
  CHECK_FALSE(cli::usage_book::from_json("{}"));
  // A new day starts from 0.
  a.read = 400;
  b.take({a}, "2026-11-15", t0 + std::chrono::seconds(10));
  CHECK(b.day == "2026-11-15");
  CHECK(b.total.read == 100);
  CHECK(cli::local_day(std::chrono::system_clock::now()).size() == 10);
}

TEST_CASE("host tray: every row fits the registry, a long name and every field at once", "[host-tray]") {
  auto s = serving();
  s.day = "2026-11-14";
  s.read_today = 999'000'000'000;
  s.written_today = 999'000'000'000;
  s.read_rate = 999'000'000;
  s.write_rate = 999'000'000;
  auto c = phone(std::string(60, 'n'), "relay");
  c.read = 999'000'000;
  c.written = 999'000'000;
  c.read_rate = c.write_rate = 1;
  s.clients = {c};
  s.drive_lines = {std::string(150, 'd') + " (mirror): healthy"};
  const auto m = ht::build(running_at(s));
  mv::addon::slot_registry r;
  mv::addon::slot_registry::shell_keep k;
  k.kept = true;
  k.screen = ht::kScreen;
  k.title = "Phone access settings";
  k.label = m.label;
  for (const auto& row : m.rows) {
    CHECK(row.label.size() <= 120);
    k.menu.push_back({row.kind, row.id, row.label, row.value});
  }
  CHECK(r.set_shell_keep(ht::kKeepId, k) == mv::status::ok);
}

TEST_CASE("host tray: a service stopping keeps what it counted, the next counts from 0", "[host-tray]") {
  using clock = std::chrono::steady_clock;
  cli::usage_book b;
  serve::server::device_usage a;
  a.device[0] = 7;
  a.read = 100;
  const auto t0 = clock::now();
  b.take({a}, "2026-11-14", t0);
  a.read = 900;  // read since the last pass, then the service stops (a new port, Hosting off)
  b.service_gone({a}, "2026-11-14", t0 + std::chrono::milliseconds(500));
  CHECK(b.total.read == 900);
  // The next service passes the old one's count before the next pass: all of it is new.
  a.read = 1500;
  b.take({a}, "2026-11-14", t0 + std::chrono::seconds(2));
  CHECK(b.total.read == 2400);
}

TEST_CASE("host tray: host status --watch", "[host-tray]") {
  auto p = parse({"host", "status", "--watch"});
  CHECK(p.error.empty());
  CHECK(p.a.host == cli::host_action::status);
  CHECK(p.a.watch);
  CHECK_FALSE(parse({"host", "pause", "--watch"}).error.empty());
  CHECK_FALSE(parse({"host"}).a.watch);
}
