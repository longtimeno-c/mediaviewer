// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slots and search providers (docs/design/25 §7, §11; PR 59) and the screens they
// carry (PR 58): the registry both chromes read (src/addon/slots.h), the
// manifest's contribution points, and a real add-on (tests/addons/slots_test)
// loaded through the signed store and the v3 host table.
//
// PR 59's verify, the parts a unit test can hold:
//   - badges for a 10,000-file folder cost no add-on call per tile (counted);
//   - a slow provider never delays typing (the chrome's call returns at once,
//     a newer query supersedes it, its stale answer is dropped);
//   - removing the add-on removes every item.
#include "catch_compat.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/open_manifest.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::addon::rejection;
using mv::addon::slot_registry;
using mv::status;

namespace {

mv_slot_theme test_theme() {
  mv_slot_theme t{};
  t.canvas = 0x1e1e1eff;
  t.surface = 0x262523ff;
  t.title = 0xf2efe9ff;
  t.body = 0xb9b4aaff;
  t.disabled = 0x6f6b64ff;
  t.hairline = 0x3a3835ff;
  t.accent = 0xe0793aff;
  t.dark = 1;
  return t;
}

// A client that records what it was asked and answers through the registry,
// as an add-on would through the host table.
class fake_client final : public mv::addon::slot_client {
 public:
  explicit fake_client(slot_registry& r, std::string id) : r_(r), id_(std::move(id)) {}

