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

TEST_CASE("phone access: the vocabulary's secure field keeps no value (contribution API 15)", "[phone-addon][slots]") {
  REQUIRE(MV_ADDON_CONTRIBUTION_API == 15);
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

  // A NAS's folder is still typed, and a path the server would refuse is not sent.
  (void)c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow);
  (void)c.on_event("new_path", kChange, quoted("photos"), kNow);
  CHECK(c.on_event("new_add", kClick, "", kNow).empty());
  CHECK(c.build(R"({"folder":"/Volumes/nas/lib","claimed":true})", kNow).find("starting with /") != std::string::npos);
  (void)c.on_event("new_path", kChange, quoted("/volume1/Video"), kNow);
  r = c.on_event("new_add", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"add_folder","label":"Video","path":"/volume1/Video"})");
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
  CHECK(c.api_min == 15);
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

TEST_CASE("phone access: the drives step offers the five ways, erases only what was named", "[phone-addon]") {
  screen c(mv::phone::card::computer::mac);
  std::string tree = c.build(kDrivesState, kNow);
  require_valid(tree);
  CHECK(tree.find("Running, hosting off") != std::string::npos);
  CHECK(tree.find("Today: 3.4 GB read") != std::string::npos);
  CHECK(tree.find("Replace a drive") != std::string::npos);  // the degraded mirror
  auto r = c.on_event("dr_open", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"J({"do":"host_drives"})J");
  tree = c.build(kDrivesState, kNow);
  require_valid(tree);
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
  // Separate erases nothing and sends no names.
  (void)c.on_event(screen::row_id("dk", "2") + "_pick", kChange, "true", kNow);
  (void)c.on_event("dropt_separate", kClick, "", kNow);
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
