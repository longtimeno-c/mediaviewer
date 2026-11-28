// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Phone access add-on (docs/plans/phone-access-addon.md): the card as a
// view tree (src/addons/phone/screens.*) checked by the core's own vocabulary
// (addon/screen.h), the secure field that never keeps what is typed, Local
// search's catalogue through the host (addon/catalogue_port.h), the shipped
// manifest, and the module through the real loader. Nothing here opens a
// socket; the module runs in a folder of the test's own (MV_PHONE_ACCESS_DIR).
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_addon.h>
#include <sodium.h>

#include <chrono>
#include <mutex>
#include <span>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "addon/catalogue_port.h"
#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/screen.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "addons/phone/link.h"
#include "addons/phone/screens.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::phone::card::screen;

namespace {

constexpr std::uint32_t kClick = MV_SLOT_EVENT_CLICK, kChange = MV_SLOT_EVENT_CHANGE;
constexpr std::int64_t kNow = 1'800'000'000;

std::string quoted(const std::string& s) {
  mv::json::writer w;
  w.string(s);
  return w.take();
}

// The core accepts it whole, as a host would.
void require_valid(const std::string& tree) {
  mv::addon::screen::tree t;
  const auto e = t.set(tree);
  INFO(tree);
  REQUIRE(std::string(mv::addon::screen::error_name(e)) == "ok");
}

const char* kClaimed = R"({"folder":"/Volumes/photo/MediaViewer","library_name":"Home","claimed":true,
  "local":"nas.local:7878","public":"photos.example.com:7878","nas_passphrase":"set","unlocked_s":300,
  "nas_features":["passphrase","backup","relay"],"relay":"relay.example.com:7879","local_search":true,
  "catalogue":{"running":true,"phase":"sending","fraction":0.4},
  "devices":[{"name":"Ann's iPhone","key":"aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44"},
             {"name":"Guest","key":"0011","guest":true,"limited":true,"expires":1800003600}],
  "folders":[{"id":"r1","label":"Catalogue","path":"/volume1/photo/MediaViewer"},
             {"id":"r2","label":"Phone backup","path":"/volume1/backup","backup":true,"limit":1000000000000,
              "reserve":1000000000,"used":5000000000,"indexed":true}],
  "qr":"mediaviewer://pair#abc","qr_library":"Home","qr_fingerprint":"1234 5678","qr_expires":1800000125,
  "qr_for_s":86400})";

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 1000; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

}  // namespace

TEST_CASE("phone access: every state of the card is a tree the core accepts", "[phone-addon]") {
  for (const char* state : {"{}", R"({"starting":true})", R"({"secondary":true})", R"({"driftwire":false})",
                            R"({"error":"refused"})", R"({"folder":"/Volumes/nas/lib"})",
                            R"({"folder":"/Volumes/nas/lib","other_publisher":true,"other_fingerprint":"ab cd",
                                "asking_name":"Studio","asking_key":"k","asking_fingerprint":"ef 01","mismatch":true})",
                            R"({"host":{"installed":true,"set_up":true,"running":true,"service_port":7878,
                                "duckdns":"home","offline":2,"keepers":["DuckDNS: home.duckdns.org"]}})",
                            kClaimed, "not json"}) {
    screen c(mv::phone::card::computer::mac);
    require_valid(c.build(state, kNow));
    // Each part that opens in place stays a tree the core accepts.
    for (const std::string& node : {std::string("setup_open"), std::string("unlock_open"), std::string("change_open"),
                                   std::string("relay_open"), screen::row_id("fo", "r2") + "_limit",
                                   std::string("folder_add_open"), std::string("backup_add_open"),
                                   std::string("restart_open"), std::string("handover_open")}) {
      (void)c.on_event(node, kClick, "", kNow);
      require_valid(c.build(state, kNow));
    }
    for (const char* kind : {"synology", "other", "this"}) {
      (void)c.on_event("setup_kind", kChange, quoted(kind), kNow);
      require_valid(c.build(state, kNow));
    }
  }
}

TEST_CASE("phone access: only a phone with an end date reads as time-limited", "[phone-addon]") {
  // As on 2026-11-23: an iPhone with no limit, a MacBook kept in the grants only because its
  // backup is off (no end date), and a guest with an end.
  const char* state = R"({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":300,
    "nas_features":["passphrase","backup"],
    "devices":[{"name":"iPhone","key":"b50089df00"},
               {"name":"MacBook","key":"9d14160800","limited":true,"expires":0,"backup":false},
               {"name":"Guest","key":"0011","guest":true,"limited":true,"expires":1800003600}]})";
  screen c(mv::phone::card::computer::pc);
  const std::string tree = c.build(state, kNow);
  require_valid(tree);
  CHECK(tree.find("No time limit \xC2\xB7 Key b50089df") != std::string::npos);
  CHECK(tree.find("No backup \xC2\xB7 No time limit \xC2\xB7 Key 9d141608") != std::string::npos);
  CHECK(tree.find("Time limit for iPhone") != std::string::npos);
  CHECK(tree.find("Time limit for MacBook") != std::string::npos);
  CHECK(tree.find("Extend MacBook's access") == std::string::npos);
  CHECK(tree.find("Extend Guest's access") != std::string::npos);
  CHECK(tree.find("Guest \xC2\xB7 Until ") != std::string::npos);
  // The MacBook gets a time limit from the same menu.
  const auto r = c.on_event(screen::row_id("ph", "9d14160800") + "_access", kChange, quoted("604800"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"grant","key":"9d14160800","for_s":604800})");
}

TEST_CASE("phone access: the claimed card shows the code, its countdown, the phones and the folders", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  const std::string tree = c.build(kClaimed, kNow);
  CHECK(c.qr_text() == "mediaviewer://pair#abc");
  CHECK(c.ticking());
  CHECK(tree.find("\"surface\":\"qr\"") != std::string::npos);
  CHECK(tree.find("Good for 2:05") != std::string::npos);
  CHECK(tree.find("Guest code \xC2\xB7 access for 24 hours from when it is scanned") != std::string::npos);
  CHECK(tree.find("Key aa11bb22") != std::string::npos);
  CHECK(tree.find("Guest \xC2\xB7 ") != std::string::npos);
  CHECK(tree.find("Sending to your server") != std::string::npos);
  CHECK(tree.find("Unlocked \xC2\xB7 5 min") != std::string::npos);
  CHECK(tree.find("Indexed by this PC") != std::string::npos);
  CHECK(tree.find("Stop sharing") != std::string::npos);
  // The Catalogue is never offered to stop sharing: one Stop sharing, the backup folder's.
  CHECK(tree.find("\"" + screen::row_id("fo", "r1") + "_stop\"") == std::string::npos);
  CHECK(tree.find("\"" + screen::row_id("fo", "r2") + "_stop\"") != std::string::npos);

  // A row's controls reach the link with that row's ids.
  auto r = c.on_event(screen::row_id("ph", "aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44") + "_remove", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"revoke","key":"aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44"})");
  r = c.on_event(screen::row_id("ph", "0011") + "_access", kChange, quoted("604800"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"grant","key":"0011","for_s":604800})");
  r = c.on_event(screen::row_id("ph", "0011") + "_access", kChange, quoted("unlimited"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"grant","key":"0011","for_s":0})");
  r = c.on_event(screen::row_id("fo", "r2") + "_stop", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"remove_folder","id":"r2"})");
  // A row that is no longer there (removed elsewhere) reaches nothing, never its neighbour.
  CHECK(c.on_event(screen::row_id("ph", "gone") + "_remove", kClick, "", kNow).empty());
  // A server too old for guest codes gets a plain code, whatever was chosen.
  (void)c.on_event("code_for", kChange, quoted("86400"), kNow);
  (void)c.build(R"({"folder":"/x","claimed":true})", kNow);
  r = c.on_event("pair", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"pair","for_s":0})");
  (void)c.build(kClaimed, kNow);
  r = c.on_event("pair_stop", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"pair_stop"})");
}

