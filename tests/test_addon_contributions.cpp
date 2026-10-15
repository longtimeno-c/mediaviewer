// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Declarative contributions (docs/design/25 PR 56, contribution API 2): settings
// pages from a schema, the values people choose on them, keymap packs as
// data, theme shape. The keymap packs' effect on the live table is
// tests/test_keymap.cpp (mv_tests, beside the router).
#include "catch_compat.h"

#include <fstream>
#include <string>
#include <vector>

#include "addon/keymap.h"
#include "addon/open_json.h"
#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "addon/settings_schema.h"
#include "addon/settings_values.h"
#include "addon/theme.h"
#include "import_fixture.h"
#include "io/paths.h"
#include "mediaviewer/mediaviewer_addon.h"
#include "open_addon_fixture.h"

using namespace mv::test;
using mv::addon::rejection;
using mv::addon::setting_kind;

namespace {

// The Places page the location add-on (#344) asked for, plus a text field.
constexpr const char* kPlacesPage = R"({
  "title": "Places",
  "sections": [
    { "title": "Place", "help": "How a photo's place is named.",
      "items": [
        { "type": "toggle", "key": "show_map", "label": "Show the map", "default": true },
        { "type": "toggle", "key": "show_town", "label": "Name the town", "default": true },
        { "type": "number", "key": "town_radius_km", "label": "Town radius", "min": 1,
          "max": 50, "step": 1, "unit": "km", "default": 15,
          "help": "Name the nearest town within this distance.", "enabled_by": "show_town" },
        { "type": "choice", "key": "label_names", "label": "Place names",
          "options": [ { "value": "english", "label": "English" },
                       { "value": "local", "label": "Local" } ],
          "default": "english" } ] },
    { "title": "Home",
      "items": [
        { "type": "text", "key": "home", "label": "Home town", "max_length": 32,
          "placeholder": "Lisbon", "default": "" } ] } ] })";

constexpr const char* kVimPacks = R"([
  { "id": "vim", "name": "Vim",
    "bindings": [ { "command": "next", "keys": ["L", "Right"] },
                  { "command": "prev", "keys": ["H", "Left"] },
                  { "command": "toggle_mark", "keys": [""] },
                  { "command": "acme.tools/count-files", "keys": ["Ctrl+Shift+K"] } ] } ])";

mv::addon::settings_page places() {
  auto page = mv::addon::parse_settings_page_text(kPlacesPage);
  REQUIRE(page);
  return *page;
}

// kPlacesPage with `from` replaced by `to` (once).
std::string places_with(const std::string& from, const std::string& to) {
  std::string s = kPlacesPage;
  const auto at = s.find(from);
  REQUIRE(at != std::string::npos);
  s.replace(at, from.size(), to);
  return s;
}

}  // namespace

TEST_CASE("a settings page parses in the order it is written", "[addon][settings]") {
  const auto page = places();
  REQUIRE(page.title == "Places");
  REQUIRE(page.sections.size() == 2);
  REQUIRE(page.sections[0].title == "Place");
  REQUIRE(page.sections[0].help == "How a photo's place is named.");
  const auto& items = page.sections[0].items;
  REQUIRE(items.size() == 4);
  REQUIRE(items[0].kind == setting_kind::toggle);
  REQUIRE(items[0].key == "show_map");
  REQUIRE(items[0].flag);
  REQUIRE(items[2].kind == setting_kind::number);
  REQUIRE(items[2].min == 1.0);
  REQUIRE(items[2].max == 50.0);
  REQUIRE(items[2].step == 1.0);
  REQUIRE(items[2].unit == "km");
  REQUIRE(items[2].number == 15.0);
  REQUIRE(items[2].enabled_by == "show_town");
  REQUIRE(items[3].kind == setting_kind::choice);
  REQUIRE(items[3].options.size() == 2);
  REQUIRE(items[3].options[1].value == "local");
  REQUIRE(items[3].text == "english");
  REQUIRE(page.sections[1].items[0].kind == setting_kind::text);
  REQUIRE(page.sections[1].items[0].max_length == 32);
  REQUIRE(page.find("home") == &page.sections[1].items[0]);
  REQUIRE(page.find("nope") == nullptr);

  REQUIRE(mv::addon::setting_default_json(items[0]) == "true");
  REQUIRE(mv::addon::setting_default_json(items[2]) == "15");
  REQUIRE(mv::addon::setting_default_json(items[3]) == "\"english\"");
  REQUIRE(mv::addon::setting_default_json(page.sections[1].items[0]) == "\"\"");
}