  status info_section(const mv_slot_info_request& q) override {
    ++info;
    last_path = q.path_utf8;
    const std::string tree = std::string(R"({"type":"text","id":"t","text":")") + q.location_text + "\"}";
    if (hold_info) wait_release();
    info_status = r_.post_screen(id_, q.section_id, q.generation, tree);
    return status::ok;
  }
  status search(const mv_slot_search_request& q) override {
    ++searches;
    {
      std::lock_guard lock(m);
      queries.emplace_back(q.query_utf8);
    }
    if (std::string(q.folded_utf8).rfind("slow", 0) == 0) {
      // A slow provider: sleeps, then answers for a query that has moved on.
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::vector<slot_registry::search_row> rows{{std::string("/f/") + q.folded_utf8 + ".jpg", "why"}};
    last_search_status = r_.post_search(id_, q.provider_id, q.generation, std::move(rows), true);
    return status::ok;
  }
  status screen_open(const mv_slot_screen_request& q) override {
    ++opens;
    return r_.post_screen(id_, q.screen_id, q.generation,
                          R"({"type":"stack","id":"root","children":[{"type":"toggle","id":"sw","label":"On","value":false}]})");
  }
  status badges(const mv_slot_badges_request& q) override {
    ++badge_requests;
    std::vector<std::string> paths;
    for (int i = 0; i < 10'000; i += 2) paths.push_back(std::string(q.folder_utf8) + "/IMG_" + std::to_string(i) + ".JPG");
    return r_.post_badges(id_, "geo", q.generation, std::move(paths), true);
  }
  void event(const mv_slot_event& e) override {
    std::lock_guard lock(m);
    events.push_back(std::to_string(e.kind) + ":" + e.node_id + ":" + e.value_json);
  }

  void release() {
    {
      std::lock_guard lock(m);
      released = true;
    }
    cv.notify_all();
  }

  std::atomic<int> info{0}, searches{0}, opens{0}, badge_requests{0};
  std::atomic<bool> hold_info{false};
  status info_status = status::ok;
  status last_search_status = status::ok;
  std::string last_path;
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  std::vector<std::string> queries;
  std::vector<std::string> events;

 private:
  void wait_release() {
    std::unique_lock lock(m);
    cv.wait(lock, [this] { return released; });
  }
  slot_registry& r_;
  std::string id_;
};

mv::addon::contributions every_point() {
  using namespace mv::addon;
  contributions c;
  c.api_min = 4;
  c.api_max = 5;
  c.screens.push_back({"explorer", "Explorer", screen_place::window, 640, 480});
  c.info_sections.push_back({"spot", "Spot", true, 10});
  c.info_sections.push_back({"always", "Always", false, -5});
  c.search_providers.push_back({"spot", "Slots test", {"test:"}, true, true});
  c.items.push_back({item_slot::bar, "open", "Slots test", "star", "explorer", item_target::none});
  c.items.push_back({item_slot::path, "find", "Slots test path item", "flag", "", item_target::folder});
  c.items.push_back({item_slot::context, "mark", "Slots test entry", "", "", item_target::files});
  c.items.push_back({item_slot::status, "count", "Slots test", "", "", item_target::none});
  c.badges.push_back({"geo", "Slots test badge", "check"});
  return c;
}

// Waits (bounded) for a condition the add-on's worker makes true.
template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 400; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

}  // namespace

TEST_CASE("coordinates and queries fold the way both panes write them", "[addon][slots]") {
  using mv::addon::parse_location_text;
  auto loc = parse_location_text("38.79810\xC2\xB0 N, 9.39020\xC2\xB0 W");
  REQUIRE(loc);
  REQUIRE(loc->has == 1);
  REQUIRE(std::abs(loc->lat - (38.7981)) < 1e-9);
  REQUIRE(std::abs(loc->lon - (-9.3902)) < 1e-9);
  loc = parse_location_text("33.86882\xC2\xB0 S, 151.20929\xC2\xB0 E");
  REQUIRE(loc);
  REQUIRE(std::abs(loc->lat - (-33.86882)) < 1e-9);
  REQUIRE_FALSE(parse_location_text(""));
  REQUIRE_FALSE(parse_location_text("48.8 N, 2.2 E"));
  REQUIRE_FALSE(parse_location_text("91.0\xC2\xB0 N, 2.0\xC2\xB0 E"));
  REQUIRE_FALSE(parse_location_text("48.0\xC2\xB0 N, 2.0\xC2\xB0 E trailing"));

  using mv::addon::fold_query;
  REQUIRE(fold_query("Portugal") == "portugal");
  REQUIRE(fold_query("S\xC3\xA3o Paulo") == "sao paulo");          // ã
  REQUIRE(fold_query("Z\xC3\xBCrich") == "zurich");                // ü
  REQUIRE(fold_query("Stra\xC3\x9F" "e") == "strasse");             // ß
  REQUIRE(fold_query("\xC5\x81\xC3\xB3" "d\xC5\xBA") == "lodz");    // Łódź
  REQUIRE(fold_query("Sa\xCC\x83o") == "sao");                       // decomposed ã
  REQUIRE(fold_query("\xE6\x9D\xB1\xE4\xBA\xAC") == "\xE6\x9D\xB1\xE4\xBA\xAC");  // passes through
}

TEST_CASE("contributions are tables resolved once at attach; detach removes every item",
          "[addon][slots]") {
  slot_registry r;
  std::atomic<std::uint32_t> changes{0};
  r.set_listener([&](std::uint32_t bits) { changes |= bits; });
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client), "/addons/slotstest/1.0.0") == status::ok);
  REQUIRE((changes & mv::addon::slot_changed_contributions) != 0);
  // Nothing is asked of the add-on at attach.
  REQUIRE(r.addon_calls() == 0);
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::make_unique<fake_client>(r, "slotstest")) ==
          status::invalid_arg);
  REQUIRE(r.attach("empty", "Empty", {}, std::make_unique<fake_client>(r, "empty")) == status::invalid_arg);

  const auto doc = mv::json::parse(r.contributions_json());
  REQUIRE(doc);
  REQUIRE(doc->find("sections")->a.size() == 2);
  REQUIRE(*doc->find("sections")->a[0].str("id") == "always");  // order -5 first
  REQUIRE(doc->find("screens")->a.size() == 1);
  REQUIRE(doc->find("providers")->a.size() == 1);
  REQUIRE(doc->find("items")->a.size() == 4);
  REQUIRE(doc->find("badges")->a.size() == 1);
  REQUIRE(doc->boolean("local_search") == true);
  REQUIRE(*doc->find("addons")->a[0].str("dir") == "/addons/slotstest/1.0.0");

  // Something of everything posted, then removed.
  const auto theme = test_theme();
  r.request_info("/f/a.jpg", "38.79810\xC2\xB0 N, 9.39020\xC2\xB0 W", theme, 280, 2);
  r.search(MV_SLOT_SCOPE_FILES, "portugal", "/f");
  r.request_badges("/f");
  REQUIRE(r.open_screen("slotstest", "explorer", "/f/a.jpg", theme, 0, 0, 2) == status::ok);
  r.drain();
  REQUIRE(r.post_status("slotstest", "count", "3 chosen") == status::ok);
  REQUIRE(fc->opens == 1);
  REQUIRE(mv::json::parse(r.info_json())->find("sections")->a.size() == 2);
  REQUIRE(mv::json::parse(r.screens_json())->a.size() == 1);
  REQUIRE(mv::json::parse(r.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.size() == 1);
  REQUIRE_FALSE(r.badges_for("/f/IMG_0.JPG").empty());
  REQUIRE(mv::json::parse(r.status_json())->a.size() == 1);

  changes = 0;
  r.detach("slotstest");
  REQUIRE(changes == (mv::addon::slot_changed_contributions | mv::addon::slot_changed_info |
                      mv::addon::slot_changed_screens | mv::addon::slot_changed_search |
                      mv::addon::slot_changed_badges | mv::addon::slot_changed_status));
  REQUIRE_FALSE(r.attached("slotstest"));
  const auto after = mv::json::parse(r.contributions_json());
  REQUIRE(after->find("sections")->a.empty());
  REQUIRE(after->find("items")->a.empty());
  REQUIRE(after->boolean("local_search") == false);
  REQUIRE(mv::json::parse(r.info_json())->find("sections")->a.empty());
  REQUIRE(mv::json::parse(r.screens_json())->a.empty());
  REQUIRE(mv::json::parse(r.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());
  REQUIRE(r.badges_for("/f/IMG_0.JPG").empty());
  REQUIRE(mv::json::parse(r.status_json())->a.empty());
  r.detach("slotstest");  // idempotent
}

TEST_CASE("an info section that needs a location is never asked for a file without one",
          "[addon][slots]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  const auto theme = test_theme();
  r.request_info("/f/nowhere.jpg", "", theme, 280, 2);
  r.drain();
  REQUIRE(fc->info == 1);  // only "always"
  auto info = mv::json::parse(r.info_json());
  REQUIRE(info->find("sections")->a.size() == 1);
  REQUIRE(*info->find("sections")->a[0].str("id") == "always");

  r.request_info("/f/sintra.jpg", "38.79810\xC2\xB0 N, 9.39020\xC2\xB0 W", theme, 280, 2);
  r.drain();
  REQUIRE(fc->info == 3);
  info = mv::json::parse(r.info_json());
  REQUIRE(info->find("sections")->a.size() == 2);
  REQUIRE(*info->str("path") == "/f/sintra.jpg");
  REQUIRE(info->find("sections")->a[1].find("tree")->str("text")->find("38.79810") != std::string::npos);

  // A section of the previous file answers late: dropped.
  fc->hold_info = true;
  r.request_info("/f/a.jpg", "1.00000\xC2\xB0 N, 1.00000\xC2\xB0 E", theme, 280, 2);
  r.request_info("/f/b.jpg", "2.00000\xC2\xB0 N, 2.00000\xC2\xB0 E", theme, 280, 2);
  fc->release();
  r.drain();
  // The first request's call was already running when the second came: its
  // post found the generation stale.
  info = mv::json::parse(r.info_json());
  REQUIRE(*info->str("path") == "/f/b.jpg");
  for (const auto& s : info->find("sections")->a) {
    REQUIRE(s.find("tree")->str("text")->find("1.00000") == std::string::npos);
  }
  r.clear_info();
  REQUIRE(mv::json::parse(r.info_json())->find("sections")->a.empty());
}

TEST_CASE("a slow provider never delays typing; a stale answer is dropped", "[addon][slots]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);

  // The provider is busy for 300 ms on "slow"; then the person types five more
  // keys. Every call returns at once, and the queued ones coalesce to the last.
  r.search(MV_SLOT_SCOPE_FILES, "slow", "/f");
  REQUIRE(eventually([&] { return fc->searches.load() == 1; }));
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t last = 0;
  for (const char* q : {"p", "po", "por", "port", "portu"}) last = r.search(MV_SLOT_SCOPE_FILES, q, "/f");
  const auto typed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  CHECK(typed < 50);
  r.drain();
  REQUIRE(fc->searches == 2);  // "slow", then only "portu"
  REQUIRE(fc->queries.back() == "portu");
  REQUIRE(fc->last_search_status == status::ok);
  const auto res = mv::json::parse(r.search_json(MV_SLOT_SCOPE_FILES));
  REQUIRE(res->integer("generation") == static_cast<std::int64_t>(last));
  REQUIRE(res->find("rows")->a.size() == 1);
  REQUIRE(*res->find("rows")->a[0].str("path") == "/f/portu.jpg");  // never "/f/slow.jpg"
  REQUIRE(res->boolean("searching") == false);
  REQUIRE(r.is_stale(last) == false);
  REQUIRE(r.is_stale(last - 1) == true);
  REQUIRE(r.post_search("slotstest", "spot", last - 1, {{"/late.jpg", ""}}, true) == status::cancelled);

  // A prefix routes the query, without the prefix.
  r.search(MV_SLOT_SCOPE_LOCAL, "test: Lisboa", "");
  r.drain();
  REQUIRE(fc->queries.back() == "Lisboa");
  // ...but a digit after it is a date term (Local search's `in:2024`): not routed,
  // the whole query goes to every provider of the scope.
  r.search(MV_SLOT_SCOPE_LOCAL, "test:2024", "");
  r.drain();
  REQUIRE(fc->queries.back() == "test:2024");
  // An empty query asks nobody.
  const int before = fc->searches;
  r.search(MV_SLOT_SCOPE_FILES, "   ", "/f");
  r.drain();
  REQUIRE(fc->searches == before);
  REQUIRE(mv::json::parse(r.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());
}

TEST_CASE("badges for a 10,000-file folder cost no add-on call per tile", "[addon][slots]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  r.request_badges("/big");
  r.drain();
  REQUIRE(fc->badge_requests == 1);
  const auto calls = r.addon_calls();
  // Every tile of the folder asks the table, the way the gallery does.
  std::size_t badged = 0;
  for (int i = 0; i < 10'000; ++i) badged += r.badges_for("/big/IMG_" + std::to_string(i) + ".JPG").size();
  const auto table = mv::json::parse(r.badges_json());
  REQUIRE(badged == 5'000);
  REQUIRE(table->find("tiles")->o.size() == 5'000);
  REQUIRE(*table->find("defs")->a[0].str("key") == "slotstest/geo");
  REQUIRE(r.addon_calls() == calls);  // counted: no add-on call per tile
  REQUIRE(fc->badge_requests == 1);
  // A post for a folder the gallery has left is dropped.
  const auto g = r.request_badges("/other");
  REQUIRE(r.post_badges("slotstest", "geo", g - 1, {"/big/IMG_1.JPG"}, true) == status::cancelled);
  REQUIRE(r.post_badges("slotstest", "nope", g, {"/x"}, true) == status::not_found);
}

TEST_CASE("screens open from items, take checked events, and close", "[addon][slots][screens]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  // A bar item that opens a screen opens it with no event to the add-on.
  REQUIRE(r.invoke_item("slotstest", "open", "[]") == status::ok);
  r.drain();
  REQUIRE(fc->opens == 1);
  auto screens = mv::json::parse(r.screens_json());
  REQUIRE(screens->a.size() == 1);
  REQUIRE(*screens->a[0].str("place") == "window");
  REQUIRE(screens->a[0].find("tree")->str("type"));
  // Opening again brings it forward: no second open.
  REQUIRE(r.invoke_item("slotstest", "open", "[]") == status::ok);
  r.drain();
  REQUIRE(fc->opens == 1);

  REQUIRE(r.send_event("slotstest", "explorer", "sw", MV_SLOT_EVENT_CHANGE, "true") == status::ok);
  REQUIRE(r.send_event("slotstest", "explorer", "sw", MV_SLOT_EVENT_CHANGE, "\"no\"") == status::invalid_arg);
  REQUIRE(r.send_event("slotstest", "explorer", "root", MV_SLOT_EVENT_CLICK, "") == status::invalid_arg);
  REQUIRE(r.send_event("slotstest", "nowhere", "sw", MV_SLOT_EVENT_CHANGE, "true") == status::not_found);
  // A context entry and a path item reach the add-on with their paths.
  REQUIRE(r.invoke_item("slotstest", "mark", R"(["/f/a.jpg","/f/b.jpg"])") == status::ok);
  REQUIRE(r.invoke_item("slotstest", "find", R"(["/f"])") == status::ok);
  REQUIRE(r.invoke_item("slotstest", "mark", R"({"no":1})") == status::invalid_arg);
  REQUIRE(r.invoke_item("slotstest", "ghost", "[]") == status::not_found);
  r.drain();
  {
    std::lock_guard lock(fc->m);
    REQUIRE(fc->events.size() == 3);
    REQUIRE(fc->events[0] == "2:sw:true");
    REQUIRE(fc->events[1] == R"(9:mark:["/f/a.jpg","/f/b.jpg"])");
    REQUIRE(fc->events[2] == R"(9:find:["/f"])");
  }
  // The tree kept the toggle's new value while the add-on decides.
  REQUIRE(r.screens_json().find("\"value\":true") != std::string::npos);

  // Closing tells the add-on; a post for the closed screen is dropped.
  const auto g = mv::json::parse(r.screens_json())->a[0].integer("generation");
  REQUIRE(r.close_screen("slotstest", "explorer") == status::ok);
  r.drain();
  REQUIRE(r.post_screen("slotstest", "explorer", static_cast<std::uint64_t>(*g), R"({"type":"separator","id":"x"})") ==
          status::cancelled);
  {
    std::lock_guard lock(fc->m);
    REQUIRE(fc->events.back().rfind("8:", 0) == 0);
  }
  // The add-on may open its own screen (screen_show), with the last theme.
  REQUIRE(r.show_screen("slotstest", "explorer", true) == status::ok);
  r.drain();
  REQUIRE(fc->opens == 2);
  REQUIRE(r.show_screen("slotstest", "explorer", false) == status::ok);
  REQUIRE(r.status_json() == "[]");
  REQUIRE(r.post_status("slotstest", "open", "x") == status::not_found);  // not a status item
  REQUIRE(r.post_status("slotstest", "count", std::string(200, 'x')) == status::invalid_arg);
}

TEST_CASE("surfaces are copied, bounded, and go with their tree", "[addon][slots][screens]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  const auto theme = test_theme();
  const auto g = r.request_info("/f/a.jpg", "1.00000\xC2\xB0 N, 1.00000\xC2\xB0 E", theme, 280, 1);
  r.drain();
  REQUIRE(r.post_screen("slotstest", "spot", g,
                        R"({"type":"surface","id":"m","surface":"pixels","label":"Test surface","height":40})") == status::ok);
  std::vector<std::uint8_t> px(8 * 4 * 2 + 16, 0x7f);  // stride 48 > width 32 bytes
  REQUIRE(r.post_surface("slotstest", "spot", "pixels", g, px.data(), 8, 2, 48) == status::ok);
  mv::addon::surface_pixels out;
  REQUIRE(r.surface("slotstest", "spot", "pixels", out));
  REQUIRE(out.width == 8);
  REQUIRE(out.bgra.size() == 8 * 2 * 4);
  REQUIRE(out.revision == 1);
  REQUIRE(r.post_surface("slotstest", "spot", "pixels", g, px.data(), 8, 2, 16) == status::invalid_arg);
  REQUIRE(r.post_surface("slotstest", "spot", "pixels", g, px.data(), 5000, 1, 20000) == status::invalid_arg);
  REQUIRE(r.post_surface("slotstest", "spot", "pixels", g + 99, px.data(), 8, 2, 48) == status::cancelled);
  // A new tree without that surface drops its pixels.
  REQUIRE(r.post_screen("slotstest", "spot", g, R"({"type":"text","id":"t","text":"x"})") == status::ok);
  REQUIRE_FALSE(r.surface("slotstest", "spot", "pixels", out));
}

// ---- manifests ----------------------------------------------------------------

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
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(text.data()),
                         text.size(), sec.data());
    return sig;
  }
};

std::span<const std::uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

const char* kContributes = R"({
  "api": {"min": 4, "max": 5},
  "screens": [{"id": "explorer", "title": "Explorer", "place": "window", "width": 640, "height": 480}],
  "info_sections": [{"id": "spot", "title": "Spot", "when": "location", "order": 10}],
  "search_providers": [{"id": "spot", "name": "Slots test", "prefixes": ["test:"], "scope": ["files", "local"]}],
  "bar_items": [{"id": "open", "label": "Slots test", "symbol": "star", "opens": "explorer"}],
  "path_items": [{"id": "find", "label": "Slots test path item", "symbol": "flag", "target": "folder"}],
  "context_menu": [{"id": "mark", "label": "Slots test entry"}],
  "status_items": [{"id": "count", "label": "Slots test"}],
  "tile_badges": [{"id": "geotagged", "label": "Slots test badge", "symbol": "check"}]
})";

