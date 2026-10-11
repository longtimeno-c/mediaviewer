// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Script add-ons as packages (docs/design/25 §4, §9, PR 57): a manifest with
// `scripts`, `permissions` and contributed commands installs; every other PR 55
// refusal still holds; the host runs what is installed and nothing else.
#include "catch_compat.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/open_json.h"
#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "addon/script_host.h"
#include "import_fixture.h"
#include "lua.h"
#include "open_addon_fixture.h"

using namespace mv::test;
using mv::addon::open_store;
using mv::addon::rejection;
using namespace std::chrono_literals;

namespace {

const char* kCountScript = R"(
function count_files()
  local n = mv.folder.count()
  mv.notify(n == 1 and "1 file" or (n .. " files"))
end
)";

std::vector<packed_file> script_files(const std::string& text = kCountScript) {
  return {packed_file{"main.lua", text}};
}

manifest_options script_options() {
  manifest_options o;
  o.id = "acme.count";
  o.name = "Count";
  o.themes = false;
  o.api_min = 3;
  o.api_max = 3;
  o.scripts_json = R"(["main.lua"])";
  o.permissions_json = R"(["folder.read"])";
  o.commands_json =
      R"([{"id":"count-files","name":"Count files","key":"Ctrl+Shift+K","run":"count_files"}])";
  return o;
}

rejection check(const publisher_key& k, const manifest_options& o,
                const std::vector<packed_file>& files = script_files(),
                std::uint32_t api = mv::addon::kContributionApi) {
  const std::string m = open_manifest_json(k, files, o);
  return mv::addon::check_open_manifest(bytes_from(m), k.sign(m), api).why;
}

}  // namespace

TEST_CASE("a script add-on's manifest is read in full", "[script][manifest]") {
  publisher_key k;
  const std::string text = open_manifest_json(k, script_files(), script_options());
  const auto d = mv::addon::check_open_manifest(bytes_from(text), k.sign(text));
  REQUIRE(d.ok());
  REQUIRE(d.m.runs_code());
  REQUIRE(d.m.scripts == std::vector<std::string>{"main.lua"});
  REQUIRE(d.m.permissions == mv::addon::perm_folder_read);
  REQUIRE(d.m.commands.size() == 1);
  REQUIRE(d.m.commands[0].id == "count-files");
  REQUIRE(d.m.commands[0].key == "Ctrl+Shift+K");
  REQUIRE(d.m.commands[0].run == "count_files");
  REQUIRE(d.m.commands[0].modes == "viewing");
  REQUIRE(d.m.commands[0].payload == "none");
}

