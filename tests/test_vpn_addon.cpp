// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on (docs/plans/vpn-addon.md): taking the Tunnel over from
// Driftwire, the Host section through Phone access, and the module itself
// through the real loader, beside Phone access.
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_addon.h>
#include <sodium.h>

#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/screen.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "addons/driftwire/prefs.h"
#include "addons/vpn/prefs.h"
#include "addons/vpn/service.h"
#include "core/json.h"
#include "import_fixture.h"
#include "vpn_fixture.h"
#include "wire/known.h"
#include "wire/secrets.h"

using namespace mv::test;
namespace vpn = mv::driftwire::vpn;
namespace secrets = mv::driftwire::secrets;

namespace {

constexpr const char* kKey = "TEST-RELAY-KEY-0123456789";

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 300; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return f();
}

std::span<const std::uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

// ---- taking the Tunnel over from Driftwire ------------------------------------------------

TEST_CASE("vpn: the first start takes Driftwire's Tunnel: its switches, the key and a crash's proxy record",
          "[vpn]") {
  scratch_dir s("vpn_adopt");
  // The host's layout: <root>/<id>/data.
#if defined(__APPLE__)
  const fs::path theirs = s / "addons" / "Driftwire" / "data";  // the add-on's name on the Mac
  const fs::path mine = s / "addons" / "VPN" / "data";
#else
  const fs::path theirs = s / "addons" / "driftwire" / "data";
  const fs::path mine = s / "addons" / "vpn" / "data";
#endif
  fs::create_directories(theirs);
  fs::create_directories(mine);
  CHECK(vpn::driftwire_data_dir(utf8(mine)) == utf8(theirs));
  mv::driftwire::prefs d;
  d.relay = true;
  d.relay_server = "relay.example:47614";  // the key in Driftwire's store (#500)
  d.tunnel = true;
  d.tunnel_port = 50123;
  d.tunnel_all_apps = false;
  d.tunnel_tls = true;
  d.tunnel_tls_port = 8443;
  d.tunnel_vpn = true;
  d.tunnel_block = true;
  REQUIRE(mv::driftwire::save_prefs(utf8(theirs / mv::driftwire::kPrefsFile), d));
  write_text(theirs / "driftwire_proxy.json", R"({"pac_url":"http://127.0.0.1:50123/x.pac","saved":""})");
  fs::create_directories(s / "dw_secrets");
  fs::create_directories(s / "my_secrets");
  auto dw_store = secrets::file_store(utf8(s / "dw_secrets"));
  auto my_store = secrets::file_store(utf8(s / "my_secrets"));
  REQUIRE(dw_store->put(mv::driftwire::known::kRelayKeyName, bytes_of(kKey)));

  const std::string prefs_path = utf8(mine / vpn::kPrefsFile);
  REQUIRE(vpn::adopt_driftwire(utf8(theirs), utf8(mine), prefs_path, dw_store.get(), my_store.get()));
  const auto p = vpn::load_prefs(prefs_path);
  REQUIRE(p);
  CHECK(p->server == "relay.example:47614");  // the key is not in the file
  CHECK(p->tunnel);
  CHECK(p->tunnel_port == 50123);
  CHECK_FALSE(p->all_apps);
  CHECK(p->tls);
  CHECK(p->tls_port == 8443);
  CHECK(p->all_traffic);
  CHECK(p->block);
  const auto key = my_store->get(vpn::kServerKeyName);
  REQUIRE(key);
  CHECK(std::string(key->begin(), key->end()) == kKey);
  // The record moved, so the VPN puts the setting back; Driftwire's own key and file stay.
  CHECK(fs::exists(mine / vpn::kProxyRecordFile));
  CHECK_FALSE(fs::exists(theirs / "driftwire_proxy.json"));
  CHECK(dw_store->get(mv::driftwire::known::kRelayKeyName));
  CHECK(mv::driftwire::load_prefs(utf8(theirs / mv::driftwire::kPrefsFile))->tunnel);
  // Once only: a vpn.json of its own is never replaced.
  REQUIRE(vpn::save_prefs(prefs_path, vpn::prefs{}));
  CHECK_FALSE(vpn::adopt_driftwire(utf8(theirs), utf8(mine), prefs_path, dw_store.get(), my_store.get()));
  CHECK_FALSE(vpn::load_prefs(prefs_path)->tunnel);

  // The service starts with what was taken, the key put back on the server.
  REQUIRE(vpn::save_prefs(prefs_path, *p));
  vpn_test::fake_host host;
  vpn::service::options o;
  o.prefs_path = prefs_path;
  o.offline = true;
  o.make_secrets = [&] { return secrets::file_store(utf8(s / "my_secrets")); };
  vpn::service svc(host, o);
  CHECK(svc.snapshot().server == "relay.example:47614#" + std::string(kKey));
  CHECK(svc.snapshot().tunnel);
}

TEST_CASE("vpn: a Driftwire whose Tunnel was never used gives nothing; a key in its file is moved to the store",
          "[vpn]") {
  scratch_dir s("vpn_adopt_none");
  const fs::path theirs = s / "dw";
  const fs::path mine = s / "vpn";
  fs::create_directories(theirs);
  fs::create_directories(mine);
  mv::driftwire::prefs d;
  d.relay_server = "relay.example:47614#" + std::string(kKey);  // relayed transfers only
  REQUIRE(mv::driftwire::save_prefs(utf8(theirs / mv::driftwire::kPrefsFile), d));
  fs::create_directories(s / "my_secrets");
  auto my_store = secrets::file_store(utf8(s / "my_secrets"));
  const std::string prefs_path = utf8(mine / vpn::kPrefsFile);
  CHECK_FALSE(vpn::adopt_driftwire(utf8(theirs), utf8(mine), prefs_path, nullptr, my_store.get()));
  CHECK_FALSE(fs::exists(mine / vpn::kPrefsFile));
  CHECK_FALSE(my_store->get(vpn::kServerKeyName));
  // Used once (its proxy had a port), with the key in the file (no store there).
  d.tunnel_port = 50999;
  REQUIRE(mv::driftwire::save_prefs(utf8(theirs / mv::driftwire::kPrefsFile), d));
  REQUIRE(vpn::adopt_driftwire(utf8(theirs), utf8(mine), prefs_path, nullptr, my_store.get()));
  const auto p = vpn::load_prefs(prefs_path);
  REQUIRE(p);
  CHECK(p->server == "relay.example:47614");
  CHECK_FALSE(p->tunnel);
  CHECK(p->tunnel_port == 50999);
  CHECK(my_store->get(vpn::kServerKeyName));
}

// ---- the Host section: Phone access, through the host ------------------------------------

TEST_CASE("vpn: the Host section is Phone access's answer; its switch and code are Phone access's requests",
          "[vpn]") {
  vpn_test::fake_host host;
  std::mutex m;
  bool on = false, code = false;
  host.phone = [&](const std::string& method, const std::string& json) -> std::optional<std::string> {
    std::lock_guard lock(m);
    if (method == "vpn_set") on = json.find("true") != std::string::npos;
    if (method == "vpn_code") code = true;
    if (method == "vpn_code_hide") code = false;
    mv::json::writer w;
    w.begin_object().key("where").string("here").key("title").string("VPN through this Mac");
    w.key("line").string(on ? "On" : "Off").key("on").boolean(on).key("busy").boolean(false);
    if (code) w.key("code").string("mediaviewer://relay#home.example:47614#k3y");
    w.end_object();
    return w.take();
  };
  vpn::service::options o;
  o.offline = true;
  o.phone_every = std::chrono::milliseconds(50);
  vpn::service svc(host, o);
  svc.settings_opened();
  // The posted tree, not the snapshot: it is posted just after the snapshot changes.
  REQUIRE(eventually([&] { return host.settings().find("VPN through this Mac") != std::string::npos; }));
  auto tree = host.settings();
  INFO(tree);
  CHECK(tree.find("VPN through this Mac") != std::string::npos);
  CHECK(tree.find(R"("id":"host_on","label":"Use as VPN","value":false)") != std::string::npos);
  CHECK(tree.find("\"id\":\"host_code\"") == std::string::npos);  // off: no code to show

  svc.on_event("host_on", 2, "true");
  REQUIRE(eventually([&] { return svc.snapshot().host_on; }));
  REQUIRE(eventually([&] { return host.settings().find("\"id\":\"host_code\"") != std::string::npos; }));
  svc.on_event("host_code", 1, "");
  REQUIRE(eventually([&] { return !svc.snapshot().host_code.empty(); }));
  REQUIRE(eventually([&] { return host.settings().find("mediaviewer://relay#home.example") != std::string::npos; }));
  CHECK(host.settings().find("Settings \xE2\x86\x92 Add-ons \xE2\x86\x92 VPN \xE2\x86\x92 Connect to a VPN host") !=
        std::string::npos);
  // The page closes: the code (it holds the relay's key) goes, here and in Phone access.
  host.show(false);
  svc.settings_closed();
  CHECK(svc.snapshot().host_code.empty());
  svc.settle();
  REQUIRE(eventually([&] {
    for (const auto& [method, json] : host.asked()) {
      if (method == "vpn_code_hide") return true;
    }
    return false;
  }));
  // Every request was one of Phone access's documented ones.
  for (const auto& [method, json] : host.asked()) {
    CHECK((method == "vpn_state" || method == "vpn_set" || method == "vpn_code" || method == "vpn_code_hide"));
  }
}

TEST_CASE("vpn: a switch Phone access refuses says so; a switch then a close sends both, in order", "[vpn]") {
  vpn_test::fake_host host;
  std::mutex m;
  bool refuse = true;
  host.phone = [&](const std::string& method, const std::string&) -> std::optional<std::string> {
    std::lock_guard lock(m);
    if (method != "vpn_state" && refuse) return std::nullopt;  // busy, or gone meanwhile
    return R"({"where":"here","title":"VPN through this Mac","line":"On","on":true,"busy":false,
      "code":"mediaviewer://relay#home.example:47614#k3y"})";
  };
  vpn::service::options o;
  o.offline = true;
  vpn::service svc(host, o);
  svc.settings_opened();
  REQUIRE(eventually([&] { return svc.snapshot().host_where == "here"; }));
  svc.on_event("host_on", 2, "false");
  svc.settle();
  REQUIRE(eventually([&] { return svc.snapshot().host_error.find("didn't take that") != std::string::npos; }));
  REQUIRE(eventually([&] { return host.settings().find("didn't take that") != std::string::npos; }));
  {
    std::lock_guard lock(m);
    refuse = false;
  }
  // Use as VPN, then the page closes at once (a code shows): both reach Phone access.
  svc.on_event("host_on", 2, "true");
  host.show(false);
  svc.settings_closed();
  svc.settle();
  std::vector<std::string> sent;
  for (const auto& [method, json] : host.asked()) {
    if (method != "vpn_state") sent.push_back(method + json);
  }
  REQUIRE(sent.size() >= 2);
  CHECK(sent[sent.size() - 2] == R"(vpn_set{"on":true})");
  CHECK(sent.back().rfind("vpn_code_hide", 0) == 0);
}