TEST_CASE("phone access: what is typed in a secure field is sent once and never in a tree", "[phone-addon]") {
  screen c(mv::phone::card::computer::mac);
  const std::string state = R"({"folder":"/Volumes/nas/lib"})";
  (void)c.build(state, kNow);
  const std::string secret = "correct horse battery staple";
  CHECK(c.on_event("local", kChange, quoted("nas.local:7878"), kNow).empty());
  CHECK(c.on_event("pass", kChange, quoted(secret), kNow).empty());
  CHECK(c.on_event("pass_again", kChange, quoted(secret), kNow).empty());
  const std::string before = c.build(state, kNow);
  CHECK(before.find(secret) == std::string::npos);
  CHECK(before.find("\"secure\":true") != std::string::npos);
  require_valid(before);
  auto r = c.on_event("claim_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0].find("\"do\":\"claim\"") != std::string::npos);
  CHECK(r[0].find("\"local\":\"nas.local:7878\"") != std::string::npos);
  CHECK(r[0].find("\"passphrase\":\"" + secret + "\"") != std::string::npos);
  CHECK(r[0].find("\"new_passphrase\":true") != std::string::npos);
  // Sent: a second Claim carries no passphrase.
  r = c.on_event("claim_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0].find("passphrase") == std::string::npos);

  // The DuckDNS token: gone when Settings closes, never shown.
  (void)c.on_event("setup_open", kClick, "", kNow);
  (void)c.on_event("duck_token", kChange, quoted("1a2b3c4d-0000-0000-0000-000000000000"), kNow);
  c.forget_secrets();
  const std::string host_state = R"({"host":{"installed":true}})";
  (void)c.build(host_state, kNow);
  (void)c.on_event("host_add", kChange, quoted("/Users/ann/Pictures"), kNow);
  (void)c.on_event("duck_name", kChange, quoted("home"), kNow);
  r = c.on_event("host_setup", kClick, "", kNow);
  CHECK(r.empty());  // the token is needed with a name, and it was forgotten
  const std::string tree = c.build(host_state, kNow);
  CHECK(tree.find("1a2b3c4d") == std::string::npos);
  CHECK(tree.find("Paste the DuckDNS token too") != std::string::npos);
  (void)c.on_event("duck_token", kChange, quoted("1a2b3c4d-0000-0000-0000-000000000000"), kNow);
  r = c.on_event("host_setup", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"host_setup","folders":[{"path":"/Users/ann/Pictures","backup":false}],)"
                R"("duckdns":"home","duckdns_token":"1a2b3c4d-0000-0000-0000-000000000000"})");
}

TEST_CASE("phone access: the claimed card is in sections, each a heading with its glyph", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  const char* here = R"({"folder":"G:\\FamilyNas","claimed":true,"nas_features":["backup"],
    "host":{"installed":true,"set_up":true,"running":true,"service_port":7878}})";
  const std::string tree = c.build(here, kNow);
  require_valid(tree);
  std::size_t at = 0;
  for (const auto& [id, symbol] : std::vector<std::pair<std::string, std::string>>{
           {"sec_server", "computer"}, {"sec_phones", "phone"}, {"sec_folders", "folder"},
           {"sec_away", "globe"}, {"sec_drives", "drive"}}) {
    const std::size_t h = tree.find("\"id\":\"" + id + "_heading\"");
    INFO(id);
    REQUIRE(h != std::string::npos);
    CHECK(h > at);  // in this order
    at = h;
    CHECK(tree.find("\"symbol\":\"" + symbol + "\"", h) < tree.find("\"id\":\"" + id + "\"", h));
  }
  // Each computer is a user by itself (self-host-users-smb.md §12): no People section until
  // someone made a person's account, and then it follows Drives.
  CHECK(tree.find("sec_users") == std::string::npos);
  const std::string with_people = c.build(
      R"({"folder":"G:\\FamilyNas","claimed":true,"host":{"installed":true,"set_up":true,"running":true},
          "users":[{"name":"anna","role":"view","applied":true}]})", kNow);
  require_valid(with_people);
  REQUIRE(with_people.find("\"id\":\"sec_users_heading\"") != std::string::npos);
  CHECK(with_people.find("\"id\":\"sec_users_heading\"") > with_people.find("\"id\":\"sec_drives_heading\""));
  // The addresses have titles of their own, not only a field.
  CHECK(tree.find("\"id\":\"local_row\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"public_row\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"backup_add_open\",\"label\":\"Add backup folder\xE2\x80\xA6\"") != std::string::npos);
  // The VPN's one line (the VPN add-on has the switch) is in Away from home, once.
  const std::size_t vpn = tree.find("\"id\":\"vpn_d\"");
  REQUIRE(vpn != std::string::npos);
  CHECK(vpn > tree.find("\"id\":\"sec_away_heading\""));
  CHECK(vpn < tree.find("\"id\":\"sec_drives_heading\""));
  CHECK(tree.find("\"id\":\"vpn_d\"", vpn + 1) == std::string::npos);
  CHECK(tree.find("\"id\":\"vpn_on\"") == std::string::npos);
  // A NAS that offers the VPN: the line too, in Away from home.
  const std::string nas_vpn = c.build(R"({"folder":"/volume1/photo","claimed":true,"nas_features":["vpn"]})", kNow);
  require_valid(nas_vpn);
  const std::size_t nv = nas_vpn.find("\"id\":\"vpn_d\"");
  REQUIRE(nv != std::string::npos);
  CHECK(nv > nas_vpn.find("\"id\":\"sec_away_heading\""));
  CHECK(nas_vpn.find("\"id\":\"nas_vpn_on\"") == std::string::npos);
  // Another server (not this computer): no Drives or Users.
  const std::string nas = c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow);
  require_valid(nas);
  CHECK(nas.find("sec_server_heading") != std::string::npos);
  CHECK(nas.find("sec_drives") == std::string::npos);
  CHECK(nas.find("sec_users") == std::string::npos);
  // A server too old for backup folders: no backup button.
  CHECK(nas.find("backup_add_open") == std::string::npos);
}

TEST_CASE("phone access: a text's glyph and the new glyphs (contribution API 16)", "[phone-addon][slots]") {
  REQUIRE(MV_ADDON_CONTRIBUTION_API == 16);
  mv::addon::screen::tree t;
  const auto with = [](const std::string& node) {
    return R"({"type":"stack","id":"root","children":[)" + node + "]}";
  };
  for (const char* g : {"phone", "drive", "lock", "computer", "sync", "folder"}) {
    INFO(g);
    CHECK(t.set(with(R"({"type":"text","id":"h","text":"Phones","style":"heading","symbol":")" + std::string(g) + "\"}")) ==
          mv::addon::screen::error::none);
    CHECK(t.set(with(R"({"type":"button","id":"b","label":"Add","symbol":")" + std::string(g) + "\"}")) ==
          mv::addon::screen::error::none);
  }
  CHECK(t.set(with(R"({"type":"text","id":"h","text":"Phones","symbol":"rocket"})")) == mv::addon::screen::error::bad_prop);
  CHECK(t.set(with(R"({"type":"text","id":"h","text":"Phones","symbol":7})")) == mv::addon::screen::error::bad_prop);
}

TEST_CASE("phone access: the vocabulary's secure field keeps no value (contribution API 15)", "[phone-addon][slots]") {
  REQUIRE(MV_ADDON_CONTRIBUTION_API >= 15);
  mv::addon::screen::tree t;
  const std::string base =
      R"({"type":"stack","id":"root","children":[{"type":"text_field","id":"p","label":"Passphrase","secure":true)";
  CHECK(t.set(base + R"(,"value":"hunter22"}]})") == mv::addon::screen::error::bad_prop);
  CHECK(t.set(base + R"(,"value":"","multiline":true}]})") == mv::addon::screen::error::bad_prop);
  CHECK(t.set(base + R"(,"value":"","pick":"folder"}]})") == mv::addon::screen::error::bad_prop);
  REQUIRE(t.set(base + R"(,"value":""}]})") == mv::addon::screen::error::none);
  CHECK(t.check_event("p", MV_SLOT_EVENT_CHANGE, R"("hunter22")") == mv::addon::screen::error::none);
  CHECK(t.json().find("hunter22") == std::string::npos);
  // A plain field still keeps what was typed (a new tree keeps it).
  REQUIRE(t.set(R"({"type":"stack","id":"root","children":[{"type":"text_field","id":"p","label":"Name"}]})") ==
          mv::addon::screen::error::none);
  CHECK(t.check_event("p", MV_SLOT_EVENT_CHANGE, R"("Ann")") == mv::addon::screen::error::none);
  CHECK(t.json().find("Ann") != std::string::npos);
}

