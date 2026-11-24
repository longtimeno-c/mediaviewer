// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// phone/self_host.h's pure parts (docs/plans/self-hosted.md slice 6): where the
// app looks for driftwire and its host, read against the command line's own
// constants and rules, so the card and the program never drift apart. The
// end-to-end setup against the test build of the program is in
// test_phone_pairing.cpp ("This computer").
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "addons/driftwire/cli/autostart.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/ddns.h"
#include "core/json.h"
#include "addons/phone/self_host.h"

namespace sh = mv::phone::self_host;
namespace cli = mv::driftwire::cli;

TEST_CASE("phone self host: driftwire's data folder is where the command line keeps it", "[phone-self-host]") {
  sh::env e;
  e.home = "/Users/a";
  CHECK(sh::data_dir_for(sh::os::mac, e) == "/Users/a/Library/Application Support/Driftwire");
  CHECK(sh::data_dir_for(sh::os::linux_os, e) == "/Users/a/.local/state/driftwire");
  e.xdg_state_home = "/s";
  CHECK(sh::data_dir_for(sh::os::linux_os, e) == "/s/driftwire");
  e.xdg_state_home = "relative";  // the XDG rule: only an absolute one counts
  CHECK(sh::data_dir_for(sh::os::linux_os, e) == "/Users/a/.local/state/driftwire");
  sh::env w;
  w.local_app_data = "C:\\Users\\a\\AppData\\Local";
  CHECK(sh::data_dir_for(sh::os::windows, w) == "C:\\Users\\a\\AppData\\Local\\Driftwire");
  CHECK(sh::data_dir_for(sh::os::mac, sh::env{}).empty());
  CHECK(sh::data_dir_for(sh::os::windows, sh::env{}).empty());

  // The files read are the host's own layout.
  const auto l = cli::host_layout_of("/d");
  CHECK(l.serve_config == "/d/host/serve/serve.json");
  CHECK(l.settings == "/d/host/host.json");
  CHECK(l.state == "/d/host/state.json");
  CHECK(l.lock == "/d/host/host.lock");
}

TEST_CASE("phone self host: the installers' places come first, then PATH", "[phone-self-host]") {
  sh::env e;
  e.home = "/Users/a";
  e.path = "/usr/bin:relative:/opt/homebrew/bin:/x/";
  const auto mac = sh::program_candidates(sh::os::mac, e);
  const std::vector<std::string> want = {"/opt/homebrew/bin/driftwire", "/usr/local/bin/driftwire",
                                         "/Users/a/.local/bin/driftwire", "/usr/bin/driftwire", "/x/driftwire"};
  CHECK(mac == want);  // no relative entry, none twice

  sh::env w;
  w.local_app_data = "C:\\L";
  w.path = "C:\\Windows;\"C:\\Program Files\\D\";;C:\\L\\Programs\\Driftwire\\";
  const auto win = sh::program_candidates(sh::os::windows, w);
  const std::vector<std::string> wwant = {"C:\\L\\Programs\\Driftwire\\driftwire.exe", "C:\\Windows\\driftwire.exe",
                                          "C:\\Program Files\\D\\driftwire.exe"};
  CHECK(win == wwant);

  const auto lin = sh::program_candidates(sh::os::linux_os, e);
  REQUIRE(lin.size() >= 3);
  CHECK(lin[0] == "/usr/bin/driftwire");
}

TEST_CASE("phone self host: the start-at-login entry is the one the program writes", "[phone-self-host]") {
  sh::env e;
  e.home = "/Users/a";
  CHECK(sh::login_entry_for(sh::os::mac, e, "") ==
        std::string("/Users/a/Library/LaunchAgents/") + cli::kAutostartLabel + ".plist");
  CHECK(sh::login_entry_for(sh::os::linux_os, e, "") ==
        std::string("/Users/a/.config/systemd/user/") + cli::kAutostartUnit);
  e.xdg_config_home = "/c";
  CHECK(sh::login_entry_for(sh::os::linux_os, e, "") == std::string("/c/systemd/user/") + cli::kAutostartUnit);
  CHECK(sh::login_entry_for(sh::os::windows, e, "C:\\L\\Driftwire\\host") == "C:\\L\\Driftwire\\host\\autostart-task.xml");
}