TEST_CASE("a settings page that breaks a rule is refused", "[addon][settings]") {
  const std::vector<std::pair<std::string, std::string>> broken = {
      {R"("type": "toggle", "key": "show_map")", R"("type": "slider", "key": "show_map")"},
      {R"("key": "show_map")", R"("key": "Show_map")"},
      {R"("key": "show_map")", R"("key": "9lives")"},
      {R"("key": "show_town")", R"("key": "show_map")"},  // duplicate
      {R"("label": "Show the map", "default": true)", R"("label": "Show the map", "default": 1)"},
      {R"("label": "Show the map")", R"("label": "Show\u202ethe map")"},
      {R"("label": "Show the map")", R"("label": "")"},
      {R"("label": "Show the map")", R"("label": "Show\u0085the map")"},  // C1 next line
      {R"("label": "Show the map")", R"("label": "Show\u061cthe map")"},  // Arabic letter mark
      {R"("label": "Show the map", "default": true)", R"("label": "Show the map")"},
      {R"("label": "Show the map", "default": true)",
       R"("label": "Show the map", "default": true, "colour": "red")"},
      {R"("min": 1,)", R"("min": 60,)"},
      {R"("default": 15)", R"("default": 51)"},
      {R"("step": 1,)", R"("step": 0,)"},
      {R"("step": 1,)", R"("step": 100,)"},
      // More than the three decimals the store writes back.
      {R"("step": 1,)", R"("step": 0.0015,)"},
      {R"("min": 1,)", R"("min": 1.0004,)"},
      {R"("default": 15)", R"("default": 15.0004)"},
      {R"("enabled_by": "show_town")", R"("enabled_by": "label_names")"},  // not a toggle
      {R"("enabled_by": "show_town")", R"("enabled_by": "home")"},         // not earlier
      {R"("default": "english")", R"("default": "french")"},
      {R"({ "value": "local", "label": "Local" })", R"({ "value": "english", "label": "Local" })"},
      {R"({ "value": "local", "label": "Local" })", R"({ "value": "Local!", "label": "Local" })"},
      {R"("max_length": 32,)", R"("max_length": 0,)"},
      {R"("max_length": 32,)", R"("max_length": 5000,)"},
      {R"("placeholder": "Lisbon", "default": "")", R"("placeholder": "Lisbon", "default": "a\nb")"},
      {R"("title": "Places",)", R"("title": "Places", "theme": 1,)"},
      {R"({ "title": "Home",)", R"({ "title": "",)"},
  };
  for (const auto& [from, to] : broken) {
    INFO(to);
    std::string detail;
    REQUIRE_FALSE(mv::addon::parse_settings_page_text(places_with(from, to), &detail));
    REQUIRE_FALSE(detail.empty());
  }
  REQUIRE_FALSE(mv::addon::parse_settings_page_text(R"({"sections": []})"));
  REQUIRE_FALSE(mv::addon::parse_settings_page_text(R"({"sections": [{"title": "A", "items": []}]})"));
  REQUIRE_FALSE(mv::addon::parse_settings_page_text("[]"));
  REQUIRE_FALSE(mv::addon::parse_settings_page_text("{"));
  {
    // 65 items: one past the ceiling.
    std::string items;
    for (int i = 0; i < 65; ++i) {
      if (i) items += ",";
      items += R"({"type":"toggle","key":"k)" + std::to_string(i) + R"(","label":"K","default":false})";
    }
    REQUIRE_FALSE(mv::addon::parse_settings_page_text(R"({"sections":[{"title":"A","items":[)" +
                                                      items + "]}]}"));
  }
  {
    // A choice with one option is not a choice.
    REQUIRE_FALSE(mv::addon::parse_settings_page_text(
        R"({"sections":[{"title":"A","items":[{"type":"choice","key":"c","label":"C",)"
        R"("options":[{"value":"a","label":"A"}],"default":"a"}]}]})"));
  }
}

TEST_CASE("a value fits its item or is refused", "[addon][settings]") {
  const auto page = places();
  const auto fits = [&](const char* key, const char* json) {
    const auto v = mv::json::parse(json);
    REQUIRE(v);
    return mv::addon::setting_value_fits(*page.find(key), *v);
  };
  REQUIRE(fits("show_map", "false"));
  REQUIRE_FALSE(fits("show_map", "0"));
  REQUIRE_FALSE(fits("show_map", "\"false\""));
  REQUIRE(fits("town_radius_km", "1"));
  REQUIRE(fits("town_radius_km", "50"));
  REQUIRE(fits("town_radius_km", "2.5"));
  REQUIRE(fits("town_radius_km", "2.125"));
  REQUIRE_FALSE(fits("town_radius_km", "2.0004"));  // would be stored as 2.000
  REQUIRE_FALSE(fits("town_radius_km", "0"));
  REQUIRE_FALSE(fits("town_radius_km", "51"));
  REQUIRE_FALSE(fits("town_radius_km", "true"));
  REQUIRE(fits("label_names", "\"local\""));
  REQUIRE_FALSE(fits("label_names", "\"french\""));
  REQUIRE(fits("home", "\"Porto\""));
  REQUIRE_FALSE(fits("home", ("\"" + std::string(33, 'a') + "\"").c_str()));
  REQUIRE_FALSE(fits("home", "\"a\\u0007b\""));
}