TEST_CASE("phone access: the card's words", "[phone-addon]") {
  CHECK(screen::share_label("/volume1/photo/MediaViewer") == "photo");
  CHECK(screen::share_label("/Volumes/Drive/Video") == "Video");
  CHECK(screen::share_label("/volumes/x") == "x");
  CHECK(screen::gigabytes(1'000'000'000'000) == "1000");
  CHECK(screen::gigabytes(1'500'000'000) == "1.5");
  CHECK(screen::gigabytes(0) == "0");
  CHECK(screen::bytes("1,5") == 1'500'000'000);
  CHECK(screen::bytes(" 500 ") == 500'000'000'000);
  CHECK(screen::bytes("abc") == -1);
  CHECK(screen::bytes("") == -1);
  CHECK(screen::duration_text(3600) == "1 hour");
  CHECK(screen::duration_text(7 * 86400) == "7 days");
  CHECK(screen::relative(kNow - 7200, kNow) == "2 hours ago");
  CHECK(screen::relative(kNow - 86400, kNow) == "yesterday");
  CHECK(screen::relative(kNow + 300, kNow) == "in 5 minutes");
}

TEST_CASE("phone access: this computer's server has Stop while it runs and Start once it has stopped", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  std::string tree = c.build(R"({"host":{"installed":true,"set_up":true,"running":true,"service_port":7878}})", kNow);
  require_valid(tree);
  CHECK(tree.find("\"id\":\"host_stop\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"host_start\"") == std::string::npos);
  auto r = c.on_event("host_stop", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"host_stop"})");

  tree = c.build(R"({"busy":"host_stop","host":{"installed":true,"set_up":true,"running":true}})", kNow);
  require_valid(tree);
  CHECK(tree.find("Stopping the server") != std::string::npos);

  tree = c.build(R"({"host":{"installed":true,"set_up":true,"running":false}})", kNow);
  CHECK(tree.find("\"id\":\"host_stop\"") == std::string::npos);
  CHECK(tree.find("\"id\":\"host_start\"") != std::string::npos);
  CHECK(tree.find("not set to start at login") != std::string::npos);

  tree = c.build(R"({"error":"host_stop","host_said":"schtasks failed","host":{"installed":true,"set_up":true,"running":true}})",
                 kNow);
  CHECK(tree.find("The server didn't stop: schtasks failed") != std::string::npos);
}

// The VPN is the VPN add-on's (docs/plans/vpn-addon.md): the card says where, and
// answers its vpn_* requests with the state and the link's requests.
std::string member(const std::string& json, const char* key) {
  const auto doc = mv::json::parse(json, 2);
  REQUIRE(doc);
  if (const std::string* s = doc->str(key)) return *s;
  if (const auto b = doc->boolean(key)) return *b ? "true" : "false";
  return "-";
}

TEST_CASE("phone access: this computer's VPN, for the VPN add-on: its status, the switch and the code",
          "[phone-addon]") {
  screen c(mv::phone::card::computer::mac);
  // The card: one line, no switch.
  const char* off = R"({"host":{"installed":true,"set_up":true,"running":true,"service_port":7878}})";
  std::string tree = c.build(off, kNow);
  require_valid(tree);
  CHECK(tree.find("Use this Mac as your laptop's VPN in Settings \xE2\x86\x92 Add-ons \xE2\x86\x92 VPN.") !=
        std::string::npos);
  CHECK(tree.find("\"id\":\"vpn_on\"") == std::string::npos);
  CHECK(c.on_event("vpn_on", kChange, "true", kNow).empty());  // an old id does nothing

  // The VPN add-on's Host section.
  std::string st = c.vpn_state(off);
  CHECK(member(st, "where") == "here");
  CHECK(member(st, "title") == "VPN through this Mac");
  CHECK(member(st, "on") == "false");
  CHECK(member(st, "line").find("Never a way into your home network") != std::string::npos);
  CHECK(member(st, "code") == "-");
  CHECK(c.vpn_request(off, "vpn_set", true) == R"({"do":"host_vpn","on":true})");
  CHECK(c.vpn_request(off, "vpn_set", false) == R"({"do":"host_vpn","on":false})");
  CHECK(c.vpn_request(off, "vpn_code", false) == R"({"do":"host_vpn_line"})");
  CHECK(c.vpn_request(off, "vpn_code_hide", false) == R"({"do":"host_vpn_hide"})");
  CHECK(c.vpn_request(off, "something", true).empty());

  CHECK(member(c.vpn_state(R"({"busy":"host_vpn","host":{"installed":true,"set_up":true,"running":true}})"), "busy") ==
        "true");
  CHECK(member(c.vpn_state(R"({"host":{"installed":true,"set_up":true,"running":true,"vpn":true,"vpn_on":false}})"),
               "line") == "Turning on\xE2\x80\xA6");
  CHECK(member(c.vpn_state(R"({"host":{"installed":true,"set_up":true,"running":false,"vpn":true}})"), "line") ==
        "On once the server runs.");
  st = c.vpn_state(R"({"host":{"installed":true,"set_up":true,"running":true,"duckdns":"home","vpn":true,"vpn_on":true,
    "relay_port":47614,"relay_outside_port":47700,"tls_port":443,"tls_failed":true}})");
  CHECK(member(st, "line") == "On \xC2\xB7 relay port 47700 \xC2\xB7 the web-site look couldn't listen on port 443");
  CHECK(member(st, "on") == "true");
  st = c.vpn_state(R"({"relay_line":"[::1]:47614#k3y","relay_code":"mediaviewer://relay#[::1]:47614#k3y","host":{"installed":true,
    "set_up":true,"running":true,"vpn":true,"vpn_on":true,"relay_port":47614}})");
  CHECK(member(st, "code") == "mediaviewer://relay#[::1]:47614#k3y");
  CHECK(member(st, "line").find("Without a DuckDNS name it works on your home network only.") != std::string::npos);
  CHECK(member(c.vpn_state(R"({"error":"host_vpn","host_said":"could not write host.json","host":{"installed":true,"set_up":true}})"),
               "error") == "The VPN wasn't changed: could not write host.json");
  // Nothing to host on: none, and nothing is asked of the link.
  const char* nothing = R"({"claimed":true,"folder":"/volume1/photo"})";
  CHECK(member(c.vpn_state(nothing), "where") == "none");
  CHECK(c.vpn_request(nothing, "vpn_set", true).empty());
  CHECK(c.build(nothing, kNow).find("as your laptop's VPN") == std::string::npos);
}

TEST_CASE("phone access: the VPN's web-site look and its own domain, for the VPN add-on", "[phone-addon][vpn-stealth]") {
  screen c(mv::phone::card::computer::mac);
  // On with the look (on by default): the port the code points at, 443 from outside.
  const char* look = R"({"host":{"installed":true,"set_up":true,"running":true,"duckdns":"home","vpn":true,
    "vpn_on":true,"relay_port":47614,"tls_port":47615,"tls_outside_port":443}})";
  std::string st = c.vpn_state(look);
  CHECK(member(st, "line") == "On \xC2\xB7 looks like a web site on port 443");
  CHECK(member(st, "domain") == "");
  CHECK(member(st, "domain_line").find("which some networks block") != std::string::npos);
  // Its own domain: `host name domain` (or none), for this computer only.
  CHECK(c.vpn_request(look, "vpn_domain", false, "vpn.example.com") ==
        R"({"do":"host_domain","domain":"vpn.example.com"})");
  CHECK(c.vpn_request(look, "vpn_domain", false, "none") == R"({"do":"host_domain","domain":"none"})");
  CHECK(c.vpn_request(look, "vpn_domain", false, "").empty());
  const char* nas = R"({"claimed":true,"folder":"/volume1/photo","nas_features":["vpn"],"nas_vpn_port":47614})";
  CHECK(c.vpn_request(nas, "vpn_domain", false, "vpn.example.com").empty());
  // Set: the two CNAMEs it needs; the line no longer says home network only.
  st = c.vpn_state(R"({"host":{"installed":true,"set_up":true,"running":true,"duckdns":"home","domain":"vpn.example.com",
    "vpn":true,"vpn_on":true,"relay_port":47614,"tls_port":47615}})");
  CHECK(member(st, "domain") == "vpn.example.com");
  CHECK(member(st, "domain_line") == "vpn.example.com: a CNAME to home.duckdns.org, and _acme-challenge.vpn.example.com "
                                     "to _acme-challenge.home.duckdns.org.");
  CHECK(member(c.vpn_state(R"({"host":{"installed":true,"set_up":true,"running":true,"domain":"vpn.example.com","vpn":true,
    "vpn_on":true,"relay_port":47614,"tls_port":47615}})"), "line").find("home network only") == std::string::npos);
  CHECK(member(c.vpn_state(R"({"busy":"host_domain","host":{"installed":true,"set_up":true,"running":true}})"), "busy") ==
        "true");
  CHECK(member(c.vpn_state(R"({"error":"host_domain","host_said":"driftwire host: \"x\" is not a domain (vpn.example.com).",
    "host":{"installed":true,"set_up":true}})"), "error").find("is not a domain") != std::string::npos);
  // The look's code, given as it came.
  CHECK(member(c.vpn_state(R"({"relay_line":"mediaviewer://relay-tls#home.duckdns.org:443#k3y",
    "relay_code":"mediaviewer://relay-tls#home.duckdns.org:443#k3y","host":{"installed":true,"set_up":true,"running":true,
    "vpn":true,"vpn_on":true}})"), "code") == "mediaviewer://relay-tls#home.duckdns.org:443#k3y");
}

TEST_CASE("phone access: the NAS's VPN, for the VPN add-on, when its service offers it", "[phone-addon][nas-vpn]") {
  screen c(mv::phone::card::computer::mac);
  // A NAS from before (no "vpn" feature): nothing.
  const char* before = R"({"claimed":true,"folder":"/volume1/photo","nas_features":["passphrase","relay"]})";
  CHECK(c.build(before, kNow).find("as your laptop's VPN") == std::string::npos);
  CHECK(member(c.vpn_state(before), "where") == "none");
  const char* off = R"({"claimed":true,"folder":"/volume1/photo","nas_features":["relay","vpn"],"nas_vpn_port":0})";
  std::string tree = c.build(off, kNow);
  require_valid(tree);
  CHECK(tree.find("Use your server as your laptop's VPN in Settings") != std::string::npos);
  std::string st = c.vpn_state(off);
  CHECK(member(st, "where") == "server");
  CHECK(member(st, "title") == "VPN through your server");
  CHECK(member(st, "on") == "false");
  CHECK(c.vpn_request(off, "vpn_set", true) == R"({"do":"nas_vpn","on":true})");
  CHECK(member(c.vpn_state(R"({"claimed":true,"folder":"/volume1/photo","nas_features":["vpn"],"nas_vpn_port":47614,"nas_vpn_running":false})"),
               "line") == "On, but your server couldn't listen on port 47614.");
  const char* on = R"({"claimed":true,"folder":"/volume1/photo","nas_features":["vpn"],"nas_vpn_port":47614,"nas_vpn_running":true,
    "relay_code":"mediaviewer://relay#nas.example.com:47614#k3yk3yk3yk3yk3yk3y"})";
  st = c.vpn_state(on);
  CHECK(member(st, "line").rfind("On \xC2\xB7 port 47614. Forward it on your router", 0) == 0);
  CHECK(member(st, "on") == "true");
  CHECK(member(st, "code") == "mediaviewer://relay#nas.example.com:47614#k3yk3yk3yk3yk3yk3y");
  CHECK(c.vpn_request(on, "vpn_code", false) == R"({"do":"nas_vpn","on":true})");  // on again: the key once more
  CHECK(c.vpn_request(on, "vpn_set", false) == R"({"do":"nas_vpn","on":false})");
  CHECK(c.build(on, kNow).find("k3yk3yk3y") == std::string::npos);  // the card never shows the code now
}