std::string slots_manifest(const std::string& native, const std::vector<std::uint8_t>& bytes,
                           const std::string& contributes, int host_min = 3, int host_max = 3,
                           const std::string& id = "slots-test") {
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string(id);
  w.key("name").string("Slots Test");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(host_min).key("max").integer(host_max).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(bytes.size()));
  w.key("native").string(native);
  // An add-on that only fills slots names no chrome (the hosts render its trees).
  w.key("chrome").string(contributes.empty() ? native : std::string());
  w.key("archive").begin_object().key("path").string("slots-test.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  w.begin_object().key("path").string(native).key("sha256").string(mv::addon::sha256_hex(bytes))
      .key("size").integer(static_cast<std::int64_t>(bytes.size())).key("licence").string("GPL-3.0-or-later")
      .end_object();
  w.end_array();
  if (!contributes.empty()) w.key("contributes").raw(contributes);
  w.end_object();
  return w.take();
}

rejection check(const keypair& k, const std::string& contributes, int host_min = 3, int host_max = 3) {
  const std::vector<std::uint8_t> bytes{1, 2, 3};
  const std::string m = slots_manifest("lib.bin", bytes, contributes, host_min, host_max);
  return mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API).why;
}

std::string with(const std::string& replace_from, const std::string& replace_to) {
  std::string s = kContributes;
  const auto at = s.find(replace_from);
  REQUIRE(at != std::string::npos);
  s.replace(at, replace_from.size(), replace_to);
  return s;
}

}  // namespace