TEST_CASE("settings values: namespaced, persistent, kept or forgotten", "[addon][settings]") {
  scratch_dir scratch("addon_settings");
  mv::addon::reset_settings_cache_for_tests();
  const std::string root = utf8(scratch / "Add-on Settings");
  const mv::addon::settings_values values(root);
  const auto page = places();

  // Nothing is created by a read.
  REQUIRE(*values.value_json("acme.places", "", page, "town_radius_km") == "15");
  REQUIRE_FALSE(values.has_values("acme.places"));
  REQUIRE_FALSE(fs::exists(scratch / "Add-on Settings"));
  REQUIRE_FALSE(values.value_json("acme.places", "", page, "nope"));

  REQUIRE(values.set("acme.places", "", page, "town_radius_km", "30"));
  REQUIRE(values.set("acme.places", "", page, "show_map", "false"));
  REQUIRE(values.set("acme.places", "", page, "label_names", "\"local\""));
  REQUIRE(values.set("acme.places", "", page, "home", "\"Porto\""));
  REQUIRE(values.set("acme.places", "", page, "town_radius_km", "2.5"));
  REQUIRE(fs::exists(scratch / "Add-on Settings" / "acme.places.json"));
  // Refused: the wrong kind, out of range, an unknown key, a bad owner.
  REQUIRE_FALSE(values.set("acme.places", "", page, "town_radius_km", "99"));
  REQUIRE_FALSE(values.set("acme.places", "", page, "show_map", "\"no\""));
  REQUIRE_FALSE(values.set("acme.places", "", page, "nope", "true"));
  REQUIRE_FALSE(values.set("../escape", "", page, "show_map", "true"));
  REQUIRE_FALSE(values.set("acme.places/x", "", page, "show_map", "true"));
  REQUIRE_FALSE(values.set("con", "", page, "show_map", "true"));
  REQUIRE_FALSE(values.set("acme.places", "", page, "show_map", "tru"));

  // Namespaced by id: another add-on with the same keys sees its defaults,
  // and a first-party id is its own file.
  REQUIRE(*values.value_json("other.places", "", page, "town_radius_km") == "15");
  REQUIRE(values.set("places", "", page, "show_map", "false"));
  REQUIRE(fs::exists(scratch / "Add-on Settings" / "places.json"));
  REQUIRE(*values.value_json("acme.places", "", page, "show_town") == "true");

  // Survives a restart: a fresh cache reads what the file holds.
  mv::addon::reset_settings_cache_for_tests();
  const mv::addon::settings_values again(root);
  REQUIRE(*again.value_json("acme.places", "", page, "town_radius_km") == "2.500");
  REQUIRE(*again.value_json("acme.places", "", page, "show_map") == "false");
  REQUIRE(*again.value_json("acme.places", "", page, "label_names") == "\"local\"");
  REQUIRE(*again.value_json("acme.places", "", page, "home") == "\"Porto\"");

  // The page as the hosts render it: order, defaults and values folded in.
  const auto doc = mv::json::parse(again.page_json("acme.places", "", page, "Ignored"));
  REQUIRE(doc);
  REQUIRE(*doc->str("title") == "Places");
  const auto& sections = doc->find("sections")->a;
  REQUIRE(sections.size() == 2);
  const auto& items = sections[0].find("items")->a;
  REQUIRE(items.size() == 4);
  REQUIRE(*items[0].str("key") == "show_map");
  REQUIRE(*items[0].str("type") == "toggle");
  REQUIRE(items[0].find("default")->b == true);
  REQUIRE(items[0].find("value")->b == false);
  REQUIRE(*items[2].str("unit") == "km");
  REQUIRE(*items[2].str("enabled_by") == "show_town");
  REQUIRE(items[2].find("min")->i == 1);
  REQUIRE(items[3].find("options")->a.size() == 2);
  REQUIRE(*sections[1].find("items")->a[0].str("placeholder") == "Lisbon");

  // A page that changed in an update: a value the new page does not allow
  // is the default to the add-on, and the stored value stays for an older one.
  const auto narrower = mv::addon::parse_settings_page_text(
      places_with(R"("max": 50, "step": 1, "unit": "km", "default": 15)",
                  R"("max": 2, "step": 1, "unit": "km", "default": 1)"));
  REQUIRE(narrower);
  REQUIRE(*again.value_json("acme.places", "", *narrower, "town_radius_km") == "1");
  REQUIRE(*again.value_json("acme.places", "", page, "town_radius_km") == "2.500");

  // Remove with "keep its settings": nothing is touched. Without: forgotten.
  REQUIRE(again.has_values("acme.places"));
  REQUIRE(again.forget("acme.places"));
  REQUIRE_FALSE(fs::exists(scratch / "Add-on Settings" / "acme.places.json"));
  REQUIRE_FALSE(again.has_values("acme.places"));
  REQUIRE(*again.value_json("acme.places", "", page, "town_radius_km") == "15");
  REQUIRE(again.forget("acme.places"));  // nothing there: fine
  REQUIRE(again.has_values("places"));   // the other id is untouched
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("settings values: a write keeps what another window wrote", "[addon][settings]") {
  // Another window is another process with its own cache. Its write must not
  // be undone by this process's next write, and Settings opening here shows it.
  scratch_dir scratch("addon_settings_two");
  mv::addon::reset_settings_cache_for_tests();
  const std::string root = utf8(scratch / "Add-on Settings");
  const mv::addon::settings_values values(root);
  const auto page = places();
  REQUIRE(values.set("acme.places", "", page, "town_radius_km", "30"));
  // The other process: town 30 (as it read it) and the map turned off.
  {
    std::ofstream other(scratch / "Add-on Settings" / "acme.places.json", std::ios::binary | std::ios::trunc);
    other << R"({"schema":1,"values":{"town_radius_km":30,"show_map":false}})";
  }
  const auto doc = mv::json::parse(values.page_json("acme.places", "", page, "Places"));
  REQUIRE(doc);
  REQUIRE(doc->find("sections")->a[0].find("items")->a[0].find("value")->b == false);
  REQUIRE(values.set("acme.places", "", page, "home", "\"Porto\""));
  mv::addon::reset_settings_cache_for_tests();
  REQUIRE(*values.value_json("acme.places", "", page, "show_map") == "false");
  REQUIRE(*values.value_json("acme.places", "", page, "home") == "\"Porto\"");
  REQUIRE(*values.value_json("acme.places", "", page, "town_radius_km") == "30");
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("an open add-on with a settings page and keymaps installs, and its page survives removal",
          "[addon][settings][open-addon]") {
  scratch_dir scratch("addon_settings_open");
  mv::addon::reset_settings_cache_for_tests();
  mv::io::set_addon_settings_dir_override(utf8(scratch / "Add-on Settings"));
  const mv::addon::open_store store(utf8(scratch / "Open Add-ons"));
  publisher_key k;
  manifest_options o;
  o.api_max = 2;
  o.contributes_raw = {{"settings", kPlacesPage}, {"keymaps", kVimPacks}};
  const auto package = make_package(k, theme_files(), o);
  const auto offer = store.inspect(package);
  REQUIRE(offer.ok());
  REQUIRE(offer.m.settings.sections.size() == 2);
  REQUIRE(offer.m.keymaps.size() == 1);
  REQUIRE(offer.m.keymaps[0].bindings.size() == 4);
  REQUIRE(store.install(package, offer.sha256));

  // The list says what it adds, and the sheet's lines say it.
  const auto listed = mv::json::parse(mv::addon::open_list_json(store));
  REQUIRE(listed);
  REQUIRE(listed->a.size() == 1);
  REQUIRE(listed->a[0].find("settings")->b);
  REQUIRE(*listed->a[0].find("keymaps")->a[0].str("name") == "Vim");
  REQUIRE(listed->a[0].str("adds")->find("a settings page") != std::string::npos);
  REQUIRE(listed->a[0].str("adds")->find("1 key layout") != std::string::npos);

  const std::string page = mv::addon::addon_settings_page_json(store, nullptr, "acme.film-tones");
  REQUIRE(page.find("\"town_radius_km\"") != std::string::npos);
  REQUIRE(mv::addon::addon_settings_page_json(store, nullptr, "acme.other").empty());
  REQUIRE(mv::addon::addon_settings_page_json(store, nullptr, "places").empty());  // no first-party store

  const auto set = mv::json::parse(
      mv::addon::addon_setting_set_json(store, nullptr, "acme.film-tones", "town_radius_km", "40"));
  REQUIRE(set->find("ok")->b);
  const auto refused = mv::json::parse(
      mv::addon::addon_setting_set_json(store, nullptr, "acme.film-tones", "town_radius_km", "400"));
  REQUIRE_FALSE(refused->find("ok")->b);
  REQUIRE_FALSE(refused->str("message")->empty());
  REQUIRE(*mv::addon::open_setting_value_json(store, "acme.film-tones", "town_radius_km") == "40");

  const std::string pack = mv::addon::open_keymap_json(store, "acme.film-tones", "vim");
  REQUIRE(pack.find("\"toggle_mark\"") != std::string::npos);
  REQUIRE(mv::addon::open_keymap_json(store, "acme.film-tones", "emacs").empty());

  // Remove, keeping its settings; install again: the value is there.
  REQUIRE(store.remove("acme.film-tones"));
  REQUIRE(mv::addon::addon_settings_page_json(store, nullptr, "acme.film-tones").empty());
  REQUIRE(fs::exists(scratch / "Add-on Settings" / "acme.film-tones.json"));
  REQUIRE(store.install(package, offer.sha256));
  mv::addon::reset_settings_cache_for_tests();  // and across a restart
  REQUIRE(*mv::addon::open_setting_value_json(store, "acme.film-tones", "town_radius_km") == "40");

  // Remove without keeping: gone, and installing again starts from defaults.
  REQUIRE(store.remove("acme.film-tones"));
  REQUIRE(mv::addon::addon_settings_forget("acme.film-tones"));
  REQUIRE_FALSE(fs::exists(scratch / "Add-on Settings" / "acme.film-tones.json"));
  REQUIRE(store.install(package, offer.sha256));
  REQUIRE(*mv::addon::open_setting_value_json(store, "acme.film-tones", "town_radius_km") == "15");

  mv::io::set_addon_settings_dir_override("");
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("settings kept for one publisher are never read for another", "[addon][settings]") {
  // The store alone: a file written for one publisher's add-on.
  scratch_dir scratch("addon_settings_publisher");
  mv::addon::reset_settings_cache_for_tests();
  const std::string root = utf8(scratch / "Add-on Settings");
  const mv::addon::settings_values values(root);
  const auto page = places();
  REQUIRE(values.set("acme.places", "aaaa", page, "home", "\"Porto\""));
  REQUIRE(values.set("acme.places", "aaaa", page, "show_map", "false"));
  REQUIRE(*values.value_json("acme.places", "aaaa", page, "home") == "\"Porto\"");
  REQUIRE(*values.value_json("acme.places", "bbbb", page, "home") == "\"\"");
  const auto doc = mv::json::parse(values.page_json("acme.places", "bbbb", page, "Places"));
  REQUIRE(doc);
  REQUIRE(*doc->find("sections")->a[1].find("items")->a[0].str("value") == "");
  // A write for the other publisher replaces, never merges.
  REQUIRE(values.set("acme.places", "bbbb", page, "town_radius_km", "20"));
  mv::addon::reset_settings_cache_for_tests();
  REQUIRE(*values.value_json("acme.places", "bbbb", page, "show_map") == "true");
  REQUIRE(*values.value_json("acme.places", "bbbb", page, "town_radius_km") == "20");
  REQUIRE(*values.value_json("acme.places", "aaaa", page, "home") == "\"\"");
  // claim: the same publisher keeps; another deletes.
  REQUIRE(values.claim("acme.places", "bbbb"));
  REQUIRE(fs::exists(scratch / "Add-on Settings" / "acme.places.json"));
  REQUIRE(values.claim("acme.places", "cccc"));
  REQUIRE_FALSE(fs::exists(scratch / "Add-on Settings" / "acme.places.json"));
  REQUIRE(values.claim("acme.places", "cccc"));  // nothing there: fine
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("installing another publisher's add-on under a kept id deletes its settings",
          "[addon][settings][open-addon]") {
  scratch_dir scratch("addon_settings_claim");
  mv::addon::reset_settings_cache_for_tests();
  mv::io::set_addon_settings_dir_override(utf8(scratch / "Add-on Settings"));
  const mv::addon::open_store store(utf8(scratch / "Open Add-ons"));
  manifest_options o;
  o.api_max = 2;
  o.contributes_raw = {{"settings", kPlacesPage}};
  const auto install = [&](const publisher_key& k) {
    const auto package = make_package(k, theme_files(), o);
    const fs::path file = scratch / ("p-" + k.hex().substr(0, 8) + ".mvaddon");
    write_bytes(file, package);
    const auto offer = store.inspect(package);
    REQUIRE(offer.ok());
    const auto done = mv::json::parse(mv::addon::open_install_json(store, utf8(file), offer.sha256));
    REQUIRE(done);
    REQUIRE(done->find("ok")->b);
  };
  const auto value = [&] {
    return *mv::addon::open_setting_value_json(store, "acme.film-tones", "home");
  };
  publisher_key first, second;
  install(first);
  REQUIRE(mv::json::parse(mv::addon::addon_setting_set_json(store, nullptr, "acme.film-tones", "home",
                                                            "\"Porto\""))
              ->find("ok")
              ->b);
  // Removed keeping its settings; the same publisher's again: they are back.
  REQUIRE(store.remove("acme.film-tones"));
  install(first);
  REQUIRE(value() == "\"Porto\"");
  // Removed keeping; a stranger's package under the same id: deleted, never read.
  REQUIRE(store.remove("acme.film-tones"));
  install(second);
  REQUIRE_FALSE(fs::exists(scratch / "Add-on Settings" / "acme.film-tones.json"));
  REQUIRE(value() == "\"\"");
  // Even without the install's claim (a file left another way), the reader
  // does not hand them over.
  REQUIRE(store.remove("acme.film-tones"));
  install(first);
  REQUIRE(mv::json::parse(mv::addon::addon_setting_set_json(store, nullptr, "acme.film-tones", "home",
                                                            "\"Faro\""))
              ->find("ok")
              ->b);
  REQUIRE(store.remove("acme.film-tones"));
  const auto package = make_package(second, theme_files(), o);
  REQUIRE(store.install(package, store.inspect(package).sha256));  // no claim
  mv::addon::reset_settings_cache_for_tests();
  REQUIRE(value() == "\"\"");
  REQUIRE(mv::addon::addon_settings_page_json(store, nullptr, "acme.film-tones").find("Faro") ==
          std::string::npos);
  mv::io::set_addon_settings_dir_override("");
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("API 2 points need API 2, and are refused when malformed", "[addon][manifest]") {
  publisher_key k;
  const auto check = [&](const manifest_options& o) {
    const std::string m = open_manifest_json(k, theme_files(), o);
    return mv::addon::check_open_manifest(bytes_from(m), k.sign(m)).why;
  };
  for (const char* point : {"settings", "keymaps"}) {
    INFO(point);
    manifest_options o;
    o.contributes_raw = {{point, std::string(point) == "settings" ? kPlacesPage : kVimPacks}};
    // Claimed under API 1 only: a mistake.
    REQUIRE(check(o) == rejection::malformed);
    o.api_max = 2;
    REQUIRE(check(o) == rejection::none);
    // An API 1 host leaves them to a later host (one package serves both).
    const std::string m = open_manifest_json(k, theme_files(), o);
    const auto old = mv::addon::check_open_manifest(bytes_from(m), k.sign(m), 1, 1);
    REQUIRE(old.why == rejection::none);
    REQUIRE(old.m.settings.empty());
    REQUIRE(old.m.keymaps.empty());
    // Needs a newer MediaViewer, never half-loaded.
    o.api_min = static_cast<int>(mv::addon::kContributionApi) + 1;
    o.api_max = o.api_min;
    REQUIRE(check(o) == rejection::needs_update);
  }
  {
    manifest_options o;
    o.api_max = 2;
    o.contributes_raw = {{"settings", R"({"sections": []})"}};
    REQUIRE(check(o) == rejection::malformed);
    o.contributes_raw = {{"keymaps", R"([{"id": "Vim", "name": "Vim", "bindings": []}])"}};
    REQUIRE(check(o) == rejection::malformed);
    // A code key is still refused by name, whatever else it holds.
    o.contributes_raw = {{"settings", kPlacesPage}};
    o.extra_key = "native";
    REQUIRE(check(o) == rejection::code_not_allowed);
  }
  // Every point §7 names has an API, settings and keymaps at 2.
  REQUIRE(mv::addon::contribution_api("themes") == 1);
  REQUIRE(mv::addon::contribution_api("settings") == 2);
  REQUIRE(mv::addon::contribution_api("keymaps") == 2);
  REQUIRE(mv::addon::contribution_api("commands") == 3);
  REQUIRE(mv::addon::contribution_api("screens") == 4);
  REQUIRE(mv::addon::contribution_api("tile_badges") == 5);
  REQUIRE(mv::addon::contribution_api("theme") == 0);
  REQUIRE(mv::addon::kContributionApi == MV_ADDON_CONTRIBUTION_API);
}

TEST_CASE("a first-party manifest may carry a settings page", "[addon][manifest][settings]") {
  // Schema 1 is checked under the release key; the page's own rules are the
  // same parser's, so this checks the wiring through read_contributions.
  const auto page = mv::json::parse(kPlacesPage);
  REQUIRE(page);
  REQUIRE(mv::addon::parse_settings_page(*page));
}

TEST_CASE("keymap packs: the shape is checked, names and labels as text", "[addon][keymap]") {
  auto packs = mv::addon::parse_keymaps_text(kVimPacks);
  REQUIRE(packs);
  REQUIRE(packs->size() == 1);
  REQUIRE((*packs)[0].id == "vim");
  REQUIRE((*packs)[0].bindings[0].keys == std::vector<std::string>{"L", "Right"});
  REQUIRE((*packs)[0].bindings[2].keys == std::vector<std::string>{""});
  REQUIRE((*packs)[0].bindings[3].command == "acme.tools/count-files");

  const auto again = mv::addon::parse_keymaps_text(mv::addon::keymaps_to_json(*packs));
  REQUIRE(again);
  REQUIRE((*again)[0].bindings.size() == 4);

  for (const char* bad : {
           R"([])",
           R"([{"id":"vim","name":"Vim","bindings":[]}])",
           R"([{"id":"Vim","name":"Vim","bindings":[{"command":"next","keys":["L"]}]}])",
           R"([{"id":"vim","name":"","bindings":[{"command":"next","keys":["L"]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"Next","keys":["L"]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"a/b/c","keys":["L"]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":[]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":[1]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":["\u0007"]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":["L"]},)"
           R"({"command":"next","keys":["J"]}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":["L"],"modes":"all"}]}])",
           R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":["L"]}]},)"
           R"({"id":"vim","name":"Vim 2","bindings":[{"command":"next","keys":["L"]}]}])",
       }) {
    INFO(bad);
    REQUIRE_FALSE(mv::addon::parse_keymaps_text(bad));
  }
  {
    // Nine keys for one command: one past the ceiling.
    REQUIRE_FALSE(mv::addon::parse_keymaps_text(
        R"([{"id":"vim","name":"Vim","bindings":[{"command":"next","keys":)"
        R"(["A","B","C","D","E","F","G","H","I"]}]}])"));
  }
}