TEST_CASE("phone self host: state.json and host.json are read as the host writes them", "[phone-self-host]") {
  const auto s = sh::parse_state(
      R"({"pid":42,"service_port":7878,"relay_port":47614,"vpn":false,"awake":true,"folders":2,"offline":1,)"
      R"j("keepers":["router: port 7878 mapped (UPnP)","duckdns: updated 2 min ago"]})j");
  REQUIRE(s);
  CHECK(s->pid == 42);
  CHECK(s->service_port == 7878);
  CHECK(s->relay_port == 47614);
  CHECK(s->awake);
  CHECK(s->folders == 2);
  CHECK(s->offline == 1);
  REQUIRE(s->keepers.size() == 2);
  CHECK(s->keepers[1] == "duckdns: updated 2 min ago");
  // As the host writes them: a terminal column, advice on following lines.
  CHECK(sh::tidy_keeper_line("DuckDNS     home.duckdns.org, updated 3 min ago") == "DuckDNS: home.duckdns.org, updated 3 min ago");
  CHECK(sh::tidy_keeper_line("router      port 7878 not mapped\n            forward TCP 7878 by hand") ==
        "router: port 7878 not mapped\nforward TCP 7878 by hand");
  CHECK(sh::parse_state(R"({"service_port":7878,"service_outside_port":17878})")->service_outside_port == 17878);
  // Converting (docs/plans/phone-clip-convert.md), as the host writes it.
  const auto conv = sh::parse_state(R"({"convert":{"on":true,"available":true,"encoder":"VideoToolbox","busy":true}})");
  REQUIRE(conv);
  CHECK(conv->convert_on);
  CHECK(conv->convert_available);
  CHECK(conv->convert_busy);
  CHECK(conv->convert_encoder == "VideoToolbox");
  CHECK_FALSE(sh::parse_state(R"({"service_port":7878})")->convert_available);
  // Out of range ports are none; a broken file is nothing.
  CHECK(sh::parse_state(R"({"service_port":70000})")->service_port == 0);
  CHECK_FALSE(sh::parse_state("[1]"));
  CHECK_FALSE(sh::parse_state("{"));

  CHECK(sh::parse_duckdns(R"({"relay_port":47614,"ddns":"Home.duckdns.org"})") == "home");
  CHECK(sh::parse_duckdns(R"({"ddns":"a.b"})").empty());
  CHECK(sh::parse_duckdns(R"({"relay_port":1})").empty());

  // The name goes in beside every key the host keeps there, and replaces an older one.
  const std::string next = sh::with_duckdns(
      R"({"relay_port":47614,"vpn":true,"stay_awake":false,"ddns":"old","ddns_stamp":5,"later":{"a":[1,2.5,null]}})", "home", 99);
  const auto d = mv::json::parse(next, 8);
  REQUIRE(d);
  CHECK(*d->str("ddns") == "home");
  CHECK(d->integer("ddns_stamp") == 99);
  CHECK(d->integer("relay_port") == 47614);
  CHECK(d->boolean("vpn") == true);
  CHECK(d->boolean("stay_awake") == false);
  REQUIRE(d->find("later"));
  CHECK(d->find("later")->find("a")->a.size() == 3);
  CHECK(sh::parse_duckdns(next) == "home");
  CHECK(mv::json::parse(sh::with_duckdns("not json", "home", 1), 4)->str("ddns"));
  // The same item the host's DuckDNS keeper reads.
  CHECK(std::string(sh::kTokenSecret) == mv::driftwire::ddns::kTokenSecret);
}

TEST_CASE("phone self host: the VPN is read as `host vpn` writes it, and its relay line found in what it printed",
          "[phone-self-host]") {
  // host.json as the program writes it (host_settings_json): what is asked for.
  cli::host_settings hs;
  CHECK_FALSE(sh::parse_vpn(cli::host_settings_json(hs)).on);
  hs.vpn = true;
  hs.tls_port = 443;
  auto v = sh::parse_vpn(cli::host_settings_json(hs));
  CHECK(v.on);
  CHECK(v.tls_port == 443);
  hs.tls_port = 0;
  CHECK(sh::parse_vpn(cli::host_settings_json(hs)).tls_port == 0);
  CHECK_FALSE(sh::parse_vpn("not json").on);
  // state.json: what the running relay does, and the router's outside port for it.
  const auto st = sh::parse_state(R"({"relay_port":47614,"vpn":true,"tls_failed":true,"relay_outside_port":47615})");
  REQUIRE(st);
  CHECK(st->vpn);
  CHECK(st->tls_failed);
  CHECK(st->relay_outside_port == 47615);

  // `host vpn on`'s own words (host_setup.cpp host_vpn): the indented line.
  const std::string printed =
      "The VPN is on: this computer carries your laptop's web traffic (TCP 80 and 443, and DNS) out to the internet, "
      "never into your home network.\n"
      "On the laptop, in Driftwire's Relay (your own server), put this line once. It holds the relay's key: keep it "
      "to yourself.\n\n  home.duckdns.org:47614#k3yk3yk3yk3yk3yk3y\r\n\n"
      "Then turn on the Tunnel or All traffic (VPN); sites see this home's address.\n";
  CHECK(sh::relay_line_in(printed) == "home.duckdns.org:47614#k3yk3yk3yk3yk3yk3y");
  CHECK(sh::relay_line_in("  [::1]:47614#abc\n") == "[::1]:47614#abc");
  // Nothing else that has a '#' in it.
  CHECK(sh::relay_line_in("The VPN is off: the relay forwards nothing.\n").empty());
  CHECK(sh::relay_line_in("see issue #12 for more\n").empty());
  CHECK(sh::relay_line_in("  home.duckdns.org#key\n").empty());     // no port
  CHECK(sh::relay_line_in("  home.duckdns.org:port#key\n").empty());
  CHECK(sh::relay_line_in("  home:1#\n").empty());                  // no key
  CHECK(sh::relay_line_in("").empty());
}