TEST_CASE("phone access: this computer's DuckDNS name can be changed after setup", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  const char* claimed = R"({"folder":"G:\\FamilyNas","claimed":true,
    "host":{"installed":true,"set_up":true,"running":true,"service_port":7878}})";
  std::string tree = c.build(claimed, kNow);
  require_valid(tree);
  CHECK(tree.find("None: phones reach this PC at home only") != std::string::npos);
  CHECK(tree.find("\"id\":\"duck_change\",\"label\":\"Add\xE2\x80\xA6\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"duck_name\"") == std::string::npos);

  CHECK(c.on_event("duck_change", kClick, "", kNow).empty());
  tree = c.build(claimed, kNow);
  require_valid(tree);
  CHECK(tree.find("\"id\":\"duck_name\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"duck_save\"") != std::string::npos);
  // Both are needed.
  (void)c.on_event("duck_name", kChange, quoted("home"), kNow);
  CHECK(c.on_event("duck_save", kClick, "", kNow).empty());
  CHECK(c.build(claimed, kNow).find("Paste the DuckDNS token too.") != std::string::npos);
  const std::string token = "1a2b3c4d-0000-0000-0000-000000000000";
  (void)c.on_event("duck_token", kChange, quoted(token), kNow);
  CHECK(c.build(claimed, kNow).find(token) == std::string::npos);
  auto r = c.on_event("duck_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"host_name","duckdns":"home","duckdns_token":"1a2b3c4d-0000-0000-0000-000000000000"})");
  // Sent once: the fields close and a second Save has nothing to send.
  tree = c.build(claimed, kNow);
  CHECK(tree.find("\"id\":\"duck_name\"") == std::string::npos);

  // Pasted from the browser: the name in it is sent.
  CHECK(c.on_event("duck_change", kClick, "", kNow).empty());
  (void)c.on_event("duck_name", kChange, quoted("http://ronan.duckdns.org"), kNow);
  (void)c.on_event("duck_token", kChange, quoted(token), kNow);
  r = c.on_event("duck_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"host_name","duckdns":"ronan","duckdns_token":"1a2b3c4d-0000-0000-0000-000000000000"})");

  // A name or token the server would refuse: nothing sent, the form (and the
  // pasted token) stay, and why is said by the form, not at the foot.
  CHECK(c.on_event("duck_change", kClick, "", kNow).empty());
  (void)c.on_event("duck_name", kChange, quoted("https://ronan.duckdns.org/photos"), kNow);
  (void)c.on_event("duck_token", kChange, quoted(token), kNow);
  CHECK(c.on_event("duck_save", kClick, "", kNow).empty());
  tree = c.build(claimed, kNow);
  require_valid(tree);
  CHECK(tree.find("\"id\":\"duck_name\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"duck_error\",\"text\":\"That isn't a DuckDNS name.") != std::string::npos);
  CHECK(tree.find("\"id\":\"error\"") == std::string::npos);
  // The name put right: the token pasted before goes with it, not pasted again.
  (void)c.on_event("duck_name", kChange, quoted("ronan"), kNow);
  r = c.on_event("duck_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"host_name","duckdns":"ronan","duckdns_token":"1a2b3c4d-0000-0000-0000-000000000000"})");
  CHECK(c.on_event("duck_change", kClick, "", kNow).empty());
  (void)c.on_event("duck_name", kChange, quoted("ronan"), kNow);
  (void)c.on_event("duck_token", kChange, quoted("not-a-token"), kNow);
  CHECK(c.on_event("duck_save", kClick, "", kNow).empty());
  CHECK(c.build(claimed, kNow).find("That isn't a DuckDNS token.") != std::string::npos);
  CHECK(c.on_event("duck_cancel", kClick, "", kNow).empty());

  // DuckDNS refused it: said by the name, once.
  tree = c.build(R"({"folder":"G:\\FamilyNas","claimed":true,"error":"host_name","host_said":"KO",
    "host":{"installed":true,"set_up":true,"running":true}})", kNow);
  CHECK(tree.find("\"id\":\"duck_error\",\"text\":\"DuckDNS: KO\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"error\"") == std::string::npos);

  // Named: Change…, and the name.
  tree = c.build(R"({"folder":"G:\\FamilyNas","claimed":true,
    "host":{"installed":true,"set_up":true,"running":true,"duckdns":"home"}})", kNow);
  CHECK(tree.find("home.duckdns.org: how your phone finds this PC away from home.") != std::string::npos);
  CHECK(tree.find("\"id\":\"duck_change\",\"label\":\"Change\xE2\x80\xA6\"") != std::string::npos);
  // Not this computer's server: no DuckDNS row.
  CHECK(c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow).find("duck_change") == std::string::npos);
}

TEST_CASE("phone access: a folder is shared after setup on a Windows server, picked when it is this computer", "[phone-addon]") {
  CHECK(screen::share_label("G:\\FamilyNas\\Backup") == "Backup");
  CHECK(screen::share_label("D:/Video") == "Video");

  screen c(mv::phone::card::computer::pc);
  const char* here = R"({"folder":"G:\\FamilyNas","claimed":true,"nas_features":["backup"],
    "host":{"installed":true,"set_up":true,"running":true}})";
  std::string tree = c.build(here, kNow);
  require_valid(tree);
  // No backup folder yet: the card says so, and Add backup folder… opens the form set for one.
  CHECK(tree.find("\"id\":\"backup_none\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"new_path\"") == std::string::npos);
  CHECK(c.on_event("backup_add_open", kClick, "", kNow).empty());
  tree = c.build(here, kNow);
  require_valid(tree);
  CHECK(tree.find("\"id\":\"new_backup\",\"label\":\"Phones back up to this folder\",\"value\":true") != std::string::npos);
  CHECK(tree.find("\"id\":\"new_limit\",\"label\":\"Storage limit in gigabytes\",\"value\":\"1000\"") != std::string::npos);
  // The folder is chosen with the OS's picker on this computer.
  CHECK(tree.find("\"id\":\"new_path\",\"label\":\"Folder on this PC to share\"") != std::string::npos);
  CHECK(tree.find("\"pick\":\"folder\"") != std::string::npos);
  (void)c.on_event("new_path", kChange, quoted("G:\\FamilyNas\\Backup"), kNow);
  (void)c.on_event("new_backup", kChange, "true", kNow);
  (void)c.on_event("new_limit", kChange, quoted("500"), kNow);
  (void)c.on_event("new_reserve", kChange, quoted("50"), kNow);
  auto r = c.on_event("new_add", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"add_folder","label":"Backup","path":"G:\\FamilyNas\\Backup","backup":"1",)"
                R"("limit":"500000000000","reserve":"50000000000"})");
  // Sent: the form closes.
  CHECK(c.build(here, kNow).find("\"id\":\"new_path\"") == std::string::npos);

  // A NAS's folder is still typed, and a path the server would refuse is not sent.
  (void)c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow);
  CHECK(c.on_event("folder_add_open", kClick, "", kNow).empty());
  (void)c.on_event("new_path", kChange, quoted("photos"), kNow);
  CHECK(c.on_event("new_add", kClick, "", kNow).empty());
  CHECK(c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow).find("starting with /") != std::string::npos);
  (void)c.on_event("new_path", kChange, quoted("/volume1/Video"), kNow);
  r = c.on_event("new_add", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"add_folder","label":"Video","path":"/volume1/Video"})");
}

