// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host` (docs/plans/self-hosted.md slice 2): the arguments, host.json,
// sharing, the entries that start it at login (their texts, and the files
// written into a home of the test's own: the OS's tools never run), the
// drives a folder is on, the service process in-process on loopback, and the
// supervisor as a program (POSIX): a crash is a restart, an update an exit 75.
// Loopback only: never a real router, never duckdns.org, never the DHT.
#include "catch_compat.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/autostart.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/volume.h"
#include "core/json.h"
#include "import_fixture.h"
#include "wire/net.h"
#include "wire/serve_wire.h"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mv::test;
namespace cli = mv::driftwire::cli;
namespace serve = mv::driftwire::serve;
namespace net = mv::driftwire::net;

namespace {

cli::parsed_args parse(std::vector<std::string> argv) { return cli::parse_args(argv, true); }

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Waits up to `ms` for `pred`.
template <typename F>
bool eventually(F pred, int ms = 10000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return pred();
}

// The service's loopback listener with port 0 is ::1's (the OS picks 127.0.0.1's apart); the relay's 127.0.0.1's.
bool accepts(std::uint16_t port) {
  if (port == 0) return false;
  for (const char* at : {"::1", "127.0.0.1"}) {
    if (net::connect(net::endpoint{at, port}, 2000)) return true;
  }
  return false;
}

std::string share_line(const fs::path& data, const fs::path& folder, const std::string& label, bool backup) {
  cli::args a;
  a.cmd = cli::command::host;
  a.host = cli::host_action::share;
  a.host_path = utf8(folder);
  a.label = label;
  a.backup = backup;
  a.data = utf8(data);
  return std::to_string(cli::run_host(a, true, "0.0.0"));
}

}  // namespace

