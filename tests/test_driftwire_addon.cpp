// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Driftwire add-on through the real loader (docs/design/28 "The
// add-on"; docs/design/25 PRs 58-59).
//
// Its manifest (src/addons/driftwire/manifest.json, which tools/package/
// addon-pack.py packs) is accepted and declares exactly one window, one
// context entry and one key-less command. Then mv_driftwire is installed in a
// signed store, loaded through the v3 host table, and its window opens from a
// command carrying files, shows them, and goes. Offline (MV_DRIFTWIRE_OFFLINE):
// this test opens no socket; tests/test_driftwire.cpp covers the network.
#include "catch_compat.h"

#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/screen.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::addon::slot_registry;
using mv::status;

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

std::span<const std::uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 1000; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

std::string driftwire_manifest(const std::string& native, const std::vector<std::uint8_t>& lib) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_DRIFTWIRE_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  mv::json::writer c;
  mv::addon::screen::write_value(c, *doc->find("contributes"));
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("driftwire");
  w.key("name").string("Driftwire");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(lib.size()));
  w.key("native").string(native);
  w.key("chrome").string("");  // the hosts render its window; it has no chrome of its own
  w.key("archive").begin_object().key("path").string("driftwire.zip").key("sha256").string(std::string(64, 'a'))
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

TEST_CASE("driftwire: the shipped manifest declares one window, one context entry, one status, one key-less command",
          "[addon][driftwire]") {
  keypair k;
  const std::string m = driftwire_manifest("libmv_driftwire.dylib", {1, 2, 3});
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  const auto& c = d.m.slots;
  REQUIRE(c.screens.size() == 2);
  CHECK(c.screens[0].id == "driftwire");
  CHECK(c.screens[0].title == "Driftwire");
  CHECK(c.screens[0].place == mv::addon::screen_place::window);
  // Its page in Settings -> Add-ons (contribution API 8).
  CHECK(c.screens[1].id == "settings");
  CHECK(c.screens[1].place == mv::addon::screen_place::settings);
  // 9: its transfer card asks to be brought into view (`reveal`, 2026-10-28).
  CHECK(c.api_min == 9);
  REQUIRE(c.items.size() == 2);
  CHECK(c.items[0].id == "send_files");
  CHECK(c.items[0].slot == mv::addon::item_slot::context);
  CHECK(c.items[0].target == mv::addon::item_target::files);
  // #442: "Driftwire: receiving" in the command bar while Keep receiving is on.
  CHECK(c.items[1].id == "receiving");
  CHECK(c.items[1].slot == mv::addon::item_slot::status);
  REQUIRE(d.m.commands.size() == 1);
  CHECK(d.m.commands[0].id == "open");
  CHECK(d.m.commands[0].name == "Driftwire");
  CHECK(d.m.commands[0].windows.empty());  // no new default key (issue #410)
  CHECK(d.m.commands[0].mac.empty());
  CHECK(d.m.commands[0].payload == "marked_or_current");
  CHECK(c.info_sections.empty());
  CHECK(c.search_providers.empty());
  CHECK(c.badges.empty());
  CHECK(d.m.chrome.empty());
}