TEST_CASE("phone access: a backup folder may have no storage limit, only the free space to keep", "[phone-addon]") {
  // The server's word for none is INT64_MAX (serve_wire.h kNoLimit), what `driftwire host share --backup` writes.
  const char* unlimited = R"({"folder":"/Volumes/nas/lib","claimed":true,"nas_features":["backup"],
    "folders":[{"id":"r2","label":"Phone backup","path":"/volume1/backup","backup":true,
                "limit":9223372036854775807,"reserve":1000000000,"used":5000000000}]})";
  screen c(mv::phone::card::computer::mac);
  std::string tree = c.build(unlimited, kNow);
  require_valid(tree);
  CHECK(tree.find("5 GB used, no limit, keeps 1 GB free") != std::string::npos);
  CHECK(tree.find("9223372036") == std::string::npos);

  // Change limit…: No storage limit is on, and there is no limit to type.
  const std::string row = screen::row_id("fo", "r2");
  CHECK(c.on_event(row + "_limit", kClick, "", kNow).empty());
  tree = c.build(unlimited, kNow);
  require_valid(tree);
  CHECK(tree.find(R"("id":"edit_no_limit","label":"No storage limit","value":true)") != std::string::npos);
  CHECK(tree.find(R"("id":"edit_limit")") == std::string::npos);
  auto r = c.on_event("edit_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"set_backup","id":"r2","limit":"none","reserve":"1000000000"})");
  // Turned off, a limit is typed again (from a suggestion).
  CHECK(c.on_event(row + "_limit", kClick, "", kNow).empty());
  (void)c.on_event("edit_no_limit", kChange, "false", kNow);
  tree = c.build(unlimited, kNow);
  CHECK(tree.find(R"("id":"edit_limit","label":"Storage limit in gigabytes","value":"1000")") != std::string::npos);
  (void)c.on_event("edit_limit", kChange, quoted("200"), kNow);
  r = c.on_event("edit_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"set_backup","id":"r2","limit":"200000000000","reserve":"1000000000"})");

  // A folder with a limit is changed to none.
  screen d(mv::phone::card::computer::mac);
  (void)d.build(kClaimed, kNow);
  CHECK(d.on_event(row + "_limit", kClick, "", kNow).empty());
  tree = d.build(kClaimed, kNow);
  CHECK(tree.find(R"("id":"edit_no_limit","label":"No storage limit","value":false)") != std::string::npos);
  CHECK(tree.find(R"("id":"edit_limit","label":"Storage limit in gigabytes","value":"1000")") != std::string::npos);
  (void)d.on_event("edit_no_limit", kChange, "true", kNow);
  (void)d.on_event("edit_reserve", kChange, quoted(""), kNow);
  CHECK(d.on_event("edit_save", kClick, "", kNow).empty());  // still the free space to keep
  CHECK(d.build(kClaimed, kNow).find("Type the space to keep free, in GB.") != std::string::npos);
  (void)d.on_event("edit_reserve", kChange, quoted("20"), kNow);
  r = d.on_event("edit_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"set_backup","id":"r2","limit":"none","reserve":"20000000000"})");

  // Add backup folder…: No storage limit hides the limit, and the folder goes with none.
  CHECK(c.on_event("backup_add_open", kClick, "", kNow).empty());
  (void)c.on_event("new_no_limit", kChange, "true", kNow);
  tree = c.build(unlimited, kNow);
  require_valid(tree);
  CHECK(tree.find(R"("id":"new_no_limit","label":"No storage limit","value":true)") != std::string::npos);
  CHECK(tree.find(R"("id":"new_limit")") == std::string::npos);
  CHECK(tree.find("until only the free space you set is left") != std::string::npos);
  (void)c.on_event("new_path", kChange, quoted("/volume1/Camera"), kNow);
  r = c.on_event("new_add", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"add_folder","label":"Camera","path":"/volume1/Camera","backup":"1","limit":"none","reserve":"1000000000"})");
}

TEST_CASE("phone access: the link sends a backup folder's No storage limit as the server's none", "[phone-addon]") {
  scratch_dir dir("phone_link_no_limit");
  std::mutex m;
  std::vector<std::pair<std::string, std::string>> sent;
  mv::phone::link::options o;
  o.data_dir = (dir.root() / "phone").string();
  o.route = [&](const char* method, const std::string& json) -> mv::result<std::string> {
    const std::string what(method);
    if (what != "add_root" && what != "set_backup") return mv::err(mv::status::not_found);
    std::lock_guard lock(m);
    sent.emplace_back(what, json);
    return std::string(what == "add_root" ? R"({"id":"r9"})" : "{}");
  };
  mv::phone::link l(std::move(o));
  REQUIRE(l.request(R"({"do":"add_folder","label":"Camera","path":"/volume1/Camera","backup":"1","limit":"none","reserve":"1000000000"})"));
  REQUIRE(l.request(R"({"do":"set_backup","id":"r2","limit":"none","reserve":"0"})"));
  REQUIRE(l.request(R"({"do":"set_backup","id":"r2","limit":"500000000000","reserve":"0"})"));
  // Anything else is still refused here, never sent as a limit of 0.
  REQUIRE(l.request(R"({"do":"set_backup","id":"r2","limit":"unlimited","reserve":"0"})"));
  REQUIRE(l.request(R"({"do":"set_backup","id":"r2","limit":"0","reserve":"0"})"));
  // One more that goes: the requests run in order, so the two before it are done once it is sent.
  REQUIRE(l.request(R"({"do":"set_backup","id":"r2","limit":"7","reserve":"0"})"));
  REQUIRE(eventually([&] {
    std::lock_guard lock(m);
    return sent.size() >= 4;
  }));
  l.stop();
  REQUIRE(sent.size() == 4);
  CHECK(sent[3].second == R"({"id":"r2","limit":7,"reserve":0})");
  CHECK(sent[0].first == "add_root");
  CHECK(sent[0].second ==
        R"({"label":"Camera","path":"/volume1/Camera","backup":true,"limit":9223372036854775807,"reserve":1000000000})");
  CHECK(sent[1].second == R"({"id":"r2","limit":9223372036854775807,"reserve":0})");
  CHECK(sent[2].second == R"({"id":"r2","limit":500000000000,"reserve":0})");
}

TEST_CASE("phone access: Local search's catalogue through the host, and none without it", "[phone-addon]") {
  mv::addon::catalogue::set_provider({});
  CHECK(mv::addon::catalogue::publish("/x").error() == mv::status::not_found);
  CHECK(mv::addon::catalogue::json().error() == mv::status::not_found);
  std::string published;
  mv::addon::catalogue::provider p;
  p.publish = [&](const std::string& d) -> mv::expected {
    published = d;
    return {};
  };
  p.cancel = [] {};
  p.json = [] { return std::string(R"({"running":false})"); };
  mv::addon::catalogue::set_provider(std::move(p));
  CHECK(mv::addon::catalogue::publish("/Volumes/nas/lib").has_value());
  CHECK(published == "/Volumes/nas/lib");
  CHECK(*mv::addon::catalogue::json() == R"({"running":false})");
  CHECK(mv::addon::catalogue::cancel().has_value());
  mv::addon::catalogue::set_provider({});
  CHECK_FALSE(mv::addon::catalogue::cancel().has_value());
}

#if defined(MV_PHONE_ACCESS_MODULE_PATH)
namespace {

struct keypair {
  std::vector<std::uint8_t> pub = std::vector<std::uint8_t>(crypto_sign_PUBLICKEYBYTES);
  std::vector<std::uint8_t> sec = std::vector<std::uint8_t>(crypto_sign_SECRETKEYBYTES);
  keypair() {
    REQUIRE(sodium_init() >= 0);
    crypto_sign_keypair(pub.data(), sec.data());
  }
  [[nodiscard]] std::vector<std::uint8_t> sign(const std::string& text) const {
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(text.data()), text.size(),
                         sec.data());
    return sig;
  }
};

std::string phone_manifest(const std::string& native, const std::vector<std::uint8_t>& lib) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_PHONE_ACCESS_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  mv::json::writer c;
  mv::addon::screen::write_value(c, *doc->find("contributes"));
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("phone-access");
  w.key("name").string("Phone access");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(lib.size()));
  w.key("native").string(native);
  w.key("chrome").string("");
  w.key("archive").begin_object().key("path").string("phone-access.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  w.begin_object().key("path").string(native).key("sha256").string(mv::addon::sha256_hex(lib))
      .key("size").integer(static_cast<std::int64_t>(lib.size())).key("licence").string("GPL-3.0-or-later")
      .end_object();
  w.end_array();
  w.key("description").string(*doc->str("description"));
  w.key("contributes").raw(c.take());
  w.end_object();
  return w.take();
}

}  // namespace