TEST_CASE("a first-party manifest names its screens and slots under a contribution API range",
          "[addon][slots][manifest]") {
  keypair k;
  const std::vector<std::uint8_t> bytes{1, 2, 3};
  const std::string m = slots_manifest("lib.bin", bytes, kContributes);
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(d.trusted());
  const auto& c = d.m.slots;
  REQUIRE(c.api_min == 4);
  REQUIRE(c.screens.size() == 1);
  REQUIRE(c.screens[0].place == mv::addon::screen_place::window);
  REQUIRE(c.info_sections[0].needs_location);
  REQUIRE(c.search_providers[0].prefixes == std::vector<std::string>{"test:"});
  REQUIRE(c.search_providers[0].files);
  REQUIRE(c.search_providers[0].local);
  REQUIRE(c.items.size() == 4);
  REQUIRE(c.items[2].target == mv::addon::item_target::files);  // a context entry's default
  REQUIRE(c.badges[0].symbol == "check");

  // Outside the range: "needs a newer MediaViewer", never half-loaded.
  REQUIRE(check(k, with(R"("min": 4, "max": 5)", R"("min": 6, "max": 7)")) == rejection::needs_update);
  // Malformed, each a mistake an author should hear about.
  REQUIRE(check(k, kContributes, 2, 3) == rejection::malformed);                       // table v3 needed
  REQUIRE(check(k, with(R"("min": 4, "max": 5)", R"("min": 4, "max": 4)")) == rejection::malformed);  // slots are 5
  REQUIRE(check(k, with(R"("api": {"min": 4, "max": 5},)", "")) == rejection::malformed);   // no range
  REQUIRE(check(k, with(R"("opens": "explorer")", R"("opens": "nothing")")) == rejection::malformed);
  REQUIRE(check(k, with(R"("symbol": "check")", R"("symbol": "rocket")")) == rejection::malformed);
  REQUIRE(check(k, with(R"("place": "window")", R"("place": "popup")")) == rejection::malformed);
  REQUIRE(check(k, with(R"("prefixes": ["test:"])", R"("prefixes": ["Test"])")) == rejection::malformed);
  REQUIRE(check(k, with(R"("scope": ["files", "local"])", R"("scope": ["web"])")) == rejection::malformed);
  REQUIRE(check(k, with(R"("when": "location")", R"("when": "sometimes")")) == rejection::malformed);
  REQUIRE(check(k, with(R"("id": "spot", "title": "Spot")", R"("id": "explorer", "title": "Spot")")) ==
          rejection::malformed);  // a section and a screen share one id space
  REQUIRE(check(k, with(R"("label": "Slots test", "symbol": "star")", R"("label": "Slots test")")) ==
          rejection::malformed);  // a bar button needs its icon
  REQUIRE(check(k, R"({"api": {"min": 5, "max": 5}})") == rejection::malformed);  // a range over nothing
  REQUIRE(check(k, with(R"("tile_badges")", R"("tile_badgez")")) == rejection::malformed);
  // An add-on with no screens or slots is unaffected.
  REQUIRE(check(k, "", 1, 1) == rejection::none);
}