TEST_CASE("vpn: without Phone access, or nothing to host on, the Host section says where to set it up", "[vpn]") {
  vpn_test::fake_host host;
  vpn::service::options o;
  o.offline = true;
  vpn::service svc(host, o);
  svc.settings_opened();
  REQUIRE(eventually([&] { return host.settings().find("add Phone access") != std::string::npos; }));
  CHECK(host.settings().find("\"id\":\"host_on\"") == std::string::npos);
  host.phone = [](const std::string&, const std::string&) -> std::optional<std::string> {
    return R"({"where":"none","busy":false})";
  };
  svc.settings_opened();
  REQUIRE(eventually([&] { return host.settings().find("set it up in Settings") != std::string::npos; }));
  CHECK(host.settings().find("\"id\":\"host_on\"") == std::string::npos);
}

TEST_CASE("vpn: the page has both cards, in light and dark the same tree, never the VPN code's key", "[vpn]") {
  vpn::view v;
  auto tree = vpn::build_settings_tree(v);
  CHECK(tree.find("\"id\":\"connect_card\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"host_card\"") != std::string::npos);
  CHECK(tree.find("\"id\":\"connect_card\"") < tree.find("\"id\":\"host_card\""));
  CHECK(mv::json::parse(tree));
  // The menu has the Tunnel's switch, its label cut within the host's limit.
  v.server_shown = std::string(90, 'a') + "\xC3\xA9" + std::string(40, 'b') + ":1";
  v.tunnel = true;
  const auto menu = mv::json::parse(vpn::build_menu(v));
  REQUIRE(menu);
  for (const auto& r : menu->find("items")->a) {
    if (const std::string* label = r.str("label")) {
      CHECK(label->size() <= 120);
      CHECK(label->find("\xC3\xE2") == std::string::npos);  // not cut inside the é
    }
  }
  CHECK(vpn::kept_label(v).size() <= 120);
}