TEST_CASE("phone access: the shipped manifest declares one settings screen", "[phone-addon][addon]") {
  keypair k;
  const std::string m = phone_manifest("libmv_phone_access.dylib", {1, 2, 3});
  const auto d = mv::addon::check_manifest(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  const auto& c = d.m.slots;
  REQUIRE(c.screens.size() == 1);
  CHECK(c.screens[0].id == "settings");
  CHECK(c.screens[0].place == mv::addon::screen_place::settings);
  CHECK(c.api_min == 16);
  CHECK(c.items.empty());
  CHECK(d.m.commands.empty());
  CHECK(d.m.chrome.empty());
}

TEST_CASE("phone access: the module loads, draws its card in Settings and goes", "[phone-addon][addon]") {
  keypair k;
  scratch_dir s("addons_phone_access");
  fs::create_directories(s / "feature");
#if defined(_WIN32)
  _putenv_s("MV_PHONE_ACCESS_DIR", utf8(s / "feature").c_str());
#else
  setenv("MV_PHONE_ACCESS_DIR", utf8(s / "feature").c_str(), 1);
#endif
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_PHONE_ACCESS_MODULE_PATH).filename());
  const std::vector<std::uint8_t> lib = read_bytes(MV_PHONE_ACCESS_MODULE_PATH);
  REQUIRE(lib.size() > 1000);
  const std::string m = phone_manifest(native, lib);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / native, lib);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  mv::addon::slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  auto loaded = mv::addon::loaded_addon::load(st, "phone-access", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  // The feature's own instance lock: this process runs Phone access.
  CHECK(fs::exists(s / "feature" / "instance.lock"));

  mv_slot_theme theme{};
  REQUIRE(reg.open_screen("phone-access", "settings", "", theme, 0, 0, 2) == mv::status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("Your server") != std::string::npos;
  }));
  {
    const auto open = mv::json::parse(reg.screens_json());
    REQUIRE(open->a.size() == 1);
    CHECK(*open->a[0].str("place") == "settings");
  }
  // A secure field's value reaches the add-on and is never in what the chromes read.
  REQUIRE(reg.send_event("phone-access", "settings", "setup_open", MV_SLOT_EVENT_CLICK, "") == mv::status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("Where the service runs") != std::string::npos;
  }));
  REQUIRE(reg.close_screen("phone-access", "settings") == mv::status::ok);
  reg.drain();
  CHECK(mv::json::parse(reg.screens_json())->a.empty());

  loaded->reset();
  CHECK_FALSE(reg.attached("phone-access"));
#if defined(_WIN32)
  _putenv_s("MV_PHONE_ACCESS_DIR", "");
#else
  unsetenv("MV_PHONE_ACCESS_DIR");
#endif
}
#endif

// ---- This computer: drives and users (ported from the native cards' tests, 2026-11-14) ----

namespace {

const char* kDrivesState = R"J({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":300,
  "host":{"installed":true,"set_up":true,"running":true,"service_port":7878,"paused":true,
          "stats":["Today: 3.4 GB read, 120 MB written"]},
  "users":[{"name":"anna","role":"view","applied":true},{"name":"ben","role":"change","folders":["Photos"],"applied":false}],
  "share_addresses":["smb://pc.local/Photos"],
  "drives":{"os":"mac","pool_ok":true,"disks":[
     {"id":"2","name":"Untitled (2)","facts":"Untitled (2) · 2 TB · WD"},
     {"id":"3","name":"Photos B","facts":"Photos B · 2 TB"},
     {"id":"0","name":"Macintosh HD","facts":"x","refused":"it holds this computer's system"}],
   "sets":[{"id":"{abc}","name":"Photos","kind":"mirror","state":"degraded","line":"Photos (mirror): degraded",
            "members":[{"id":"3"},{"id":"","missing":true}]}]},
  "drives_choices":{"not_a_backup":"RAID is not a backup.","options":[
     {"choice":"separate","title":"Separate","offered":true},
     {"choice":"mirror","title":"Mirror (RAID 1)","offered":true,"recommended":true,"space_words":"2 TB","dead_words":"Nothing is lost."},
     {"choice":"stripe","title":"Stripe (RAID 0)","offered":true,"warning":true},
     {"choice":"parity","title":"Parity (RAID 5)","offered":false,"why_not":"Needs 3 or more drives."}]}})J";

}  // namespace

TEST_CASE("phone access: every typed drive name must match its drive, and the combined drive's name", "[phone-addon]") {
  using mv::phone::card::drive_names_match;
  using mv::phone::card::good_drive_label;
  CHECK(drive_names_match({"Untitled (2)", "  Photos B\n"}, {"Untitled (2)", "Photos B"}));
  CHECK_FALSE(drive_names_match({"Untitled (2)", "photos b"}, {"Untitled (2)", "Photos B"}));
  CHECK_FALSE(drive_names_match({"Photos B", "Untitled (2)"}, {"Untitled (2)", "Photos B"}));
  CHECK_FALSE(drive_names_match({"Untitled (2)"}, {"Untitled (2)", "Photos B"}));
  CHECK_FALSE(drive_names_match({}, {}));
  CHECK_FALSE(drive_names_match({""}, {""}));
  for (const auto& [label, good] : std::vector<std::pair<std::string, bool>>{
           {"Photos", true}, {"My Photos_2-b", true}, {std::string(27, 'a'), true}, {std::string(28, 'a'), false},
           {"", false}, {" Photos", false}, {"Photos/2", false}, {"Fot\xC3\xB3s", false}}) {
    CHECK(good_drive_label(label) == good);
  }
  using mv::phone::card::valid_user_name;
  CHECK(valid_user_name("anna"));
  CHECK(valid_user_name("ann.b_2-c"));
  CHECK_FALSE(valid_user_name("Anna"));
  CHECK_FALSE(valid_user_name("2anna"));
  CHECK_FALSE(valid_user_name("admin"));
  CHECK_FALSE(valid_user_name("anna."));
  CHECK_FALSE(valid_user_name(std::string(21, 'a')));
}

TEST_CASE("phone access: the drives step asks what they are for; combining erases only what was named", "[phone-addon]") {
  screen c(mv::phone::card::computer::mac);
  std::string tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("Running, hosting off") != std::string::npos);
  CHECK(tree.find("Today: 3.4 GB read") != std::string::npos);
  CHECK(tree.find("Replace a drive") != std::string::npos);  // the degraded mirror
  auto r = c.on_event("dr_open", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives"})J");
  // First the question, with no drives or RAID words yet.
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("What are the drives for?") != std::string::npos);
  CHECK(tree.find("Back up a drive") != std::string::npos);
  CHECK(tree.find("Share drives as they are") != std::string::npos);
  CHECK(tree.find("Erases the drives") != std::string::npos);
  CHECK(tree.find("RAID is not a backup.") == std::string::npos);
  CHECK(tree.find(screen::row_id("dk", "2")) == std::string::npos);

  (void)c.on_event("dr_for_combine", kClick, "", kNow);
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("What are the drives for?") == std::string::npos);
  CHECK(tree.find("Can't be erased: it holds this computer's system") != std::string::npos);
  // Picked out of order, asked in the list's order.
  r = c.on_event(screen::row_id("dk", "3") + "_pick", kChange, "true", kNow);
  REQUIRE(r.size() == 1);
  r = c.on_event(screen::row_id("dk", "2") + "_pick", kChange, "true", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_choices","ids":["2","3"]})J");
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("RAID is not a backup.") != std::string::npos);
  CHECK(tree.find("If one dies: Nothing is lost.") != std::string::npos);
  CHECK(tree.find("For photos") != std::string::npos);
  CHECK(tree.find("Risky") != std::string::npos);
  CHECK(tree.find("Needs 3 or more drives.") != std::string::npos);
  CHECK(tree.find("Choose Separate") == std::string::npos);  // that is Share drives as they are
  // Parity is not offered: choosing it does nothing.
  (void)c.on_event("dropt_parity", kClick, "", kNow);
  CHECK(c.on_event("dr_combine", kClick, "", kNow).empty());
  // Mirror: nothing is sent until each drive's name is typed as shown.
  (void)c.on_event("dropt_mirror", kClick, "", kNow);
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("Erase and combine") != std::string::npos);
  CHECK(c.on_event("dr_combine", kClick, "", kNow).empty());
  CHECK(c.build(kDrivesState, kNow).find("Type each drive's name exactly") != std::string::npos);
  (void)c.on_event(screen::row_id("dt", "2"), kChange, quoted("Untitled (2)"), kNow);
  (void)c.on_event(screen::row_id("dt", "3"), kChange, quoted(" Photos B "), kNow);
  r = c.on_event("dr_combine", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_combine","choice":"mirror","ids":["2","3"],"label":"Photos","names":["Untitled (2)","Photos B"]})J");

  // Back, then Share drives as they are: no erase words, nothing typed, no names sent.
  (void)c.on_event("dr_back", kClick, "", kNow);
  CHECK(c.build(kDrivesState, kNow).find("What are the drives for?") != std::string::npos);
  (void)c.on_event("dr_for_share", kClick, "", kNow);
  CHECK(c.on_event("dr_share", kClick, "", kNow).empty());  // nothing picked
  (void)c.on_event(screen::row_id("dk", "2") + "_pick", kChange, "true", kNow);
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("Can't be erased") == std::string::npos);
  CHECK(tree.find("RAID is not a backup.") == std::string::npos);
  r = c.on_event("dr_share", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_combine","choice":"separate","ids":["2"]})J");
  // Replacing the lost drive: only a drive outside the set, named as shown.
  (void)c.build(kDrivesState, kNow);
  (void)c.on_event(screen::row_id("ds", "{abc}") + "_replace", kClick, "", kNow);
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("A drive to take its place") != std::string::npos);
  (void)c.on_event("dr_rep_pick", kChange, quoted("2"), kNow);
  (void)c.on_event("dr_rep_typed", kChange, quoted("Untitled (2)"), kNow);
  r = c.on_event("dr_rep_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_replace","set":"Photos","ids":["2"],"name":"Untitled (2)"})J");
}