TEST_CASE("an open add-on cannot fill screens or slots until scripts can (PR 57)",
          "[addon][slots][manifest]") {
  // The contribution API this host serves names them...
  REQUIRE(mv::addon::kContributionApi == 5);
  REQUIRE(MV_ADDON_CONTRIBUTION_API == mv::addon::kContributionApi);
  REQUIRE(mv::addon::contribution_api("info_sections") == 5);
  REQUIRE(mv::addon::contribution_api("screens") == 4);
  // ...and the host function table carries their calls.
  REQUIRE(MV_ADDON_HOST_API == mv::addon::kSlotsHostApi);
}

// ---- a real add-on through the loader -------------------------------------------

#if defined(MV_SLOTS_TEST_MODULE_PATH)
TEST_CASE("a native add-on fills every slot and screen through the v3 host table", "[addon][slots][screens]") {
  keypair k;
  scratch_dir s("addons_slots");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::vector<std::uint8_t> module = read_bytes(MV_SLOTS_TEST_MODULE_PATH);
  REQUIRE(module.size() > 1000);
  const std::string native = utf8(fs::path(MV_SLOTS_TEST_MODULE_PATH).filename());
  const std::string m = slots_manifest(native, module, kContributes);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / fs::path(native), module);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  // A folder: two Portuguese photos, one Spanish, one with no coordinates.
  const fs::path folder = s / "trip";
  fs::create_directories(folder);
  for (const char* name : {"lisboa.jpg", "porto.jpg", "madrid.jpg", "none.jpg"}) write_text(folder / name, "x");
  std::atomic<int> reads{0};
  mv::addon::clear_location_cache();

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  svc.location = [&reads](const std::string& path) -> std::optional<mv_addon_location> {
    ++reads;
    mv_addon_location l{};
    l.has = 1;
    if (path.find("lisboa") != std::string::npos) {
      l.lat = 38.72;
      l.lon = -9.14;
    } else if (path.find("porto") != std::string::npos) {
      l.lat = 41.15;
      l.lon = -8.61;
    } else if (path.find("madrid") != std::string::npos) {
      l.lat = 40.42;
      l.lon = -3.70;
    } else {
      return std::nullopt;
    }
    return l;
  };
  auto loaded = mv::addon::loaded_addon::load(st, "slots-test", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  REQUIRE(reg.attached("slots-test"));
  REQUIRE(reg.addon_calls() == 0);  // zero add-on work until something is shown

  struct test_api {
    uint32_t struct_size;
    uint32_t reserved;
    void (*last_event)(char* out, uint32_t cap);
    uint64_t (*calls)(uint32_t which);
  };
  const auto* probe = static_cast<const test_api*>((*loaded)->query("mv.slots_test.1"));
  REQUIRE(probe);

  // Info section: label, the host's coordinates, the map surface with a pin.
  const auto theme = test_theme();
  reg.request_info(utf8(folder / "lisboa.jpg"), "38.72000\xC2\xB0 N, 9.14000\xC2\xB0 W", theme, 300, 2);
  reg.drain();
  const auto info = mv::json::parse(reg.info_json());
  REQUIRE(info->find("sections")->a.size() == 1);
  const std::string tree = reg.info_json();
  REQUIRE(tree.find("\"Portugal\"") != std::string::npos);
  REQUIRE(tree.find("38.72000") != std::string::npos);
  REQUIRE(tree.find("\"pin\":{\"x\":0.500,\"y\":0.500}") != std::string::npos);
  mv::addon::surface_pixels px;
  REQUIRE(reg.surface("slots-test", "spot", "pixels", px));
  REQUIRE(px.width == 600);   // width_pt * scale
  REQUIRE(px.height == 240);
  REQUIRE(px.bgra[2] == 0x26);  // the theme's surface colour, R
  // No location, no call.
  const auto info_calls = probe->calls(0);
  reg.request_info(utf8(folder / "none.jpg"), "", theme, 300, 2);
  reg.drain();
  REQUIRE(probe->calls(0) == info_calls);
  REQUIRE(mv::json::parse(reg.info_json())->find("sections")->a.empty());

  // Search: "portugal" (and "test:portu") finds exactly the Portuguese files.
  reg.search(MV_SLOT_SCOPE_FILES, "Portugal", utf8(folder));
  reg.drain();
  auto res = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES));
  REQUIRE(res->find("rows")->a.size() == 2);
  for (const auto& row : res->find("rows")->a) {
    REQUIRE(*row.str("why") == "Portugal");
    REQUIRE(row.str("path")->find("madrid") == std::string::npos);
  }
  REQUIRE(res->find("providers")->a[0].integer("total") == 4);
  const int reads_after_first = reads;
  REQUIRE(reads_after_first == 4);  // each file read once; none.jpg is cached as "none"
  reg.search(MV_SLOT_SCOPE_LOCAL, "test: portu", "");
  reg.drain();
  res = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_LOCAL));
  REQUIRE(res->find("rows")->a.empty());  // Local search over the index: no folder given
  reg.search(MV_SLOT_SCOPE_FILES, "test:portu", utf8(folder));
  reg.drain();
  REQUIRE(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.size() == 2);
  REQUIRE(reads == reads_after_first);  // the second pass read nothing (cached)

  // A slow query is overtaken by typing and its answer never shows.
  reg.search(MV_SLOT_SCOPE_FILES, "slowportugal", utf8(folder));
  const auto start = std::chrono::steady_clock::now();
  const auto last = reg.search(MV_SLOT_SCOPE_FILES, "madrid", utf8(folder));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
  reg.drain();
  res = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES));
  REQUIRE(res->integer("generation") == static_cast<std::int64_t>(last));
  REQUIRE(res->find("rows")->a.empty());

  // Badges: one request for the folder, then a table.
  reg.request_badges(utf8(folder));
  reg.drain();
  REQUIRE(probe->calls(3) == 1);
  const auto calls = reg.addon_calls();
  int badged = 0;
  for (const char* name : {"lisboa.jpg", "porto.jpg", "madrid.jpg", "none.jpg"}) {
    badged += static_cast<int>(reg.badges_for(utf8(folder / name)).size());
  }
  REQUIRE(badged == 3);
  REQUIRE(reg.addon_calls() == calls);

  // The window: every node type, the 2,000-tile grid, events both ways.
  REQUIRE(reg.invoke_item("slots-test", "open", "[]") == status::ok);
  reg.drain();
  const auto screens = mv::json::parse(reg.screens_json());
  REQUIRE(screens->a.size() == 1);
  const auto* grid_items = [&]() -> const mv::json::value* {
    const mv::json::value* root = screens->a[0].find("tree");
    return root->find("children")->a[1].find("items");
  }();
  REQUIRE(grid_items->a.size() == 2000);
  REQUIRE(reg.send_event("slots-test", "explorer", "switch", MV_SLOT_EVENT_CHANGE, "true") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return reg.screens_json().find("\"text\":\"On\"") != std::string::npos; }));
  char last_event[256];
  probe->last_event(last_event, sizeof last_event);
  REQUIRE(std::string(last_event) == "2 switch true");
  REQUIRE(reg.send_event("slots-test", "explorer", "close", MV_SLOT_EVENT_CLICK, "") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return reg.screens_json() == "[]"; }));

  // Items: the context entry reaches the add-on, which updates its status item.
  REQUIRE(reg.invoke_item("slots-test", "mark", R"(["/a.jpg","/b.jpg"])") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return reg.status_json().find("2 chosen") != std::string::npos; }));

  // Removing the add-on removes every item, with no restart.
  loaded->reset();
  REQUIRE_FALSE(reg.attached("slots-test"));
  REQUIRE(mv::json::parse(reg.contributions_json())->find("items")->a.empty());
  REQUIRE(reg.status_json() == "[]");
  REQUIRE(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());
  REQUIRE(reg.badges_for(utf8(folder / "lisboa.jpg")).empty());
}

TEST_CASE("an add-on on an older host table gets no slots", "[addon][slots]") {
  keypair k;
  scratch_dir s("addons_slots_old");
  // A host that serves only table 2 (an older MediaViewer): the manifest asks
  // for 3, so the add-on is "needs an update", never loaded without its calls.
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, 2);
  fs::create_directories(s / "addons");
  const std::vector<std::uint8_t> module = read_bytes(MV_SLOTS_TEST_MODULE_PATH);
  const std::string native = utf8(fs::path(MV_SLOTS_TEST_MODULE_PATH).filename());
  const std::string m = slots_manifest(native, module, kContributes);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / fs::path(native), module);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE_FALSE(st.install(*staged));
}
#endif