TEST_CASE("driftwire host: the arguments", "[host]") {
  auto p = parse({"host"});
  REQUIRE(p.error.empty());
  CHECK(p.a.cmd == cli::command::host);
  CHECK(p.a.host == cli::host_action::run);

  p = parse({"host", "share", "/srv/Photos", "--label", "Photos", "--backup"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::share);
  CHECK(p.a.host_path == "/srv/Photos");
  CHECK(p.a.label == "Photos");
  CHECK(p.a.backup);

  p = parse({"host", "unshare", "Photos"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::unshare);
  CHECK(p.a.label == "Photos");

  p = parse({"host", "start-at-login", "on"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::start_at_login);
  CHECK(p.a.on);
  p = parse({"host", "stay-awake", "off"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::stay_awake);
  CHECK_FALSE(p.a.on);
  CHECK(parse({"host", "status"}).a.host == cli::host_action::status);
  CHECK(parse({"host", "child"}).a.host == cli::host_action::child);
  CHECK(parse({"host", "--test-quick"}).a.test_quick);
  CHECK_FALSE(cli::parse_args(std::vector<std::string>{"host", "--test-quick"}, false).error.empty());

  CHECK_FALSE(parse({"host", "share"}).error.empty());
  CHECK_FALSE(parse({"host", "start-at-login"}).error.empty());
  CHECK_FALSE(parse({"host", "stay-awake", "maybe"}).error.empty());
  CHECK(parse({"host", "vpn", "on"}).a.host == cli::host_action::vpn);  // slice 7 (more in [host-setup])
  CHECK(parse({"host", "setup"}).a.host == cli::host_action::setup);    // slice 5
  CHECK_FALSE(parse({"host", "status", "--backup"}).error.empty());
  CHECK_FALSE(parse({"serve", "--label", "x"}).error.empty());
  CHECK_FALSE(parse({"host", "unshare", "A", "--label", "B"}).error.empty());
  CHECK(contains(cli::help_text(), "driftwire host"));
}

TEST_CASE("driftwire host: host.json, and what serve.json keeps of a drive", "[host]") {
  const cli::host_settings d;
  CHECK(d.relay_port == 47614);
  CHECK_FALSE(d.vpn);
  CHECK(d.stay_awake);
  cli::host_settings s;
  s.relay_port = 8443;
  s.vpn = true;
  s.stay_awake = false;
  const auto back = cli::parse_host_settings(cli::host_settings_json(s));
  REQUIRE(back);
  CHECK(back->relay_port == 8443);
  CHECK(back->vpn);
  CHECK_FALSE(back->stay_awake);
  CHECK(cli::parse_host_settings("{}"));
  CHECK_FALSE(cli::parse_host_settings("{\"relay_port\":0}"));
  CHECK_FALSE(cli::parse_host_settings("{\"vpn\":\"yes\"}"));
  CHECK_FALSE(cli::parse_host_settings("[]"));

  serve::config c;
  c.port = 7878;
  c.name = "PC";
  serve::root_config r{"0123456789abcdef", "Photos", "/mnt/photos"};
  r.mount = true;
  c.roots.push_back(r);
  serve::root_config w{"fedcba9876543210", "E", "E:\\Photos"};
  w.volume = "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\";
  c.roots.push_back(w);
  const auto parsed = serve::parse_config(serve::config_json(c));
  REQUIRE(parsed);
  REQUIRE(parsed->roots.size() == 2);
  CHECK(parsed->roots[0].mount);
  CHECK(parsed->roots[0].volume.empty());
  CHECK_FALSE(parsed->roots[1].mount);
  CHECK(parsed->roots[1].volume == w.volume);
  // Neither is ever in what a phone is sent.
  CHECK_FALSE(contains(serve::config_json(serve::config{}), "mount"));
  CHECK_FALSE(serve::parse_config(R"({"port":1,"roots":[{"id":"0123456789abcdef","label":"a","path":"/a","volume":"C:\\"}]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":1,"roots":[{"id":"0123456789abcdef","label":"a","path":"/a","mount":1}]})"));
  // No folders: the host's, never the service's.
  CHECK_FALSE(serve::parse_config(R"({"port":1,"roots":[]})"));
  CHECK(serve::parse_config(R"({"port":1,"roots":[]})", false, true));
}

TEST_CASE("driftwire host: sharing and unsharing folders", "[host]") {
  scratch_dir s("host_share");
  fs::create_directories(s / "data/host");
  fs::create_directories(s / "Photos");
  const std::string host_folder = utf8(s / "data/host");
  CHECK(cli::default_label("/Volumes/Photos/") == "Photos");
  CHECK(cli::default_label("E:\\") == "E");
  CHECK(cli::default_label("D:\\Media\\Pictures") == "Pictures");
  CHECK(cli::default_label("/") == "Computer");

  serve::config c;
  CHECK(cli::share_folder(c, utf8(s / "Photos"), "Photos", false, {}, "0123456789abcdef", host_folder).empty());
  REQUIRE(c.roots.size() == 1);
  CHECK_FALSE(c.roots[0].backup);
  CHECK_FALSE(cli::share_folder(c, utf8(s / "Photos"), "Other", false, {}, "1123456789abcdef", host_folder).empty());
  CHECK_FALSE(cli::share_folder(c, utf8(s / "Elsewhere"), "photos", false, {}, "1123456789abcdef", host_folder).empty());
  // Never the host's own folder, nor one that holds it (its keys would be readable).
  CHECK_FALSE(cli::share_folder(c, utf8(s.root()), "All", false, {}, "2123456789abcdef", host_folder).empty());
  CHECK_FALSE(cli::share_folder(c, utf8(s / "data/host/serve"), "Keys", false, {}, "2123456789abcdef", host_folder).empty());
  CHECK_FALSE(cli::share_folder(c, utf8(s / "Elsewhere"), "", false, {}, "2123456789abcdef", host_folder).empty());
  CHECK_FALSE(cli::share_folder(c, utf8(s / "Elsewhere"), "a\nb", false, {}, "2123456789abcdef", host_folder).empty());
  cli::volume_facts f;
  f.mount = true;
  CHECK(cli::share_folder(c, utf8(s / "Backup"), "Backup", true, f, "3123456789abcdef", host_folder).empty());
  REQUIRE(c.roots.size() == 2);
  CHECK(c.roots[1].backup);
  CHECK(c.roots[1].limit > (1ull << 50));
  CHECK(c.roots[1].reserve == serve::kDefaultReserve);
  CHECK(c.roots[1].mount);

  CHECK_FALSE(cli::unshare_folder(c, "Nope").empty());
  CHECK(cli::unshare_folder(c, "photos").empty());
  REQUIRE(c.roots.size() == 1);
  CHECK(c.roots[0].label == "Backup");
}

TEST_CASE("driftwire host: what starts it at login", "[host]") {
  cli::autostart_command c;
  c.program = "/Users/a b/bin/driftwire";
  c.args = {"host", "--data", "/tmp/x&y"};
  const std::string plist = cli::launch_agent_plist(c);
  CHECK(contains(plist, cli::kAutostartLabel));
  CHECK(contains(plist, "<string>/Users/a b/bin/driftwire</string>"));
  CHECK(contains(plist, "<string>/tmp/x&amp;y</string>"));
  CHECK(contains(plist, "<key>SuccessfulExit</key>\n    <false/>"));
  CHECK(contains(plist, "<key>RunAtLoad</key>"));

  c.args = {"host", "--data", "/home/a/100% \"x\""};
  const std::string unit = cli::systemd_user_unit(c);
  CHECK(contains(unit, "ExecStart=\"/Users/a b/bin/driftwire\" \"host\" \"--data\" \"/home/a/100%% \\\"x\\\"\""));
  CHECK(contains(unit, "Restart=on-failure"));
  CHECK(contains(unit, "RestartSec=30"));
  CHECK(contains(unit, "RestartPreventExitStatus=3"));
  CHECK(contains(unit, "WantedBy=default.target"));
  CHECK(contains(cli::linger_hint("ann"), "sudo loginctl enable-linger ann"));

  c.program = "C:\\Users\\Ann\\AppData\\Local\\Programs\\Driftwire\\driftwire.exe";
  c.args = {"host", "--data", "C:\\My Data\\"};
  const std::string xml = cli::scheduled_task_xml(c, "PC\\Ann");
  CHECK(contains(xml, "<LogonTrigger>"));
  CHECK(contains(xml, "<UserId>PC\\Ann</UserId>"));
  CHECK(contains(xml, "<RunLevel>LeastPrivilege</RunLevel>"));
  CHECK(contains(xml, "<Interval>PT5M</Interval>"));
  CHECK(contains(xml, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));
  CHECK(contains(xml, "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"));
  CHECK(contains(xml, "<Arguments>host --data &quot;C:\\My Data\\\\&quot;</Arguments>"));
  CHECK(cli::windows_command_line({"a", "b c", "d\"e", "", "f\\"}) == "a \"b c\" \"d\\\"e\" \"\" f\\");

  const auto there = [](const std::string& p) { return p == "/opt/homebrew/opt/driftwire/bin/driftwire"; };
  CHECK(cli::stable_program("/opt/homebrew/Cellar/driftwire/0.1.42/bin/driftwire", there) ==
        "/opt/homebrew/opt/driftwire/bin/driftwire");
  CHECK(cli::stable_program("/usr/local/Cellar/driftwire/0.1.42/bin/driftwire", there) ==
        "/usr/local/Cellar/driftwire/0.1.42/bin/driftwire");
  CHECK(cli::stable_program("/usr/bin/driftwire", there) == "/usr/bin/driftwire");

  // The port, in a home of the test's own; the OS's tool never runs.
  scratch_dir s("host_autostart");
  cli::autostart_place where;
  where.home = utf8(s.root());
  where.apply = false;
  CHECK_FALSE(cli::autostart_is_on(where));
  const auto on = cli::set_autostart(true, c, where);
  CHECK(on.ok);
  CHECK(cli::autostart_is_on(where));
#if defined(__APPLE__)
  CHECK(contains(read_text(s / ("Library/LaunchAgents/" + std::string(cli::kAutostartLabel) + ".plist")), "KeepAlive"));
#elif !defined(_WIN32)
  CHECK(contains(read_text(s / ".config/systemd/user/driftwire-host.service"), "ExecStart="));
  CHECK(contains(on.note, "enable-linger"));  // printed, never run
#endif
  CHECK(cli::set_autostart(false, c, where).ok);
  CHECK_FALSE(cli::autostart_is_on(where));
}

TEST_CASE("driftwire host: a folder's drive", "[host]") {
  scratch_dir s("host_volume");
  fs::create_directories(s / "Photos");
  const std::string path = utf8(s / "Photos");
  const cli::volume_facts f = cli::volume_of(path);
  CHECK_FALSE(f.mount);
  serve::root_config r{"0123456789abcdef", "Photos", path};
  CHECK(cli::volume_present(r));
  CHECK(cli::volume_locate(r) == path);
#if !defined(_WIN32)
  // Kept as a mount point, now an ordinary folder (unmounted, left empty): not there.
  r.mount = true;
  CHECK_FALSE(cli::volume_present(r));
  CHECK_FALSE(cli::volume_locate(r));
  // /dev is a file system of its own on Linux and macOS: a mount point.
  CHECK(cli::volume_of("/dev").mount);
  serve::root_config dev{"0123456789abcdef", "dev", "/dev"};
  dev.mount = true;
  CHECK(cli::volume_present(dev));
#else
  CHECK_FALSE(f.volume.empty());
  r.volume = "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\";
  CHECK_FALSE(cli::volume_present(r));
  CHECK_FALSE(cli::volume_locate(r));  // mounted nowhere
#endif
}

TEST_CASE("driftwire host: after the child ends", "[host]") {
  cli::child_process::ended e;
  CHECK(cli::decide_after_child(e, true) == cli::after_child::stop);
  CHECK(cli::decide_after_child(e, false) == cli::after_child::restart);  // ended unasked
  e.crashed = true;
  e.code = 11;
  CHECK(cli::decide_after_child(e, false) == cli::after_child::restart);
  e.crashed = false;
  e.code = cli::kHostChildCannotRun;
  CHECK(cli::decide_after_child(e, false) == cli::after_child::fail);
  e.code = 1;
  CHECK(cli::decide_after_child(e, false) == cli::after_child::restart);
}

TEST_CASE("driftwire host: the service and the relay in one process, on loopback", "[host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_service");
  const fs::path data = s / "data";
  fs::create_directories(data);
  fs::create_directories(s / "Photos");
  fs::create_directories(s / "Backup");
  write_text(s / "Photos/a.jpg", "jpeg");
  const cli::host_layout l = cli::host_layout_of(utf8(data));

  // The folders' drives, as the test says (a drive unplugged, a letter changed).
  std::atomic<bool> backup_there{true};
  std::optional<std::string> moved_to;
  std::mutex moved_m;
  cli::host_service_options o;
  o.where = l;
  o.loopback_only = true;
  o.any_port = true;
  o.config_every_ms = 100;
  o.roots_every_ms = 100000;  // recheck_now() only
  o.retry_ms = 200;
  o.power = false;
  o.present = [&](const serve::root_config& r) { return r.label != "Backup" || backup_there.load(); };
  o.locate = [&](const serve::root_config& r) -> std::optional<std::string> {
    std::lock_guard lock(moved_m);
    if (r.label == "Backup" && moved_to) return *moved_to;
    if (r.label == "Backup" && !backup_there.load()) return std::nullopt;
    return r.path;
  };
  std::string why;
  auto hs = cli::host_service::start(o, why);
  REQUIRE(hs);
  CHECK(why.empty());
  // No folder yet: the relay alone.
  auto snap = hs->snapshot();
  CHECK(snap.service_port == 0);
  CHECK(accepts(snap.relay_port));
  CHECK(snap.folders == 0);
  CHECK_FALSE(snap.vpn);
  CHECK_FALSE(snap.awake);  // never asked in the tests
#if !defined(_WIN32)
  struct stat st {};
  REQUIRE(::stat(l.folder.c_str(), &st) == 0);
  CHECK((st.st_mode & 077) == 0);
#endif

  // `host share` from another terminal, with port 0 (the OS picks; the test build).
  {
    serve::config c;
    c.port = 0;
    c.name = "Test PC";
    write_text(l.serve_config, serve::config_json(c));
  }
  REQUIRE(share_line(data, s / "Photos", "Photos", false) == "0");
  REQUIRE(eventually([&] { return hs->snapshot().service_port != 0; }));
  snap = hs->snapshot();
  CHECK(accepts(snap.service_port));
  CHECK(snap.folders == 1);
  const std::uint16_t first_port = snap.service_port;

  // A second folder, taken by the running service (no restart: the same port).
  REQUIRE(share_line(data, s / "Backup", "Backup", true) == "0");
  REQUIRE(eventually([&] { return hs->snapshot().folders == 2; }));
  CHECK(hs->snapshot().service_port == first_port);

  // state.json: ports and counts, never a path or an address (rule 6).
  REQUIRE(eventually([&] { return contains(read_text(l.state), "\"folders\":2"); }));
  const std::string state = read_text(l.state);
  CHECK(contains(state, "\"service_port\":" + std::to_string(first_port)));
  CHECK_FALSE(contains(state, utf8(s.root())));
  CHECK_FALSE(contains(state, "127.0.0.1"));

  // The drive unplugged: offline at the minute's recheck.
  backup_there.store(false);
  hs->recheck_now();
  CHECK(hs->snapshot().offline == 1);
  // Back under another letter: found there, and written.
  fs::create_directories(s / "Moved");
  {
    std::lock_guard lock(moved_m);
    moved_to = utf8(s / "Moved");
  }
  backup_there.store(true);
  hs->recheck_now();
  CHECK(hs->snapshot().offline == 0);
  {
    const auto c = serve::parse_config(read_text(l.serve_config), true, true);
    REQUIRE(c);
    REQUIRE(c->roots.size() == 2);
    CHECK(c->roots[1].path == utf8(s / "Moved"));
  }

  // VPN on in host.json: the relay starts again with forwarding.
  {
    cli::host_settings hs_on;
    hs_on.vpn = true;
    write_text(l.settings, cli::host_settings_json(hs_on));
  }
  REQUIRE(eventually([&] { return hs->snapshot().vpn; }));
  CHECK(accepts(hs->snapshot().relay_port));

  // A serve.json that is not valid: the folders stay as they were.
  const std::string good = read_text(l.serve_config);
  write_text(l.serve_config, "{not json");
  hs->recheck_now();
  CHECK(hs->snapshot().folders == 2);
  CHECK(accepts(hs->snapshot().service_port));
  write_text(l.serve_config, good);

  // Unsharing every folder stops the service; the relay stays.
  cli::args un;
  un.cmd = cli::command::host;
  un.host = cli::host_action::unshare;
  un.data = utf8(data);
  un.label = "Photos";
  CHECK(cli::run_host(un, true, "0.0.0") == 0);
  un.label = "Backup";
  CHECK(cli::run_host(un, true, "0.0.0") == 0);
  CHECK(cli::run_host(un, true, "0.0.0") == 1);  // not shared any more
  REQUIRE(eventually([&] { return hs->snapshot().service_port == 0; }));
  CHECK(accepts(hs->snapshot().relay_port));
  // One service per folder: a second host there cannot start.
  std::string why2;
  CHECK_FALSE(cli::host_service::start(o, why2));
  CHECK_FALSE(why2.empty());
  CHECK(fs::exists(l.state));  // the running host's state.json is not the failed one's to remove

  hs->stop();
  CHECK_FALSE(fs::exists(l.state));
  CHECK(cli::run_host([&] {
    cli::args st;
    st.cmd = cli::command::host;
    st.host = cli::host_action::status;
    st.data = utf8(data);
    return st;
  }(), true, "0.0.0") == 0);
}

#if !defined(_WIN32)
namespace {

pid_t spawn(const std::string& exe, const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    std::vector<char*> argv{const_cast<char*>(exe.c_str())};
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(exe.c_str(), argv.data());
    ::_exit(127);
  }
  return pid;
}

std::optional<long> child_pid(const cli::host_layout& l) {
  std::ifstream f(l.state, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto doc = mv::json::parse(text);
  if (!doc) return std::nullopt;
  const auto pid = doc->integer("pid");
  if (!pid || *pid <= 0) return std::nullopt;
  return static_cast<long>(*pid);
}

std::optional<int> exited_within(pid_t pid, int ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    int st = 0;
    if (::waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("driftwire host: the supervisor starts a crashed service again, and ends 75 for an update",
          "[host]") {
  scratch_dir s("host_supervisor");
  // A copy of the test build of its own, so the test can "update" it.
  const fs::path exe = s / "driftwire";
  fs::copy_file(MV_DRIFTWIRE_CLI_TEST_PATH, exe);
  fs::permissions(exe, fs::perms::owner_all);
  const fs::path data = s / "data";
  const cli::host_layout l = cli::host_layout_of(utf8(data));
  const std::vector<std::string> args{"host", "--data", utf8(data), "--test-loopback", "--test-quick"};
  const pid_t sup = spawn(utf8(exe), args);
  REQUIRE(sup > 0);
  std::optional<long> first;
  REQUIRE(eventually([&] { return (first = child_pid(l)).has_value(); }, 20000));
  CHECK(*first != static_cast<long>(sup));  // the services are in a process of their own

  // A second supervisor sees the first and leaves it alone (exit 0).
  const pid_t second = spawn(utf8(exe), args);
  CHECK(exited_within(second, 10000) == 0);

  // A crash: started again (after 1 s in the test build, 30 s in the product).
  REQUIRE(::kill(static_cast<pid_t>(*first), SIGKILL) == 0);
  std::optional<long> next;
  REQUIRE(eventually([&] { return (next = child_pid(l)).has_value() && *next != *first; }, 20000));
  CHECK_FALSE(exited_within(sup, 0).has_value());

  // The program replaced (an update installed): the supervisor stops the
  // service and ends 75, for the start-at-login entry to start the new one.
  fs::last_write_time(exe, fs::last_write_time(exe) + std::chrono::hours(1));
  const auto code = exited_within(sup, 20000);
  REQUIRE(code);
  CHECK(*code == 75);
  // Its service went with it.
  CHECK(eventually([&] { return ::kill(static_cast<pid_t>(*next), 0) != 0; }, 15000));

  // Started again, it stops on SIGTERM with 0, its service too.
  const pid_t again = spawn(utf8(exe), args);
  std::optional<long> third;
  REQUIRE(eventually([&] { return (third = child_pid(l)).has_value() && *third != *next; }, 20000));
  REQUIRE(::kill(again, SIGTERM) == 0);
  CHECK(exited_within(again, 20000) == 0);
  CHECK(eventually([&] { return ::kill(static_cast<pid_t>(*third), 0) != 0; }, 15000));
}
#endif