// Back up a drive: two drives, one full of photos, one nearly empty (as on 2026-11-23: a 4 TB
// drive with 1.2 TB on it, its new twin with 231 MB).
const char* kCopyState = R"J({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":300,
  "host":{"installed":true,"set_up":true,"running":true,"service_port":7878},
  "drives":{"os":"windows","pool_ok":true,"disks":[
     {"id":"4","name":"Family Media Copy","facts":"x","used":231000000,"free":3999000000000,"mounted":true,
      "used_words":"231 MB","free_words":"4 TB"},
     {"id":"5","name":"Family Media","facts":"x","used":1200000000000,"free":2800000000000,"mounted":true,
      "used_words":"1.2 TB","free_words":"2.8 TB",
      "refused":"it holds a folder this server shares, which may be its only copy"}],
   "sets":[],"copies":[]}})J";

const char* kCopyKept = R"J({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":300,
  "host":{"installed":true,"set_up":true,"running":true,"service_port":7878},
  "drives":{"os":"windows","pool_ok":true,"disks":[],"sets":[],
   "copies":[{"label":"Family Media","to_name":"Family Media Copy","from_id":"5","to_id":"4","state":"copying",
              "line":"Copy on “Family Media Copy”: copying, 1,204 files (420 GB) left"}]}})J";

std::string replaced(std::string s, const std::string& what, const std::string& with) {
  s.replace(s.find(what), what.size(), with);
  return s;
}

TEST_CASE("phone access: back up a full drive onto another, nothing erased and nothing typed", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  (void)c.build(kCopyState, kNow);
  (void)c.on_event("dr_open", kClick, "", kNow);
  std::string tree = c.build(kCopyState, kNow);
  CHECK(tree.find("Start backing up") == std::string::npos);  // not chosen yet
  CHECK(c.on_event("dr_keep_go", kClick, "", kNow).empty());
  (void)c.on_event("dr_for_copy", kClick, "", kNow);
  tree = c.build(kCopyState, kNow);
  require_valid(tree);
  // The full drive is the one copied from, though it can't be erased (that is the point).
  CHECK(tree.find("It is only read, never changed.") != std::string::npos);
  CHECK(tree.find("Fits: 1.2 TB onto 4 TB free.") != std::string::npos);
  CHECK(tree.find("stay in the copy's Recycle Bin for 30 days") != std::string::npos);
  CHECK(tree.find("to erase it") == std::string::npos);  // no name to type
  CHECK(tree.find("Can't be erased") == std::string::npos);
  auto r = c.on_event("dr_keep_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_copy","ids":["5","4"]})J");

  // The other way round can be chosen; going back sends the first way again.
  (void)c.on_event("dr_for_copy", kClick, "", kNow);
  (void)c.build(kCopyState, kNow);
  (void)c.on_event("dr_keep_from", kChange, quoted("4"), kNow);
  tree = c.build(kCopyState, kNow);
  CHECK(tree.find("Fits: 231 MB onto 2.8 TB free.") != std::string::npos);
  (void)c.on_event("dr_keep_from", kChange, quoted("5"), kNow);
  (void)c.build(kCopyState, kNow);
  r = c.on_event("dr_keep_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_copy","ids":["5","4"]})J");

  // A copy drive too small: said with both numbers, and Start backing up can't be pressed.
  std::string small = replaced(replaced(kCopyState, "3999000000000", "500000000000"), "\"4 TB\"", "\"500 GB\"");
  (void)c.on_event("dr_for_copy", kClick, "", kNow);
  tree = c.build(small, kNow);
  CHECK(tree.find("Doesn't fit: Family Media has 1.2 TB on it, Family Media Copy has 500 GB free.") != std::string::npos);
  CHECK(tree.find(R"J("id":"dr_keep_go","label":"Start backing up","style":"primary","help":"Back up Family Media onto Family Media Copy","disabled":true)J") !=
        std::string::npos);

  // Three drives: onto the one with most room unless another is chosen.
  const std::string three = replaced(kCopyState, "\"disks\":[",
                                     R"J("disks":[{"id":"6","name":"Spare","facts":"x","used":0,"free":8000000000000,
                                        "mounted":true,"used_words":"0 KB","free_words":"8 TB"},)J");
  screen t(mv::phone::card::computer::pc);
  (void)t.build(three, kNow);
  (void)t.on_event("dr_open", kClick, "", kNow);
  (void)t.on_event("dr_for_copy", kClick, "", kNow);
  (void)t.build(three, kNow);
  r = t.on_event("dr_keep_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_copy","ids":["5","6"]})J");
  (void)t.on_event("dr_for_copy", kClick, "", kNow);
  (void)t.build(three, kNow);
  (void)t.on_event("dr_keep_to", kChange, quoted("4"), kNow);
  (void)t.build(three, kNow);
  r = t.on_event("dr_keep_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_copy","ids":["5","4"]})J");

  // Drives already in a copy aren't offered again: with one left there is nothing to copy onto.
  const std::string one_left = replaced(three, "\"copies\":[]", R"J("copies":[{"label":"Family Media",
     "to_name":"Family Media Copy","from_id":"5","to_id":"4","state":"up_to_date","line":"x"}])J");
  (void)t.on_event("dr_for_copy", kClick, "", kNow);
  tree = t.build(one_left, kNow);
  require_valid(tree);
  CHECK(tree.find("Connect a second drive") != std::string::npos);
  CHECK(t.on_event("dr_keep_go", kClick, "", kNow).empty());
}

TEST_CASE("phone access: a kept copy says plainly how it stands, and is paused while the server is off", "[phone-addon]") {
  screen k(mv::phone::card::computer::pc);
  std::string tree = k.build(kCopyKept, kNow);
  require_valid(tree);
  CHECK(tree.find("Family Media \xE2\x86\x92 Family Media Copy") != std::string::npos);
  CHECK(tree.find("Copying, 1,204 files (420 GB) left") != std::string::npos);
  CHECK(tree.find("Changes are copied every 15 minutes while the server is on.") != std::string::npos);
  CHECK(tree.find("Copy on ") == std::string::npos);
  auto r = k.on_event(screen::row_id("cp", "Family Media") + "_stop", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives_copy_stop","label":"Family Media"})J");

  // The server off: paused, with what the copy last said.
  const std::string off = replaced(kCopyKept, "\"running\":true", "\"running\":false");
  tree = k.build(off, kNow);
  CHECK(tree.find("Paused: the server is off, so nothing is copied. Last check: Copying, 1,204 files") != std::string::npos);
  // Never started: paused, and nothing about a last check.
  const std::string never = replaced(replaced(off, "\"copying\"", "\"waiting\""),
                                     "copying, 1,204 files (420 GB) left", "starts when the server runs");
  tree = k.build(never, kNow);
  CHECK(tree.find("Paused: the server is off, so nothing is copied.") != std::string::npos);
  CHECK(tree.find("Last check") == std::string::npos);
  CHECK(tree.find("starts when the server runs") == std::string::npos);
  // The server on, the copy not started yet.
  tree = k.build(replaced(never, "\"running\":false", "\"running\":true"), kNow);
  CHECK(tree.find("Starting\xE2\x80\xA6") != std::string::npos);

  // The Mac's word for the bin.
  screen m(mv::phone::card::computer::mac);
  CHECK(m.build(kCopyKept, kNow).find("stay in the copy's Trash for 30 days") != std::string::npos);
}