namespace {

// A key label that writes as `bytes` bytes of JSON string body (0..64): '\'
// escapes to two bytes, 'A' to one. At most 32 characters, as labels are.
std::string label_of(std::size_t bytes) {
  return std::string(bytes / 2, '\\') + (bytes % 2 ? "A" : "");
}

// A pack whose JSON is exactly `target` bytes: full bindings (eight 32-character
// labels, each escaping to 64 bytes) while they fit, then one-key bindings
// whose label takes up what is left.
mv::addon::keymap_pack pack_of_bytes(std::size_t target) {
  mv::addon::keymap_pack p{.id = "vim", .name = "Vim", .bindings = {}};
  int n = 0;
  const auto command = [&n] { return "c" + std::to_string(n++); };
  const auto bytes = [&p] { return mv::addon::keymap_pack_json_bytes(p); };
  for (;;) {
    p.bindings.push_back({command(), std::vector<std::string>(8, label_of(64))});
    if (bytes() > target - 400) {
      p.bindings.pop_back();
      break;
    }
  }
  // One-key bindings take `overhead` .. `overhead + 64` bytes each.
  p.bindings.push_back({command(), {""}});
  const std::size_t overhead = bytes();
  p.bindings.pop_back();
  const std::size_t before = bytes();
  const std::size_t one = overhead - before;
  std::size_t left = target - before;
  while (left > 2 * (one + 64)) {
    p.bindings.push_back({command(), {label_of(64)}});
    left -= one + 64;
  }
  const std::size_t a = left / 2;
  p.bindings.push_back({command(), {label_of(a - one)}});
  p.bindings.push_back({command(), {label_of(left - a - one)}});
  return p;
}

}  // namespace