TEST_CASE("script manifests: every way of getting one wrong", "[script][manifest]") {
  publisher_key k;
  REQUIRE(check(k, script_options()) == rejection::none);

  const auto with = [&](auto edit) {
    manifest_options o = script_options();
    edit(o);
    return check(k, o);
  };
  // A host from before code: "needs a newer MediaViewer" for a range it does
  // not reach, and refused by name for one it does.
  REQUIRE(check(k, script_options(), script_files(), 2) == rejection::needs_update);
  REQUIRE(with([](manifest_options& o) { o.api_min = 1; }) == rejection::none);
  {
    manifest_options o = script_options();
    o.api_min = 1;
    REQUIRE(check(k, o, script_files(), 2) == rejection::code_not_allowed);
  }
  // Code under a range that ends before API 3.
  REQUIRE(with([](manifest_options& o) { o.api_min = 1; o.api_max = 2; }) == rejection::malformed);
  REQUIRE(with([](manifest_options& o) { o.api_min = 1; }) == rejection::none);
  // scripts: an array of listed .lua files, no duplicates.
  REQUIRE(with([](manifest_options& o) { o.scripts_json = R"("main.lua")"; }) == rejection::malformed);
  REQUIRE(with([](manifest_options& o) { o.scripts_json = "[]"; }) == rejection::malformed);
  REQUIRE(with([](manifest_options& o) { o.scripts_json = R"(["other.lua"])"; }) ==
          rejection::unsafe_path);
  REQUIRE(with([](manifest_options& o) { o.scripts_json = R"(["main.lua","main.lua"])"; }) ==
          rejection::malformed);
  {
    manifest_options o = script_options();
    o.scripts_json = R"(["main.txt"])";
    REQUIRE(check(k, o, {packed_file{"main.txt", kCountScript}}) == rejection::malformed);
  }
  {
    manifest_options o = script_options();
    o.scripts_json = R"(["big.lua"])";
    REQUIRE(check(k, o, {packed_file{"big.lua", std::string((1u << 20) + 1, ' ')}}) ==
            rejection::malformed);
  }
  // permissions: known names once each; nothing for a data add-on; no network, ever.
  REQUIRE(with([](manifest_options& o) { o.permissions_json = R"(["network"])"; }) ==
          rejection::malformed);
  REQUIRE(with([](manifest_options& o) {
            o.permissions_json = R"(["network"])";
            o.api_max = 9;
          }) == rejection::malformed);
  REQUIRE(with([](manifest_options& o) { o.permissions_json = R"(["folder.write"])"; }) ==
          rejection::malformed);
  REQUIRE(with([](manifest_options& o) { o.permissions_json = R"(["folder.read","folder.read"])"; }) ==
          rejection::malformed);
  REQUIRE(with([](manifest_options& o) {
            o.permissions_json = R"(["folder.read","thumbnails","selection"])";
          }) == rejection::none);
  // PR 60's permissions: a newer MediaViewer, not this one.
  REQUIRE(with([](manifest_options& o) { o.permissions_json = R"(["files.write"])"; }) ==
          rejection::needs_update);
  REQUIRE(with([](manifest_options& o) { o.permissions_json = R"(["metadata.write"])"; }) ==
          rejection::needs_update);
  {
    manifest_options o;  // a theme add-on asking for a permission
    o.permissions_json = R"(["folder.read"])";
    o.api_max = 3;
    REQUIRE(check(k, o, theme_files()) == rejection::malformed);
  }
  // commands
  for (const char* bad : {
           R"([{"id":"Count","name":"Count","run":"f"}])",
           R"([{"id":"count","name":"","run":"f"}])",
           R"([{"id":"count","name":"Count","run":"1f"}])",
           R"([{"id":"count","name":"Count","run":"f.g"}])",
           R"([{"id":"count","name":"Count"}])",
           R"([{"id":"count","name":"Count","run":"f","key":"Ctrl+ K"}])",
           R"([{"id":"count","name":"Count","run":"f","modes":"always"}])",
           R"([{"id":"count","name":"Count","run":"f","payload":"everything"}])",
           R"([{"id":"count","name":"Count","run":"f","colour":"red"}])",
           R"([{"id":"a","name":"A","run":"f"},{"id":"a","name":"B","run":"g"}])",
           R"([])",
           R"({})",
       }) {
    INFO(bad);
    REQUIRE(with([&](manifest_options& o) { o.commands_json = bad; }) == rejection::malformed);
  }
  // Paths ride along only with `selection`.
  REQUIRE(with([](manifest_options& o) {
            o.commands_json = R"([{"id":"c","name":"C","run":"f","payload":"marks"}])";
          }) == rejection::malformed);
  REQUIRE(with([](manifest_options& o) {
            o.commands_json = R"([{"id":"c","name":"C","run":"f","payload":"marks"}])";
            o.permissions_json = R"(["selection"])";
          }) == rejection::none);
  // A command with no script to run.
  {
    manifest_options o;
    o.api_max = 3;
    o.commands_json = R"([{"id":"c","name":"C","run":"f"}])";
    REQUIRE(check(k, o, theme_files()) == rejection::malformed);
  }
  // Native code stays refused beside a script.
  REQUIRE(with([](manifest_options& o) { o.extra_key = "native"; }) == rejection::code_not_allowed);
  REQUIRE(with([](manifest_options& o) { o.extra_contributes = "main"; }) ==
          rejection::code_not_allowed);
}

TEST_CASE("the consent sheet lists what a script may reach", "[script][consent]") {
  scratch_dir scratch("script_consent");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto package = make_package(k, script_files(), script_options());
  write_bytes(scratch / "count.mvaddon", package);
  const auto doc = mv::json::parse(mv::addon::open_inspect_json(store, utf8(scratch / "count.mvaddon")));
  REQUIRE(doc);
  REQUIRE(*doc->boolean("ok"));
  REQUIRE(*doc->str("adds") == "1 command");
  std::vector<std::string> can;
  for (const auto& v : doc->find("can")->a) can.push_back(v.s);
  REQUIRE(can == std::vector<std::string>{
                     "Run its own script inside MediaViewer, stopped if it uses too much memory or time",
                     "Add commands you can run from the keyboard",
                     "See the names of the files in the open folder"});
  REQUIRE(*doc->str("cannot") == "Change your files or use the network");
  REQUIRE(*doc->boolean("script"));
  REQUIRE(doc->find("permissions")->a.size() == 1);
  REQUIRE(doc->find("permissions")->a[0].s == "folder.read");
  REQUIRE(*doc->find("commands")->a[0].str("key") == "Ctrl+Shift+K");

  // Without folder.read it cannot read files either.
  manifest_options o = script_options();
  o.permissions_json.clear();
  write_bytes(scratch / "bare.mvaddon", make_package(k, script_files(), o));
  const auto bare = mv::json::parse(mv::addon::open_inspect_json(store, utf8(scratch / "bare.mvaddon")));
  REQUIRE(*bare->str("cannot") == "Read your files, change your files, or use the network");

  // A theme add-on reads as it always has.
  write_bytes(scratch / "theme.mvaddon", make_package(k, theme_files()));
  const auto theme = mv::json::parse(mv::addon::open_inspect_json(store, utf8(scratch / "theme.mvaddon")));
  REQUIRE(*theme->str("cannot") == "Run code, read your files, or use the network");
  REQUIRE_FALSE(*theme->boolean("script"));
}