TEST_CASE("phone access: users ask for the admin first, and a password is sent once and never shown", "[phone-addon]") {
  screen c(mv::phone::card::computer::pc);
  std::string tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  // Asked for once when the section first shows.
  auto queued = c.take_queued();
  REQUIRE(queued.size() == 1);
  CHECK(queued[0] == R"J({"do":"users_refresh"})J");
  CHECK(c.take_queued().empty());
  CHECK(tree.find("Waiting for this PC's administrator") != std::string::npos);
  CHECK(tree.find("smb://pc.local/Photos") != std::string::npos);
  CHECK(tree.find("Can change files \xC2\xB7 Photos") != std::string::npos);
  // A role change keeps the user's folders.
  auto r = c.on_event(screen::row_id("us", "ben") + "_role", kChange, quoted("view"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"user_set","name":"ben","role":"view","folders":["Photos"]})J");
  r = c.on_event(screen::row_id("us", "anna") + "_remove", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"user_remove","name":"anna"})J");
  (void)c.on_event("us_add_open", kClick, "", kNow);
  (void)c.on_event("us_name", kChange, quoted("carla"), kNow);
  (void)c.on_event("us_pass", kChange, quoted("s3cret-pass"), kNow);
  (void)c.on_event("us_pass2", kChange, quoted("s3cret-pass"), kNow);
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("s3cret") == std::string::npos);
  r = c.on_event("us_add_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"user_set","name":"carla","role":"view","password":"s3cret-pass"})J");
  r = c.on_event("us_apply", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"users_apply"})J");
  // Locked: the unlock first, nothing else.
  const std::string locked = R"J({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":0,
    "host":{"installed":true,"set_up":true,"running":true},"users":[{"name":"anna","role":"view","applied":true}]})J";
  tree = c.build(locked, kNow);
  require_valid(tree);
  CHECK(tree.find("Unlock as admin") != std::string::npos);
  CHECK(tree.find(screen::row_id("us", "anna")) == std::string::npos);
}

// docs/plans/phone-clip-convert.md: the server's row says whether it converts clips for the phone,
// with what, and the switch asks the host to change it.
TEST_CASE("phone access: the server row says whether clips are converted, and the switch changes it", "[phone-addon][convert]") {
  const auto tree_for = [](const std::string& convert) {
    screen c(mv::phone::card::computer::mac);
    const std::string state = R"({"host":{"installed":true,"set_up":true,"running":true,"service_port":7878,)" + convert + "}}";
    const std::string tree = c.build(state, kNow);
    require_valid(tree);
    return tree;
  };
  CHECK(tree_for(R"("convert_on":true,"convert_available":true,"convert_encoder":"VideoToolbox","convert_helper":true)")
            .find("graphics hardware (VideoToolbox) as your phone plays them") != std::string::npos);
  CHECK(tree_for(R"("convert_on":true,"convert_available":true,"convert_encoder":"NVENC","convert_busy":true,"convert_helper":true)")
            .find("Converting now.") != std::string::npos);
  CHECK(tree_for(R"("convert_on":true,"convert_available":false,"convert_helper":true)")
            .find("has no hardware video encoder") != std::string::npos);
  CHECK(tree_for(R"("convert_on":false)").find("Off. Your phone offers the download") != std::string::npos);

  screen c(mv::phone::card::computer::pc);
  (void)c.build(R"({"host":{"installed":true,"set_up":true,"running":true,"convert_on":true}})", kNow);
  const auto asked = c.on_event("host_convert", kChange, "false", kNow);
  REQUIRE(asked.size() == 1);
  CHECK(asked[0].find(R"("do":"host_convert")") != std::string::npos);
  CHECK(asked[0].find(R"("on":false)") != std::string::npos);
}

// self-host-users-smb.md §12 (owner, 2026-11-27): each paired computer is a user of the share at
// home. Its row says so, picks its role, renames it, and the host's step makes its account.
TEST_CASE("phone access: a computer is a user: role, rename, and its share account", "[phone-addon]") {
  const char* state = R"({"folder":"/x","claimed":true,"nas_passphrase":"set","unlocked_s":300,
    "nas_features":["passphrase","backup"],"host":{"installed":true,"set_up":true,"running":true},
    "devices":[{"name":"iPhone","key":"b50089df00"},
               {"name":"MacBook Pro","key":"9d14160800","limited":true,"expires":0,"backup":false,
                "desktop":true,"share":"waiting"},
               {"name":"Studio","key":"77aa","desktop":true,"share":"ready","backup":true}]})";
  screen c(mv::phone::card::computer::pc);
  std::string tree = c.build(state, kNow);
  require_valid(tree);
  CHECK(tree.find("Computer \xC2\xB7 Can view files \xC2\xB7 Its share at home waits for this PC's administrator") !=
        std::string::npos);
  CHECK(tree.find("Computer \xC2\xB7 Can change files \xC2\xB7 Opens your shared folders at home with no sign-in") !=
        std::string::npos);
  // A computer picks its role; a phone keeps its Backs up switch.
  CHECK(tree.find(screen::row_id("ph", "9d14160800") + "_role") != std::string::npos);
  CHECK(tree.find(screen::row_id("ph", "9d14160800") + "_backup") == std::string::npos);
  CHECK(tree.find(screen::row_id("ph", "b50089df00") + "_backup") != std::string::npos);
  auto r = c.on_event(screen::row_id("ph", "9d14160800") + "_role", kChange, quoted("change"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"phone_backup","key":"9d14160800","on":true})");
  // One computer waits: the host's administrator finishes it.
  CHECK(tree.find("A computer's share waits for this PC's administrator") != std::string::npos);
  r = c.on_event("ph_share_apply", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"users_apply"})");
  // Rename: the field opens under the row, and Save sends the trimmed name.
  (void)c.on_event(screen::row_id("ph", "9d14160800") + "_rename", kClick, "", kNow);
  tree = c.build(state, kNow);
  require_valid(tree);
  CHECK(tree.find(screen::row_id("ph", "9d14160800") + "_name") != std::string::npos);
  (void)c.on_event(screen::row_id("ph", "9d14160800") + "_name", kChange, quoted("  Tristan's MacBook "), kNow);
  r = c.on_event(screen::row_id("ph", "9d14160800") + "_name_save", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"rename","key":"9d14160800","name":"Tristan's MacBook"})");
  tree = c.build(state, kNow);
  CHECK(tree.find(screen::row_id("ph", "9d14160800") + "_name\"") == std::string::npos);
  // Nothing waiting: no Finish row.
  const std::string ready = c.build(R"({"folder":"/x","claimed":true,"host":{"installed":true,"set_up":true,"running":true},
    "devices":[{"name":"Studio","key":"77aa","desktop":true,"share":"ready"}]})", kNow);
  CHECK(ready.find("ph_share_apply") == std::string::npos);
}

// self-host-users-smb.md §13: people on the card. Add a person, pick them under Belongs to,
// and their role is set once on their row.
TEST_CASE("phone access: people own devices; their role is set once on their row", "[phone-addon]") {
  const char* state = R"({"folder":"/x","claimed":true,"nas_features":["backup"],
    "host":{"installed":true,"set_up":true,"running":true},
    "people":[{"name":"Tristan","role":"change"}],
    "devices":[{"name":"iPhone","key":"b50089df00","person":"Tristan","backup":true},
               {"name":"MacBook Pro","key":"9d14160800","desktop":true,"share":"ready","person":"Tristan","backup":true},
               {"name":"Guest phone","key":"0011","backup":false}]})";
  screen c(mv::phone::card::computer::pc);
  std::string tree = c.build(state, kNow);
  require_valid(tree);
  CHECK(tree.find("2 devices \xC2\xB7 the same access in the app and, at home, in Finder or Explorer") != std::string::npos);
  CHECK(tree.find("Tristan's \xC2\xB7 Can change files") != std::string::npos);
  // A device of Tristan's has no role of its own to set; one of nobody's does.
  CHECK(tree.find(screen::row_id("ph", "9d14160800") + "_role") == std::string::npos);
  CHECK(tree.find(screen::row_id("ph", "b50089df00") + "_backup") == std::string::npos);
  CHECK(tree.find(screen::row_id("ph", "0011") + "_backup") != std::string::npos);
  auto r = c.on_event(screen::row_id("pe", "Tristan") + "_role", kChange, quoted("view"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"person_set","name":"Tristan","role":"view"})");
  r = c.on_event(screen::row_id("ph", "0011") + "_person", kChange, quoted("Tristan"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"device_person","key":"0011","person":"Tristan"})");
  r = c.on_event(screen::row_id("pe", "Tristan") + "_remove", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"person_remove","name":"Tristan"})");
  // Add person: a name and what their devices can do (change, by default).
  (void)c.on_event("people_add_open", kClick, "", kNow);
  tree = c.build(state, kNow);
  require_valid(tree);
  CHECK(tree.find("\"pe_new_name\"") != std::string::npos);
  (void)c.on_event("pe_new_name", kChange, quoted(" Anna "), kNow);
  r = c.on_event("pe_new_go", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"person_set","name":"Anna","role":"change"})");
  // No people yet: the row invites one, and devices have no Belongs to.
  const std::string none = c.build(R"({"folder":"/x","claimed":true,"devices":[{"name":"iPhone","key":"b5"}]})", kNow);
  CHECK(none.find("people_add_open") != std::string::npos);
  CHECK(none.find(screen::row_id("ph", "b5") + "_person") == std::string::npos);
}