TEST_CASE("phone self host: a name and a token are read by DuckDNS's rules", "[phone-self-host]") {
  namespace ddns = mv::driftwire::ddns;
  const char* names[] = {"home", " home.duckdns.org ", "HOME.DuckDNS.org.", "a.b", "-x", "x-", "", "home.duckdns.org.uk",
                         "ab_c", "1234567890123456789012345678901234567890123456789012345678901234", ".duckdns.org",
                         "http://home.duckdns.org", "HTTPS://home.duckdns.org/", "https://home.duckdns.org/x",
                         "https://home.duckdns.org:7878", "ftp://home", "https://"};
  for (const char* n : names) {
    INFO(n);
    CHECK(sh::parse_subdomain(n) == ddns::parse_subdomain(n));
  }
  const char* tokens[] = {"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0", " 0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0\n",
                          "0f1e2d3c4b5a69788796a5b4c3d2e1f0", "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1fg", "",
                          "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0&x=1"};
  for (const char* t : tokens) {
    INFO(t);
    CHECK(sh::parse_token(t) == ddns::parse_token(t));
  }
  CHECK(sh::public_route("home", 7878) == "home.duckdns.org:7878");
  CHECK(sh::host_port("192.168.1.4", 7878) == "192.168.1.4:7878");
  CHECK(sh::host_port("fd00::4", 7878) == "[fd00::4]:7878");
}

TEST_CASE("phone self host: the card's folders are checked before anything runs", "[phone-self-host]") {
#if defined(_WIN32)
  const std::string a = "C:\\Photos", b = "D:\\Backup";
#else
  const std::string a = "/Volumes/Photos", b = "/Volumes/Backup";
#endif
  using fc = sh::folder_choice;
  CHECK(sh::check_folders({}) == "no_folders");
  const fc only_backup[] = {{b, "", true}};
  CHECK(sh::check_folders(only_backup) == "no_library");
  const fc relative[] = {{"Photos", "", false}};
  CHECK(sh::check_folders(relative) == "bad_path");
  const fc twice[] = {{a, "", false}, {a, "Again", true}};
  CHECK(sh::check_folders(twice) == "twice");
  const fc good[] = {{b, "", true}, {a, "Photos", false}};
  CHECK(sh::check_folders(good).empty());
  REQUIRE(sh::library_folder(good));
  CHECK(sh::library_folder(good)->path == a);  // the first one phones read, never the backup folder

  const auto args = sh::share_args(a, "Photos", true);
  const std::vector<std::string> want = {"share", a, "--label", "Photos", "--backup"};
  CHECK(args == want);
  const std::vector<std::string> plain = {"share", a};
  CHECK(sh::share_args(a, "", false) == plain);
}

TEST_CASE("phone self host: labels never clash, as the program would refuse them", "[phone-self-host]") {
  CHECK(sh::folder_label("/Volumes/Card/Photos/") == "Photos");
  CHECK(sh::folder_label("E:\\") == "E");
  CHECK(sh::folder_label("e:") == "E");
  CHECK(sh::folder_label("D:\\Pics\\Trip") == "Trip");
  using fc = sh::folder_choice;
  const fc f[] = {{"/a/Photos", "", false}, {"/b/Photos", "", false}, {"/c/x", "Phone backup", true},
                  {"/d/y", "Phone backup", true}};
  const std::string taken[] = {"photos"};
  const auto l = sh::unique_labels(f, taken);
  const std::vector<std::string> want = {"Photos 2", "Photos 3", "Phone backup", "Phone backup 2"};
  CHECK(l == want);
  CHECK_FALSE(sh::answers(0));
}

TEST_CASE("phone self host: a folder is matched as the OS matches it", "[phone-self-host]") {
  CHECK(sh::same_path_key("/tmp/") == sh::same_path_key("/tmp"));
  CHECK(sh::same_path_key("/tmp/./x/..") == sh::same_path_key("/tmp"));
#if defined(_WIN32) || defined(__APPLE__)
  CHECK(sh::same_path_key("/Volumes/Photos") == sh::same_path_key("/volumes/photos"));
#endif
}