TEST_CASE("PR 55's refusals hold for a package with code", "[script][package]") {
  scratch_dir scratch("script_refusals");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const std::string manifest = open_manifest_json(k, script_files(), script_options());
  const auto entries = [&] {
    std::vector<zip_entry> e;
    e.push_back(stored("manifest.json", bytes_from(manifest)));
    e.push_back(stored("manifest.json.sig", k.sign(manifest)));
    e.push_back(stored("main.lua", bytes_from(kCountScript)));
    return e;
  };
  REQUIRE(store.inspect(make_zip(entries())).ok());
  {
    auto e = entries();
    e[2].bytes[10] ^= 1;  // one byte of the script
    REQUIRE(store.inspect(make_zip(e)).why == rejection::file_mismatch);
  }
  {
    auto e = entries();
    e.push_back(stored("extra.lua", bytes_from("print(1)")));  // unlisted, even if Lua
    REQUIRE(store.inspect(make_zip(e)).why == rejection::unexpected_file);
  }
  {
    auto e = entries();
    e[2].method = 8;  // compressed
    REQUIRE(store.inspect(make_zip(e)).why == rejection::bad_package);
  }
  {
    auto e = entries();
    e[2].name = "../main.lua";
    REQUIRE_FALSE(store.inspect(make_zip(e)).ok());
  }
}

TEST_CASE("an installed script's bytes are checked when it is read to run", "[script][store]") {
  scratch_dir scratch("script_verified");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto package = make_package(k, script_files(), script_options());
  const auto offer = store.inspect(package);
  REQUIRE(offer.ok());
  auto installed = store.install(package, offer.sha256);
  REQUIRE(installed);
  auto text = store.verified_file(*installed, "main.lua", 1u << 20);
  REQUIRE(text);
  REQUIRE(*text == kCountScript);
  REQUIRE_FALSE(store.verified_file(*installed, "other.lua", 1u << 20));
  REQUIRE_FALSE(store.verified_file(*installed, "main.lua", 4));

  // Changed after it was listed: the bytes about to run are refused.
  write_bytes(scratch / "open" / "acme.count" / "1.2.0" / "main.lua",
              bytes_from(std::string(kCountScript).replace(1, 1, "F")));
  const auto changed = store.verified_file(*installed, "main.lua", 1u << 20);
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error() == mv::status::corrupt);
}

TEST_CASE("the host runs installed scripts and their commands", "[script][host]") {
  scratch_dir scratch("script_host");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  {
    const auto package = make_package(k, script_files(), script_options());
    REQUIRE(store.install(package, store.inspect(package).sha256));
  }
  {
    // A theme add-on: nothing to run.
    const auto package = make_package(k, theme_files());
    REQUIRE(store.install(package, store.inspect(package).sha256));
  }
  std::mutex m;
  std::vector<std::string> notices;
  int changes = 0;
  mv::addon::script_services s;
  s.folder_count = [] { return 132u; };
  s.notify = [&](const std::string& id, const std::string& text) {
    std::lock_guard<std::mutex> lock(m);
    notices.push_back(id + ": " + text);
  };
  mv::addon::script_host host(s);
  host.set_on_change([&] {
    std::lock_guard<std::mutex> lock(m);
    ++changes;
  });
  host.reload(store);
  const auto rows = host.commands();
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].addon == "acme.count");
  REQUIRE(rows[0].id == "count-files");
  REQUIRE(rows[0].key == "Ctrl+Shift+K");
  REQUIRE(host.find("acme.count"));
  REQUIRE_FALSE(host.find("acme.film-tones"));
  REQUIRE(host.find("acme.count")->wait_idle(5s));

  REQUIRE(host.run_command("acme.count", "count-files", "null"));
  REQUIRE_FALSE(host.run_command("acme.count", "nope", "null"));
  REQUIRE_FALSE(host.run_command("acme.other", "count-files", "null"));
  REQUIRE(host.find("acme.count")->wait_idle(5s));
  {
    std::lock_guard<std::mutex> lock(m);
    REQUIRE(notices == std::vector<std::string>{"acme.count: 132 files"});
  }

  host.set_key_notes(R"([{"addon":"acme.count","id":"count-files","note":"Ctrl+Shift+K is MediaViewer's"}])");
  const auto status = mv::json::parse(host.status_json());
  REQUIRE(status);
  REQUIRE(status->a.size() == 1);
  REQUIRE(*status->a[0].str("state") == "running");
  REQUIRE(*status->a[0].find("commands")->a[0].str("note") == "Ctrl+Shift+K is MediaViewer's");

  host.stop_all();
  REQUIRE(host.commands().empty());
  REQUIRE(host.status_json() == "[]");
}