// ---- the module, through the real loader -------------------------------------------------

#if defined(MV_VPN_MODULE_PATH) && defined(MV_PHONE_ACCESS_MODULE_PATH)
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

std::string manifest_of(const char* manifest_json, const char* id, const char* name, const std::string& native,
                        const std::vector<std::uint8_t>& lib) {
  const std::vector<std::uint8_t> raw = read_bytes(manifest_json);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  mv::json::writer c;
  mv::addon::screen::write_value(c, *doc->find("contributes"));
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string(id);
  w.key("name").string(name);
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(lib.size()));
  w.key("native").string(native);
  w.key("chrome").string("");
  w.key("archive").begin_object().key("path").string(std::string(id) + ".zip").key("sha256")
      .string(std::string(64, 'a')).key("size").integer(1).end_object();
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

void install(mv::addon::store& st, const keypair& k, const char* module, const char* manifest_json, const char* id,
             const char* name) {
  const std::string native = utf8(fs::path(module).filename());
  const std::vector<std::uint8_t> lib = read_bytes(module);
  REQUIRE(lib.size() > 1000);
  const std::string m = manifest_of(manifest_json, id, name, native, lib);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / native, lib);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));
}

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) {
    unsetenv(name);
  } else {
    setenv(name, value.c_str(), 1);
  }