TEST_CASE("keymap packs: one pack is capped at what both hosts carry (#356)", "[addon][keymap]") {
  // The JSON a pack is measured as is the pack as keymaps_to_json writes it.
  const mv::addon::keymap_pack at = pack_of_bytes(mv::addon::kKeymapMaxPackBytes);
  REQUIRE(mv::addon::keymap_pack_json_bytes(at) == mv::addon::kKeymapMaxPackBytes);
  REQUIRE(at.bindings.size() <= mv::addon::kKeymapMaxBindings);
  const std::string text = mv::addon::keymaps_to_json({at});
  REQUIRE(text.size() == mv::addon::kKeymapMaxPackBytes + 2);  // the array's brackets

  // At the cap: accepted, and it round-trips.
  const auto ok = mv::addon::parse_keymaps_text(text);
  REQUIRE(ok);
  REQUIRE(mv::addon::keymaps_to_json(*ok) == text);

  // One byte over (a longer name): refused, and the detail says why.
  mv::addon::keymap_pack over = at;
  over.name = "Vim2";
  REQUIRE(mv::addon::keymap_pack_json_bytes(over) == mv::addon::kKeymapMaxPackBytes + 1);
  std::string detail;
  REQUIRE_FALSE(mv::addon::parse_keymaps_text(mv::addon::keymaps_to_json({over}), &detail));
  REQUIRE(detail == "vim: too large");

  // Every count within its own limit, but escaped labels make it ~170 KB: the
  // pack Windows refused and the Mac applied before the cap. Refused by both now.
  mv::addon::keymap_pack worst{.id = "vim", .name = "Vim", .bindings = {}};
  for (std::size_t i = 0; i < mv::addon::kKeymapMaxBindings; ++i) {
    worst.bindings.push_back({"c" + std::to_string(i),
                              std::vector<std::string>(mv::addon::kKeymapMaxKeys, label_of(64))});
  }
  REQUIRE(mv::addon::keymap_pack_json_bytes(worst) > (128u << 10));
  REQUIRE_FALSE(mv::addon::parse_keymaps_text(mv::addon::keymaps_to_json({worst}), &detail));
  REQUIRE(detail == "vim: too large");
}

