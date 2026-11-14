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
    for (const char* node : {"setup_open", "unlock_open", "change_open", "relay_open", "fo1_limit", "restart_open",
                             "handover_open"}) {
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
  CHECK(tree.find("\"fo0_stop\"") == std::string::npos);
  CHECK(tree.find("\"fo1_stop\"") != std::string::npos);

  // A row's controls reach the link with that row's ids.
  auto r = c.on_event("ph0_remove", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"revoke","key":"aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44"})");
  r = c.on_event("ph1_access", kChange, quoted("604800"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"grant","key":"0011","for_s":604800})");
  r = c.on_event("ph1_access", kChange, quoted("unlimited"), kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"grant","key":"0011","for_s":0})");
  r = c.on_event("fo1_stop", kClick, "", kNow);
  REQUIRE(r.size() == 1);
  CHECK(r[0] == R"({"do":"remove_folder","id":"r2"})");
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