#endif
}

}  // namespace

TEST_CASE("vpn: the shipped manifest declares one settings page", "[vpn][addon]") {
  keypair k;
  const std::string m = manifest_of(MV_VPN_MANIFEST_JSON, "vpn", "VPN", "libmv_vpn.dylib", {1, 2, 3});
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  REQUIRE(d.m.slots.screens.size() == 1);
  CHECK(d.m.slots.screens[0].id == "settings");
  CHECK(d.m.slots.screens[0].place == mv::addon::screen_place::settings);
  CHECK(d.m.slots.screens[0].title == "VPN");
  CHECK(d.m.chrome.empty());
}

TEST_CASE("vpn: the module draws its page, and asks Phone access through the host for the Host section",
          "[vpn][addon][phone-addon]") {
  keypair k;
  scratch_dir s("addons_vpn");
  fs::create_directories(s / "feature");
  fs::create_directories(s / "addons");
  set_env("MV_PHONE_ACCESS_DIR", utf8(s / "feature"));
  set_env("MV_VPN_OFFLINE", "1");  // no keychain, no system proxy
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  mv::addon::slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  SECTION("alone: the page, and where to get hosting") {
    install(st, k, MV_VPN_MODULE_PATH, MV_VPN_MANIFEST_JSON, "vpn", "VPN");
    auto v = mv::addon::loaded_addon::load(st, "vpn", svc);
    REQUIRE(v);
    REQUIRE((*v)->slots_attached());
    mv_slot_theme theme{};
    REQUIRE(reg.open_screen("vpn", "settings", "", theme, 0, 0, 2) == mv::status::ok);
    REQUIRE(eventually([&] {
      reg.drain();
      return reg.screens_json().find("add Phone access") != std::string::npos;
    }));
    CHECK(reg.screens_json().find("Connect to a VPN host") != std::string::npos);
    REQUIRE(reg.close_screen("vpn", "settings") == mv::status::ok);
    reg.drain();
    v->reset();
    CHECK_FALSE(reg.attached("vpn"));
  }
  SECTION("beside Phone access: its answer, through addon_request") {
    install(st, k, MV_PHONE_ACCESS_MODULE_PATH, MV_PHONE_ACCESS_MANIFEST_JSON, "phone-access", "Phone access");
    install(st, k, MV_VPN_MODULE_PATH, MV_VPN_MANIFEST_JSON, "vpn", "VPN");
    auto p = mv::addon::loaded_addon::load(st, "phone-access", svc);
    REQUIRE(p);
    auto v = mv::addon::loaded_addon::load(st, "vpn", svc);
    REQUIRE(v);
    mv_slot_theme theme{};
    REQUIRE(reg.open_screen("vpn", "settings", "", theme, 0, 0, 2) == mv::status::ok);
    // Nothing set up in Phone access: it answers "none", and the page says where to set it up.
    REQUIRE(eventually([&] {
      reg.drain();
      return reg.screens_json().find("set it up in Settings") != std::string::npos;
    }));
    CHECK(reg.screens_json().find("add Phone access") == std::string::npos);
    // Phone access answers only the VPN add-on.
    CHECK(reg.addon_request("phone-access", "driftwire", "vpn_state", "{}").error() == mv::status::not_found);
    REQUIRE(reg.close_screen("vpn", "settings") == mv::status::ok);
    reg.drain();
    v->reset();
    p->reset();
  }
  set_env("MV_PHONE_ACCESS_DIR", "");
  set_env("MV_VPN_OFFLINE", "");
}
#endif