TEST_CASE("a script that hits a limit leaves the host's table and says why", "[script][host]") {
  scratch_dir scratch("script_host_limit");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  manifest_options o = script_options();
  o.commands_json = R"([{"id":"spin","name":"Spin","run":"spin"}])";
  const auto package =
      make_package(k, script_files("function spin() while true do end end"), o);
  REQUIRE(store.install(package, store.inspect(package).sha256));

  mv::addon::script_limits limits;
  limits.wall = 200ms;
  limits.instructions = 10'000'000;
  mv::addon::script_host host({}, limits);
  std::mutex m;
  std::condition_variable cv;
  int changes = 0;
  host.set_on_change([&] {
    std::lock_guard<std::mutex> lock(m);
    ++changes;
    cv.notify_all();
  });
  host.reload(store);
  REQUIRE(host.find("acme.count")->wait_idle(5s));
  int before = 0;
  {
    std::lock_guard<std::mutex> lock(m);
    before = changes;
  }
  REQUIRE(host.run_command("acme.count", "spin", ""));
  {
    std::unique_lock<std::mutex> lock(m);
    REQUIRE(cv.wait_for(lock, 5s, [&] { return changes > before; }));
  }
  REQUIRE(host.commands().empty());
  const auto status = mv::json::parse(host.status_json());
  REQUIRE(*status->a[0].str("state") == "stopped");
  const std::string stop = *status->a[0].str("stop");
  REQUIRE((stop == "instructions" || stop == "time"));
  REQUIRE(status->a[0].str("message")->find("command spin") != std::string::npos);

  // Start again: running, the command back.
  host.restart(store, "acme.count");
  REQUIRE(host.commands().size() == 1);
}

TEST_CASE("Lua to JSON refuses what JSON cannot hold", "[script][json]") {
  // Through a binding-shaped task: the conversion runs on the worker.
  mv::addon::script_config c;
  c.addon_id = "acme.json";
  c.sources.push_back({"main.lua", R"(
    good = { name = "x", list = { 1, 2.5, "three", true }, nested = { a = { b = {} } } }
    cyclic = {} cyclic.self = cyclic
    mixed = { 1, 2, x = 3 }
    func = { f = print }
    nan = { v = 0/0 }
  )"});
  auto a = mv::addon::script_addon::start(std::move(c), {});
  REQUIRE(a->wait_idle(5s));
  std::vector<std::string> results;
  mv::addon::script_task t;
  t.run = [&](mv::addon::script_call& call) {
    call.with_state([&](lua_State* L) {
      for (const char* name : {"good", "cyclic", "mixed", "func", "nan"}) {
        lua_getglobal(L, name);
        std::string out;
        std::string why;
        results.push_back(mv::addon::lua_to_json(L, -1, out, 1 << 16, why) ? out
                                                                             : "refused: " + why);
        lua_pop(L, 1);
      }
    });
  };
  REQUIRE(a->post(std::move(t)));
  REQUIRE(a->wait_idle(5s));
  REQUIRE(results.size() == 5);
  const auto good = mv::json::parse(results[0]);
  REQUIRE(good);
  REQUIRE(*good->str("name") == "x");
  REQUIRE(good->find("list")->a.size() == 4);
  REQUIRE(results[1] == "refused: a table that contains itself");
  REQUIRE(results[2].rfind("refused:", 0) == 0);
  REQUIRE(results[3] == "refused: a function value");
  REQUIRE(results[4] == "refused: a number that is not finite");
}