TEST_CASE("theme shape: schema 2, bounded, defaults of 1", "[addon][theme]") {
  const std::string palette =
      R"("dark":{"canvas":"#1c1b1a","surface":"#262523","title":"#f2efe9","body":"#b9b4aa",)"
      R"("disabled":"#6f6b64","hairline":"#3a3835","accent":"#e0793a"})";
  {
    const auto r = mv::addon::parse_theme(R"({"schema":1,)" + palette + "}");
    REQUIRE(r.ok());
    REQUIRE(r.t.shape == mv::addon::theme_shape{});
    const auto doc = mv::json::parse(mv::addon::theme_to_json(r.t));
    REQUIRE(doc->find("shape")->find("radius")->k == mv::json::kind::number);
    REQUIRE(doc->find("shape")->find("radius")->d == 1.0);
  }
  {
    const auto r = mv::addon::parse_theme(R"({"schema":2,)" + palette +
                                          R"(,"shape":{"radius":0.5,"density":0.9,"type_scale":1.1}})");
    REQUIRE(r.ok());
    REQUIRE(r.t.shape.radius == 0.5);
    REQUIRE(r.t.shape.density == 0.9);
    REQUIRE(r.t.shape.type_scale == 1.1);
    const std::string json = mv::addon::theme_to_json(r.t);
    REQUIRE(json.find(R"("shape":{"radius":0.500,"density":0.900,"type_scale":1.100})") !=
            std::string::npos);
  }
  {
    // Square corners, and a shape with only one key.
    const auto r = mv::addon::parse_theme(R"({"schema":2,)" + palette + R"(,"shape":{"radius":0}})");
    REQUIRE(r.ok());
    REQUIRE(r.t.shape.radius == 0.0);
    REQUIRE(r.t.shape.density == 1.0);
  }
  for (const char* shape : {R"({"radius":2.5})", R"({"radius":-1})", R"({"density":0.5})",
                            R"({"density":1.5})", R"({"type_scale":0.5})", R"({"type_scale":2})",
                            R"({"corner":1})", R"({"radius":"1"})", R"([])"}) {
    INFO(shape);
    const auto r = mv::addon::parse_theme(R"({"schema":2,)" + palette + R"(,"shape":)" + shape + "}");
    REQUIRE(r.fault == mv::addon::theme_fault::bad_shape);
  }
  // Shape is schema 2's: in a schema 1 file it is a key that file does not know.
  REQUIRE(mv::addon::parse_theme(R"({"schema":1,)" + palette + R"(,"shape":{"radius":1}})").fault ==
          mv::addon::theme_fault::unknown_token);
  REQUIRE_FALSE(mv::addon::parse_theme(R"({"schema":3,)" + palette + "}").ok());
}

TEST_CASE("a schema 2 theme needs an add-on that claims API 2", "[addon][theme][open-addon]") {
  // Shape is an API 2 point, as settings and keymaps are: a manifest whose
  // api.max is 1 may not ship a schema 2 theme (an API 1 host refuses it).
  scratch_dir scratch("addon_theme_api");
  const mv::addon::open_store store(utf8(scratch / "Open Add-ons"));
  std::string schema2 = good_theme();
  schema2.replace(schema2.find(R"("schema":1)"), 10, R"("schema":2)");
  const std::vector<packed_file> files{{"themes/dusk.json", schema2}};
  publisher_key k;
  manifest_options o;
  o.api_max = 1;
  const auto refused = store.inspect(make_package(k, files, o));
  REQUIRE_FALSE(refused.ok());
  REQUIRE(refused.why == rejection::malformed);
  o.api_max = 2;
  const auto package = make_package(k, files, o);
  const auto offer = store.inspect(package);
  REQUIRE(offer.ok());
  REQUIRE(store.install(package, offer.sha256));
  // A schema 1 theme stays fine under API 1.
  o.api_max = 1;
  o.id = "acme.plain-tones";
  REQUIRE(store.inspect(make_package(k, theme_files(), o)).ok());
}