#if defined(MV_DRIFTWIRE_MODULE_PATH)
TEST_CASE("driftwire: the window opens from a command with its files, offline", "[addon][driftwire]") {
  keypair k;
  scratch_dir s("addons_driftwire");
#if defined(_WIN32)
  _putenv_s("MV_DRIFTWIRE_OFFLINE", "1");
#else
  setenv("MV_DRIFTWIRE_OFFLINE", "1", 1);
#endif
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_DRIFTWIRE_MODULE_PATH).filename());
  const std::vector<std::uint8_t> lib = read_bytes(MV_DRIFTWIRE_MODULE_PATH);
  REQUIRE(lib.size() > 1000);
  const std::string m = driftwire_manifest(native, lib);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / native, lib);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  write_text(s / "photos" / "IMG_1.JPG", "jpeg bytes");
  write_text(s / "photos" / "IMG_2.JPG", "more jpeg bytes");

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  auto loaded = mv::addon::loaded_addon::load(st, "driftwire", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  CHECK(reg.addon_calls() == 0);  // nothing runs until it is asked
  CHECK(mv::json::parse(reg.screens_json())->a.empty());

  // The gallery's share icon / the context entry: these files, then the window.
  const std::string paths = "[\"" + json_path(s / "photos" / "IMG_1.JPG") + "\",\"" +
                            json_path(s / "photos" / "IMG_2.JPG") + "\"]";
  REQUIRE(reg.invoke_item("driftwire", "send_files", paths) == status::ok);
  // The files post at once; the offline note follows from the control thread's
  // reconcile (start_presence), so wait for both.
  REQUIRE(eventually([&] {
    reg.drain();
    const std::string screens = reg.screens_json();
    return screens.find("2 files") != std::string::npos &&
           screens.find("Offline: nothing nearby can be found.") != std::string::npos;
  }));
  const auto screens = mv::json::parse(reg.screens_json());
  REQUIRE(screens);
  REQUIRE(screens->a.size() == 1);
  const auto& win = screens->a[0];
  CHECK(*win.str("id") == "driftwire");
  CHECK(*win.str("place") == "window");
  const std::string tree = reg.screens_json();
  CHECK(tree.find("IMG_1.JPG") != std::string::npos);
  CHECK(tree.find("\"pick\":\"folder\"") != std::string::npos);
  // #508: said once, quietly, under Nearby.
  CHECK(tree.find("Offline: nothing nearby can be found.") != std::string::npos);
  CHECK(tree.find("No one nearby yet") != std::string::npos);

  // Closing it is the person's Done.
  REQUIRE(reg.close_screen("driftwire", "driftwire") == status::ok);
  reg.drain();
  CHECK(mv::json::parse(reg.screens_json())->a.empty());

  // Its key-less command (the palette, Settings' Open) carries the marked or
  // current file and opens the window again; a command it does not have is
  // refused, and so is a payload that is not a list of paths.
  const std::string one = "[\"" + json_path(s / "photos" / "IMG_2.JPG") + "\"]";
  REQUIRE(reg.run_command("driftwire", "open", one) == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("1 file,") != std::string::npos;
  }));
  CHECK(reg.run_command("driftwire", "ghost", "[]") == status::not_found);
  CHECK(reg.run_command("nobody", "open", "[]") == status::not_found);
  CHECK(reg.run_command("driftwire", "open", R"({"no":1})") == status::invalid_arg);
  REQUIRE(reg.close_screen("driftwire", "driftwire") == status::ok);
  reg.drain();

  // Its page in Settings: the window's settings, and the same engine behind them.
  // Opening it is not the window: no presence starts and nothing is listed open
  // but the page.
  mv_slot_theme theme{};
  REQUIRE(reg.open_screen("driftwire", "settings", "", theme, 0, 0, 2) == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("Keep receiving when the Driftwire window is closed") != std::string::npos;
  }));
  {
    const auto open = mv::json::parse(reg.screens_json());
    REQUIRE(open->a.size() == 1);
    CHECK(*open->a[0].str("place") == "settings");
    CHECK(reg.screens_json().find("Offline: nothing nearby") == std::string::npos);
  }
  REQUIRE(reg.send_event("driftwire", "settings", "internet", 2, "true") == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("Find each other through the public DHT") != std::string::npos;
  }));
  REQUIRE(reg.send_event("driftwire", "settings", "internet", 2, "false") == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("Find each other through the public DHT") == std::string::npos;
  }));
  REQUIRE(reg.close_screen("driftwire", "settings") == status::ok);
  reg.drain();
  CHECK(mv::json::parse(reg.screens_json())->a.empty());

  // Keep receiving: the toggle is kept in the add-on's data folder.
  REQUIRE(reg.invoke_item("driftwire", "send_files", one) == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("receiving") != std::string::npos;
  }));
  REQUIRE(reg.send_event("driftwire", "driftwire", "receiving", 2, "true") == status::ok);
  REQUIRE(eventually([&] { return reg.status_json().find("Driftwire: receiving") != std::string::npos; }));
  REQUIRE(reg.close_screen("driftwire", "driftwire") == status::ok);
  reg.drain();

  loaded->reset();  // stops the engine and every thread before the library goes
  CHECK_FALSE(reg.attached("driftwire"));
  auto info = st.find("driftwire");
  REQUIRE(info);
  const std::vector<std::uint8_t> kept = read_bytes(fs::path(info->data_dir) / "driftwire.json");
  CHECK(std::string(kept.begin(), kept.end()).find("\"receiving\":true") != std::string::npos);

  // The next launch: on at load, before any window or command, with the
  // status text up; nothing was asked of it through the registry.
  slot_registry reg2;
  svc.slots = &reg2;
  auto again = mv::addon::loaded_addon::load(st, "driftwire", svc);
  REQUIRE(again);
  REQUIRE(eventually([&] { return reg2.status_json().find("Driftwire: receiving") != std::string::npos; }));
  CHECK(reg2.addon_calls() == 0);
  CHECK(mv::json::parse(reg2.screens_json())->a.empty());  // no window opened
  again->reset();

  // Off: kept off, and the next load makes nothing.
  write_text(fs::path(info->data_dir) / "driftwire.json", R"({"v":1,"receiving":false,"destination":""})");
  slot_registry reg3;
  svc.slots = &reg3;
  auto off = mv::addon::loaded_addon::load(st, "driftwire", svc);
  REQUIRE(off);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(reg3.status_json().find("receiving") == std::string::npos);
  off->reset();
#if defined(_WIN32)
  _putenv_s("MV_DRIFTWIRE_OFFLINE", "");
#else
  unsetenv("MV_DRIFTWIRE_OFFLINE");
#endif
}
#endif
