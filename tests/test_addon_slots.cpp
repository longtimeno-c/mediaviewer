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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/open_json.h"
#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "addon/settings_values.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"
#include "io/paths.h"

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
                      mv::addon::slot_changed_badges | mv::addon::slot_changed_status |
                      mv::addon::slot_changed_notices));
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

TEST_CASE("a status item opens its add-on's only screen, or the one it names (#512)", "[addon][slots]") {
  using namespace mv::addon;
  const auto opens_of = [](const slot_registry& r) -> std::string {
    const auto doc = mv::json::parse(r.status_json());
    REQUIRE(doc);
    REQUIRE(doc->a.size() == 1);
    const std::string* o = doc->a[0].str("opens");
    return o ? *o : std::string("(none)");
  };
  const auto status_only = [] {
    contributions c;
    c.api_min = 5;
    c.api_max = 5;
    c.items.push_back({item_slot::status, "count", "Slots test", "", "", item_target::none});
    return c;
  };

  // One window: a click opens it, with no manifest change.
  {
    slot_registry r;
    contributions c = every_point();
    REQUIRE(r.attach("slotstest", "Slots test", c, std::make_unique<fake_client>(r, "slotstest")) == status::ok);
    REQUIRE(r.post_status("slotstest", "count", "3 chosen") == status::ok);
    REQUIRE(opens_of(r) == "explorer");
  }
  // No screen: the item stays text.
  {
    slot_registry r;
    REQUIRE(r.attach("plain", "Plain", status_only(), std::make_unique<fake_client>(r, "plain")) == status::ok);
    REQUIRE(r.post_status("plain", "count", "3 chosen") == status::ok);
    REQUIRE(opens_of(r) == "(none)");
  }
  // Two screens: none is guessed, unless the item names one.
  contributions two = status_only();
  two.screens.push_back({"explorer", "Explorer", screen_place::window, 0, 0});
  two.screens.push_back({"side", "Side", screen_place::pane, 0, 0});
  {
    slot_registry r;
    REQUIRE(r.attach("two", "Two", two, std::make_unique<fake_client>(r, "two")) == status::ok);
    REQUIRE(r.post_status("two", "count", "3 chosen") == status::ok);
    REQUIRE(opens_of(r) == "(none)");
  }
  two.items[0].opens = "side";
  {
    slot_registry r;
    REQUIRE(r.attach("two", "Two", two, std::make_unique<fake_client>(r, "two")) == status::ok);
    REQUIRE(r.post_status("two", "count", "3 chosen") == status::ok);
    REQUIRE(opens_of(r) == "side");
  }
  // A settings page is not one of them: one window and a settings page opens the window.
  contributions with_page = status_only();
  with_page.api_min = with_page.api_max = 8;
  with_page.screens.push_back({"explorer", "Explorer", screen_place::window, 0, 0});
  with_page.screens.push_back({"settings", "Explorer", screen_place::settings, 0, 0});
  {
    slot_registry r;
    REQUIRE(r.attach("paged", "Paged", with_page, std::make_unique<fake_client>(r, "paged")) == status::ok);
    REQUIRE(r.post_status("paged", "count", "3 chosen") == status::ok);
    REQUIRE(opens_of(r) == "explorer");
  }
}

TEST_CASE("a settings page opens only from Settings, and is listed as place settings", "[addon][slots]") {
  using namespace mv::addon;
  contributions c;
  c.api_min = c.api_max = 8;
  c.screens.push_back({"explorer", "Explorer", screen_place::window, 0, 0});
  c.screens.push_back({"settings", "Explorer", screen_place::settings, 0, 0});
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "paged");
  fake_client* fc = client.get();
  REQUIRE(r.attach("paged", "Paged", c, std::move(client)) == status::ok);
  const auto contrib = mv::json::parse(r.contributions_json());
  REQUIRE(contrib);
  bool listed = false;
  for (const auto& s : contrib->find("screens")->a) listed = listed || (*s.str("place") == "settings");
  CHECK(listed);
  // The add-on cannot put it up (only the page in Settings does); it can its window.
  CHECK(r.show_screen("paged", "settings", true) == status::invalid_arg);
  CHECK(mv::json::parse(r.screens_json())->a.empty());
  // Settings opens it: asked with MV_SLOT_PLACE_SETTINGS, its tree listed.
  mv_slot_theme theme{};
  REQUIRE(r.open_screen("paged", "settings", "", theme, 0, 0, 2) == status::ok);
  r.drain();
  CHECK(fc->opens == 1);
  const auto open = mv::json::parse(r.screens_json());
  REQUIRE(open->a.size() == 1);
  CHECK(*open->a[0].str("place") == "settings");
  // Its controls answer as a window's do.
  REQUIRE(r.send_event("paged", "settings", "sw", MV_SLOT_EVENT_CHANGE, "true") == status::ok);
  REQUIRE(r.close_screen("paged", "settings") == status::ok);
  r.drain();
  CHECK(mv::json::parse(r.screens_json())->a.empty());
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
  // An accented prefix folds to it, and is cut whole (2-byte "\xC3\x89" folds to 1).
  r.search(MV_SLOT_SCOPE_LOCAL, "T\xC3\x89ST: Lisboa", "");
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

// #441: which queries name a place, read as the AI pack's parser reads them.
TEST_CASE("place terms are place:x and in:<word>, never a date or a quoted phrase", "[addon][slots][places]") {
  using mv::addon::has_place_terms;
  CHECK(has_place_terms("place:Portugal"));
  CHECK(has_place_terms("beach place:portugal"));
  CHECK(has_place_terms("PLACE:lisbon"));
  CHECK(has_place_terms("in:lisbon"));
  CHECK(has_place_terms("-place:spain"));
  CHECK(has_place_terms("video -in:spain"));
  CHECK(has_place_terms("place:\"new york\""));
  CHECK(has_place_terms("place:\xE2\x80\x9Cnew york\xE2\x80\x9D"));
  CHECK_FALSE(has_place_terms(""));
  CHECK_FALSE(has_place_terms("portugal"));        // a plain word: the description, and tags
  CHECK_FALSE(has_place_terms("in:2024"));         // a date
  CHECK_FALSE(has_place_terms("in:2024-06 beach"));
  CHECK_FALSE(has_place_terms("place:"));          // still being typed
  CHECK_FALSE(has_place_terms("place:\""));
  CHECK_FALSE(has_place_terms("\"we went place:x\""));  // words said, not an operator
  CHECK_FALSE(has_place_terms("file:place"));
  CHECK_FALSE(has_place_terms("replace:x"));       // not an operator the pack knows
  CHECK_FALSE(has_place_terms("--place:x"));
}

namespace {

// A Places stand-in: "portugal" is two files, posted in two batches; "slow"
// waits for release(); "fail" returns an error having posted nothing.
class place_client final : public mv::addon::slot_client {
 public:
  explicit place_client(slot_registry& r) : r_(r) {}
  status info_section(const mv_slot_info_request&) override { return status::ok; }
  status search(const mv_slot_search_request& q) override {
    ++searches;
    const std::string f = q.folded_utf8;
    {
      std::lock_guard lock(m);
      folders.emplace_back(q.folder_utf8);
      generations.push_back(q.generation);
    }
    if (f == "fail") return status::io;
    if (f == "slow") {
      std::unique_lock lock(m);
      cv.wait(lock, [this] { return released; });
      stale_seen = r_.is_stale("places", q.generation);
      return status::ok;
    }
    if (f == "portugal") {
      (void)r_.post_search("places", q.provider_id, q.generation, {{"/i/lisboa.jpg", "Lisbon, Portugal"}}, false);
      (void)r_.post_search("places", q.provider_id, q.generation, {{"/i/porto.jpg", "Porto, Portugal"}}, true);
      return status::ok;
    }
    return r_.post_search("places", q.provider_id, q.generation, {}, true);
  }
  status screen_open(const mv_slot_screen_request&) override { return status::unsupported_format; }
  status badges(const mv_slot_badges_request&) override { return status::unsupported_format; }
  void event(const mv_slot_event&) override {}

  void release() {
    {
      std::lock_guard lock(m);
      released = true;
    }
    cv.notify_all();
  }

  std::atomic<int> searches{0};
  std::atomic<bool> stale_seen{false};
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  std::vector<std::string> folders;
  std::vector<std::uint64_t> generations;

 private:
  slot_registry& r_;
};

mv::addon::contributions places_point() {
  mv::addon::contributions c;
  c.api_min = 5;
  c.api_max = 5;
  c.search_providers.push_back({"place", "Places", {"place:", "in:"}, true, true});
  return c;
}

std::vector<std::string> sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
}

int32_t MV_CALL collect_path(void* user, const char* path) {
  auto& out = *static_cast<std::vector<std::string>*>(user);
  if (path) out.emplace_back(path);
  return 1;
}

}  // namespace

TEST_CASE("Local search's place terms are the pack's: no provider is asked beside it", "[addon][slots][places]") {
  slot_registry r;
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  auto places = std::make_unique<place_client>(r);
  place_client* pc = places.get();
  REQUIRE(r.attach("places", "Places", places_point(), std::move(places)) == status::ok);

  // "place:portugal" and "beach place:portugal": the pack intersects; nobody
  // posts "+ N in Portugal" rows beside its results.
  for (const char* q : {"place:portugal", "beach place:portugal", "-place:spain"}) {
    r.search(MV_SLOT_SCOPE_LOCAL, q, "");
    r.drain();
    const auto res = mv::json::parse(r.search_json(MV_SLOT_SCOPE_LOCAL));
    CHECK(res->find("providers")->a.empty());
    CHECK(res->find("rows")->a.empty());
  }
  CHECK(fc->searches == 0);
  CHECK(pc->searches == 0);
  // A plain word still goes to every Local search provider (Places' "portugal").
  r.search(MV_SLOT_SCOPE_LOCAL, "portugal", "");
  r.drain();
  CHECK(pc->searches == 1);
  // File search keeps its routing: "place:lisbon" is Places' alone.
  r.search(MV_SLOT_SCOPE_FILES, "place:portugal", "/f");
  r.drain();
  CHECK(pc->searches == 2);
  CHECK(fc->searches == 1);  // "portugal" above; not the routed one
}

TEST_CASE("a place term's assets come from the provider that declares place:, or none", "[addon][slots][places]") {
  slot_registry r;
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::make_unique<fake_client>(r, "slotstest")) ==
          status::ok);
  const auto keep_going = [] { return true; };

  // No add-on declares place: (Places not installed): not_found, no rows.
  CHECK(mv::addon::place_terms(r, "") == 0u);
  CHECK(mv::addon::place_terms(r, "place:portugal") == MV_SLOTS_PLACE_TERMS);
  auto none = r.provider_paths("place:", "portugal", keep_going);
  REQUIRE_FALSE(none);
  CHECK(none.error() == status::not_found);

  auto places = std::make_unique<place_client>(r);
  place_client* pc = places.get();
  REQUIRE(r.attach("places", "Places", places_point(), std::move(places)) == status::ok);
  CHECK(mv::addon::place_terms(r, "") == MV_SLOTS_PLACE_PROVIDER);
  CHECK(mv::addon::place_terms(r, "in:sintra") == (MV_SLOTS_PLACE_TERMS | MV_SLOTS_PLACE_PROVIDER));
  CHECK(mv::addon::place_terms(r, "in:2024") == MV_SLOTS_PLACE_PROVIDER);

  SECTION("every batch, over the whole index, beside the chrome's own search") {
    const std::uint64_t chrome = r.search(MV_SLOT_SCOPE_FILES, "nothing", "/f");
    auto paths = r.provider_paths("place:", " Portugal ", keep_going);
    REQUIRE(paths);
    CHECK(sorted(*paths) == std::vector<std::string>{"/i/lisboa.jpg", "/i/porto.jpg"});
    CHECK(pc->folders.back().empty());  // the whole index (locations_of_index)
    CHECK(r.is_stale(pc->generations.back()));  // finished: its generation is gone
    CHECK_FALSE(r.is_stale(chrome));            // and the chrome's search was not replaced
    // Nothing of it reached the chrome's results.
    CHECK(mv::json::parse(r.search_json(MV_SLOT_SCOPE_LOCAL))->find("rows")->a.empty());
  }
  SECTION("a provider that fails says nothing: done, no rows") {
    auto paths = r.provider_paths("place:", "fail", keep_going);
    REQUIRE(paths);
    CHECK(paths->empty());
  }
  SECTION("the caller stops waiting; the provider sees its request go stale") {
    std::atomic<int> asked{0};
    auto paths = r.provider_paths("place:", "slow", [&] { return ++asked < 3; });
    REQUIRE_FALSE(paths);
    CHECK(paths.error() == status::cancelled);
    pc->release();
    r.drain();
    CHECK(pc->stale_seen);
  }
  SECTION("the provider is removed while it works") {
    std::thread remover([&] {
      (void)eventually([&] { return pc->searches.load() == 1; });
      pc->release();  // detach joins the worker: let the call return
      r.detach("places");
    });
    auto paths = r.provider_paths("place:", "slow", keep_going);
    remover.join();
    // Either it finished before the detach (no rows) or it went: never a hang.
    if (!paths) CHECK(paths.error() == status::not_found);
    CHECK(mv::addon::place_terms(r, "") == 0u);
  }
}

TEST_CASE("the host table's place_matches serves the AI pack the provider's paths", "[addon][slots][places]") {
  slot_registry r;
  mv::addon::host_services svc;
  svc.slots = &r;
  mv::addon::host_table t(svc);
  t.set_addon_id("ai");
  const mv_host_api* api = t.api();
  REQUIRE(api->struct_size >= offsetof(mv_host_api, place_matches) + sizeof(api->place_matches));
  REQUIRE(api->place_matches);
  std::vector<std::string> out;
  CHECK(api->place_matches(api->host, "portugal", &collect_path, &out) == MV_ERR_NOT_FOUND);
  CHECK(out.empty());
  CHECK(api->place_matches(api->host, nullptr, &collect_path, &out) == MV_ERR_INVALID_ARG);

  REQUIRE(r.attach("places", "Places", places_point(), std::make_unique<place_client>(r)) == status::ok);
  REQUIRE(api->place_matches(api->host, "portugal", &collect_path, &out) == MV_OK);
  CHECK(sorted(out) == std::vector<std::string>{"/i/lisboa.jpg", "/i/porto.jpg"});
  // The visitor stops it.
  const auto stop = [](void*, const char*) -> int32_t { return 0; };
  CHECK(api->place_matches(api->host, "portugal", stop, nullptr) == MV_ERR_CANCELLED);
}

namespace {

// What a locations_of_index walk handed the add-on.
struct index_walk {
  std::vector<std::string> paths;
  std::size_t stop_after = 0;  // 0: never stop
};

int32_t MV_CALL collect_index_location(void* user, const mv_addon_location_entry* e) {
  auto& w = *static_cast<index_walk*>(user);
  w.paths.emplace_back(e->path_utf8);
  return w.stop_after != 0 && w.paths.size() >= w.stop_after ? 0 : 1;
}

mv_addon_location_entry located(const std::string& path, double lat, double lon) {
  mv_addon_location_entry e{};
  e.path_utf8 = path.c_str();
  e.location.has = 1;
  e.location.lat = lat;
  e.location.lon = lon;
  return e;
}

// mv.ai.1's each_location over a fixed list, as the AI pack's index serves it.
int32_t g_fake_ai_rows = 0;
mv_status MV_CALL fake_ai_each_location(void*, mv_addon_location_fn visit, void* user) {
  for (int32_t i = 0; i < g_fake_ai_rows; ++i) {
    const std::string key = "photos:" + std::to_string(i);
    const mv_addon_location_entry e = located(key, 38.7 + i * 0.001, -9.1);
    if (visit(user, &e) == 0) return MV_ERR_CANCELLED;
  }
  return MV_OK;
}

}  // namespace

TEST_CASE("locations_of_index streams the index's coordinates, generation-tagged, reading no file",
          "[addon][slots]") {
  slot_registry r;
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::make_unique<fake_client>(r, "slotstest")) ==
          status::ok);
  std::uint64_t g = r.search(MV_SLOT_SCOPE_LOCAL, "test: portugal", "");
  r.drain();

  // A fake index of 1000 located assets, plus rows the host drops (no
  // coordinates, no key). Every 100th row can make the request go stale.
  std::atomic<int> walks{0};
  std::atomic<int> stale_at{-1};
  mv::addon::host_services svc;
  svc.slots = &r;
  svc.index_locations = [&](const mv::addon::index_location_visit& visit) -> mv::expected {
    ++walks;
    for (int i = 0; i < 1000; ++i) {
      if (i == stale_at) (void)r.search(MV_SLOT_SCOPE_LOCAL, "test: spain", "");
      const std::string key = "/index/" + std::to_string(i) + ".jpg";
      if (!visit(located(key, 38.0, -9.0))) return mv::err(status::cancelled);
    }
    mv_addon_location_entry none{};
    none.path_utf8 = "/index/screenshot.png";
    if (!visit(none)) return mv::err(status::cancelled);
    mv_addon_location_entry nameless = located("", 1, 1);
    nameless.path_utf8 = nullptr;
    if (!visit(nameless)) return mv::err(status::cancelled);
    return {};
  };
  mv::addon::host_table t(svc);
  t.set_addon_id("slotstest");
  const mv_host_api* api = t.api();
  REQUIRE(api->struct_size >= offsetof(mv_host_api, locations_of_index) + sizeof(api->locations_of_index));
  REQUIRE(api->locations_of_index);

  SECTION("every located asset, once") {
    index_walk w;
    REQUIRE(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_OK);
    CHECK(w.paths.size() == 1000);
    CHECK(w.paths.front() == "/index/0.jpg");
    CHECK(walks == 1);
  }
  SECTION("the add-on's visit stops it, as locations_in: MV_OK") {
    index_walk w;
    w.stop_after = 10;
    REQUIRE(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_OK);
    CHECK(w.paths.size() == 10);
  }
  SECTION("a stale generation never reaches the index") {
    (void)r.search(MV_SLOT_SCOPE_LOCAL, "test: spain", "");
    index_walk w;
    CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_ERR_CANCELLED);
    CHECK(walks == 0);
    CHECK(w.paths.empty());
  }
  SECTION("a request going stale mid-walk stops it within 64 rows") {
    stale_at = 100;
    index_walk w;
    CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_ERR_CANCELLED);
    CHECK(w.paths.size() >= 100);
    CHECK(w.paths.size() <= 164);
  }
  SECTION("a removed add-on's walk stops: detach does not wait it out") {
    r.detach("slotstest");
    index_walk w;
    CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_ERR_CANCELLED);
    CHECK(walks == 0);
  }
  SECTION("NULL visit") {
    CHECK(api->locations_of_index(api->host, g, nullptr, nullptr) == MV_ERR_INVALID_ARG);
  }
}

TEST_CASE("the index's coordinates come from the AI pack while it is published, and stop when it goes",
          "[addon][slots]") {
  slot_registry r;
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::make_unique<fake_client>(r, "slotstest")) ==
          status::ok);
  const std::uint64_t g = r.search(MV_SLOT_SCOPE_LOCAL, "test: portugal", "");
  r.drain();
  mv::addon::host_services svc;  // no index_locations: the process's source
  svc.slots = &r;
  mv::addon::host_table t(svc);
  t.set_addon_id("slotstest");
  const mv_host_api* api = t.api();

  // Nothing published (the AI pack not loaded): not found, nothing visited.
  mv::addon::retract_index_locations();
  index_walk w;
  CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_ERR_NOT_FOUND);
  CHECK(w.paths.empty());

  // An older pack (its mv.ai.1 table stops before each_location) is no source.
  mv_ai_api ai{};
  ai.struct_size = static_cast<uint32_t>(offsetof(mv_ai_api, each_location));
  ai.each_location = &fake_ai_each_location;
  CHECK_FALSE(mv::addon::ai_index_locations(&ai));
  CHECK_FALSE(mv::addon::ai_index_locations(nullptr));

  // The pack's each_location, through the published source.
  ai.struct_size = sizeof(mv_ai_api);
  g_fake_ai_rows = 700;
  mv::addon::publish_index_locations(mv::addon::ai_index_locations(&ai));
  CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w) == MV_OK);
  CHECK(w.paths.size() == 700);
  CHECK(w.paths[3] == "photos:3");

  // Retracted while a walk is inside the source: the walk stops at its next
  // row (MV_ERR_NOT_FOUND: the index went), and wait_index_locations_idle
  // returns only once it has left.
  std::atomic<bool> inside{false}, release{false};
  mv::addon::publish_index_locations([&](const mv::addon::index_location_visit& visit) -> mv::expected {
    const std::string key = "/index/a.jpg";
    if (!visit(located(key, 1, 1))) return mv::err(status::cancelled);
    inside = true;
    while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!visit(located(key, 1, 1))) return mv::err(status::cancelled);
    return {};
  });
  index_walk w2;
  mv_status walked = MV_OK;
  std::thread walker([&] { walked = api->locations_of_index(api->host, g, &collect_index_location, &w2); });
  REQUIRE(eventually([&] { return inside.load(); }));
  mv::addon::retract_index_locations();
  std::atomic<bool> idle{false};
  std::thread waiter([&] {
    mv::addon::wait_index_locations_idle();
    idle = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK_FALSE(idle.load());
  release = true;
  walker.join();
  waiter.join();
  CHECK(idle.load());
  CHECK(walked == MV_ERR_NOT_FOUND);
  CHECK(w2.paths.size() == 1);
  // And a new walk finds no index.
  CHECK(api->locations_of_index(api->host, g, &collect_index_location, &w2) == MV_ERR_NOT_FOUND);
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

TEST_CASE("notices and keep-alive are tables the hosts read; a notice's button reaches the add-on (#452)",
          "[addon][slots][screens]") {
  slot_registry r;
  std::atomic<std::uint32_t> bits{0};
  r.set_listener([&](std::uint32_t b) { bits |= b; });
  // Before attach (an add-on calling from mv_addon_get): not found, so it retries.
  REQUIRE(r.keep_alive("slotstest", true, "explorer", "Slots: on") == status::not_found);
  auto client = std::make_unique<fake_client>(r, "slotstest");
  fake_client* fc = client.get();
  REQUIRE(r.attach("slotstest", "Slots test", every_point(), std::move(client)) == status::ok);
  REQUIRE(*mv::json::parse(r.notices_json())->integer("ask") == 0);
  REQUIRE_FALSE(r.keeping_alive());

  bits = 0;
  slot_registry::notice_fields n{"explorer", "Slots", "Ann wants to send you files.", "decline", "Decline"};
  REQUIRE(r.post_notice("slotstest", "offer", n) == status::ok);
  REQUIRE((bits & mv::addon::slot_changed_notices) != 0);
  auto doc = mv::json::parse(r.notices_json());
  REQUIRE(doc->find("notices")->a.size() == 1);
  const auto& first = doc->find("notices")->a[0];
  CHECK(*first.str("addon") == "slotstest");
  CHECK(*first.str("id") == "offer");
  CHECK(*first.str("screen") == "explorer");
  CHECK(*first.str("body") == "Ann wants to send you files.");
  CHECK(*first.str("action") == "decline");
  const auto rev = *first.integer("revision");
  // The same id replaces it, under a new revision (the host shows it again).
  n.body = "Ann wants to send you 3 files, 2 MB.";
  REQUIRE(r.post_notice("slotstest", "offer", n) == status::ok);
  doc = mv::json::parse(r.notices_json());
  REQUIRE(doc->find("notices")->a.size() == 1);
  CHECK(*doc->find("notices")->a[0].integer("revision") > rev);

  // Bounds: a screen it does not have, no title, text too long, a button without a label.
  slot_registry::notice_fields bad = n;
  bad.screen = "nowhere";
  CHECK(r.post_notice("slotstest", "offer", bad) == status::not_found);
  bad = n;
  bad.title.clear();
  CHECK(r.post_notice("slotstest", "offer", bad) == status::invalid_arg);
  bad = n;
  bad.body = std::string(401, 'x');
  CHECK(r.post_notice("slotstest", "offer", bad) == status::invalid_arg);
  bad = n;
  bad.action_label.clear();
  CHECK(r.post_notice("slotstest", "offer", bad) == status::invalid_arg);
  CHECK(r.post_notice("slotstest", "", n) == status::invalid_arg);
  CHECK(r.post_notice("ghost", "offer", n) == status::not_found);

  // The button: an event to the add-on, never checked against a tree (no window open).
  REQUIRE(r.notice_action("slotstest", "offer", "decline") == status::ok);
  CHECK(r.notice_action("slotstest", "offer", "accept") == status::not_found);  // not its button
  CHECK(r.notice_action("slotstest", "other", "decline") == status::not_found);
  r.drain();
  {
    std::lock_guard lock(fc->m);
    REQUIRE(fc->events.size() == 1);
    CHECK(fc->events[0] == "13:decline:\"offer\"");
  }
  // Taken back; taking back what is gone wakes nobody.
  REQUIRE(r.post_notice("slotstest", "offer", std::nullopt) == status::ok);
  CHECK(mv::json::parse(r.notices_json())->find("notices")->a.empty());
  bits = 0;
  REQUIRE(r.post_notice("slotstest", "offer", std::nullopt) == status::ok);
  CHECK(bits == 0);

  // Permission asks count up; the hosts ask the OS when the count moves.
  REQUIRE(r.ask_notice_permission("slotstest") == status::ok);
  CHECK(*mv::json::parse(r.notices_json())->integer("ask") == 1);

  // Keep-alive, with the screen's title for the host's "Open …".
  REQUIRE(r.keep_alive("slotstest", true, "explorer", "Slots: on") == status::ok);
  REQUIRE(r.keeping_alive());
  doc = mv::json::parse(r.notices_json());
  REQUIRE(doc->find("keep")->a.size() == 1);
  CHECK(*doc->find("keep")->a[0].str("title") == "Explorer");
  CHECK(*doc->find("keep")->a[0].str("label") == "Slots: on");
  bits = 0;
  REQUIRE(r.keep_alive("slotstest", true, "explorer", "Slots: on") == status::ok);
  CHECK(bits == 0);  // unchanged: no wake
  CHECK(r.keep_alive("slotstest", true, "nowhere", "x") == status::not_found);
  REQUIRE(r.keep_alive("slotstest", false, "", "") == status::ok);
  CHECK_FALSE(r.keeping_alive());

  // #510: busy, for Restart now and a live reload to wait on.
  bits = 0;
  REQUIRE(r.post_busy("slotstest", "a Slots transfer") == status::ok);
  CHECK((bits & mv::addon::slot_changed_notices) != 0);
  doc = mv::json::parse(r.notices_json());
  REQUIRE(doc->find("busy")->a.size() == 1);
  CHECK(*doc->find("busy")->a[0].str("addon") == "slotstest");
  CHECK(*doc->find("busy")->a[0].str("what") == "a Slots transfer");
  bits = 0;
  REQUIRE(r.post_busy("slotstest", "a Slots transfer") == status::ok);
  CHECK(bits == 0);  // unchanged: no wake
  CHECK(r.post_busy("slotstest", std::string(121, 'x')) == status::invalid_arg);
  CHECK(r.post_busy("nobody", "x") == status::not_found);
  REQUIRE(r.post_busy("slotstest", "") == status::ok);
  CHECK(mv::json::parse(r.notices_json())->find("busy")->a.empty());

  // Detach takes them all with it.
  REQUIRE(r.keep_alive("slotstest", true, "explorer", "Slots: on") == status::ok);
  REQUIRE(r.post_notice("slotstest", "offer", n) == status::ok);
  REQUIRE(r.post_busy("slotstest", "a Slots transfer") == status::ok);
  r.detach("slotstest");
  CHECK_FALSE(r.keeping_alive());
  CHECK(mv::json::parse(r.notices_json())->find("notices")->a.empty());
  CHECK(mv::json::parse(r.notices_json())->find("busy")->a.empty());
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
  // Ids no node names are kept (pixels may come first), but not without limit.
  int kept = 0;
  for (int i = 0; i < 40; ++i) {
    if (r.post_surface("slotstest", "spot", "extra" + std::to_string(i), g, px.data(), 8, 2, 48) == status::ok) ++kept;
  }
  REQUIRE(kept == 15);  // 16 per screen, "pixels" included
  REQUIRE(r.post_surface("slotstest", "spot", "pixels", g, px.data(), 8, 2, 48) == status::ok);  // a known id still updates
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
  "tile_badges": [{"id": "geotagged", "label": "Slots test badge", "symbol": "check"}],
  "settings": {"title": "Slots test", "sections": [{"title": "Test", "items": [
    {"type": "text", "key": "suffix", "label": "Label suffix", "max_length": 16, "default": ""}]}]}
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
  REQUIRE(check(k, with(R"("min": 4, "max": 5)", R"("min": 9, "max": 10)")) == rejection::needs_update);
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
  // #512: a status item names the screen it opens from contribution API 7; an
  // add-on written for less cannot.
  const std::string status_opens = R"({"id": "count", "label": "Slots test", "opens": "explorer"})";
  REQUIRE(check(k, with(R"({"id": "count", "label": "Slots test"})", status_opens)) == rejection::malformed);
  {
    std::string s = with(R"({"id": "count", "label": "Slots test"})", status_opens);
    s.replace(s.find(R"("min": 4, "max": 5)"), 18, R"("min": 7, "max": 7)");
    const std::string m7 = slots_manifest("lib.bin", bytes, s);
    const auto d7 = mv::addon::check_manifest(bytes_of(m7), k.sign(m7), k.pub, MV_ADDON_HOST_API);
    REQUIRE(d7.trusted());
    REQUIRE(d7.m.slots.items[3].opens == "explorer");
  }
  // A settings page (contribution API 8): one, never opened by an item, never
  // asked for by an add-on written for less.
  const std::string page = R"({"id": "explorer", "title": "Explorer", "place": "window", "width": 640, "height": 480},
    {"id": "prefs", "title": "Explorer", "place": "settings"})";
  const std::string screen = R"({"id": "explorer", "title": "Explorer", "place": "window", "width": 640, "height": 480})";
  REQUIRE(check(k, with(screen, page)) == rejection::malformed);  // API 4-5
  const auto at8 = [&](std::string s) {
    s.replace(s.find(R"("min": 4, "max": 5)"), 18, R"("min": 8, "max": 8)");
    return s;
  };
  {
    const std::string m8 = slots_manifest("lib.bin", bytes, at8(with(screen, page)));
    const auto d8 = mv::addon::check_manifest(bytes_of(m8), k.sign(m8), k.pub, MV_ADDON_HOST_API);
    REQUIRE(d8.trusted());
    REQUIRE(d8.m.slots.screens.size() == 2);
    CHECK(d8.m.slots.screens[1].place == mv::addon::screen_place::settings);
  }
  const std::string two_pages = page + R"(, {"id": "more", "title": "More", "place": "settings"})";
  CHECK(check(k, at8(with(screen, two_pages))) == rejection::malformed);
  {
    std::string s = at8(with(screen, page));
    s.replace(s.find(R"("opens": "explorer")"), 19, R"("opens": "prefs")");
    CHECK(check(k, s) == rejection::malformed);
  }
  // An add-on with no screens or slots is unaffected.
  REQUIRE(check(k, "", 1, 1) == rejection::none);
}

TEST_CASE("an open add-on cannot fill screens or slots until scripts can (PR 57)",
          "[addon][slots][manifest]") {
  // The contribution API this host serves names them...
  REQUIRE(mv::addon::kContributionApi == 8);
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
  mv::addon::reset_settings_cache_for_tests();
  mv::io::set_addon_settings_dir_override(utf8(s / "Add-on Settings"));  // never the real profile
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
  std::atomic<bool> yield_now{false};  // "the viewer is presenting"
  svc.should_yield = [&yield_now] { return yield_now.load(); };
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

  // A cached walk does not wait while the viewer presents: a cache hit reads nothing.
  yield_now = true;
  const auto cached_start = std::chrono::steady_clock::now();
  reg.search(MV_SLOT_SCOPE_FILES, "portugal", utf8(folder));
  reg.drain();
  CHECK(std::chrono::steady_clock::now() - cached_start < std::chrono::milliseconds(500));
  REQUIRE(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.size() == 2);

  // Removed while its worker is inside a walk that waits out the viewer's
  // presents (uncached files): the walk sees its add-on going and stops, so
  // the remove does not wait for the folder (4 files x 2 s here).
  mv::addon::clear_location_cache();
  const auto searches_before = probe->calls(1);
  reg.search(MV_SLOT_SCOPE_FILES, "portugal", utf8(folder));
  REQUIRE(eventually([&] { return probe->calls(1) > searches_before; }));
  const auto remove_start = std::chrono::steady_clock::now();

  // Removing the add-on removes every item, with no restart.
  loaded->reset();
  CHECK(std::chrono::steady_clock::now() - remove_start < std::chrono::milliseconds(500));
  yield_now = false;
  REQUIRE_FALSE(reg.attached("slots-test"));
  REQUIRE(mv::json::parse(reg.contributions_json())->find("items")->a.empty());
  REQUIRE(reg.status_json() == "[]");
  REQUIRE(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());
  REQUIRE(reg.badges_for(utf8(folder / "lisboa.jpg")).empty());
  mv::io::set_addon_settings_dir_override("");
  mv::addon::reset_settings_cache_for_tests();
}

TEST_CASE("remove without keep calls the add-on's forget before its files go; keep does not",
          "[addon][slots]") {
  keypair k;
  scratch_dir s("addons_forget");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::vector<std::uint8_t> module = read_bytes(MV_SLOTS_TEST_MODULE_PATH);
  const std::string native = utf8(fs::path(MV_SLOTS_TEST_MODULE_PATH).filename());
  const std::string m = slots_manifest(native, module, kContributes);
  const auto install = [&] {
    auto staged = st.make_staging();
    REQUIRE(staged);
    write_bytes(fs::path(*staged) / fs::path(native), module);
    write_text(fs::path(*staged) / "manifest.json", m);
    write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
    auto done = st.install(*staged);
    REQUIRE(done);
    return done->data_dir;
  };
  const std::string data_dir = install();
  const fs::path marker = fs::path(data_dir).concat(".forgotten");

  // Keep data: the add-on is not asked to forget anything.
  REQUIRE(st.remove("slots-test", true));
  CHECK_FALSE(fs::exists(marker));

  REQUIRE(install() == data_dir);
  mv::addon::forget_outside_data(st, "slots-test");
  CHECK(fs::exists(marker));
  REQUIRE(st.remove("slots-test", false));
  // Not installed: nothing to load, nothing to call.
  fs::remove(marker);
  mv::addon::forget_outside_data(st, "slots-test");
  CHECK_FALSE(fs::exists(marker));
}

TEST_CASE("a changed setting re-asks the add-on once, and settings_get serves the new value",
          "[addon][slots][settings]") {
  keypair k;
  scratch_dir s("addons_slots_settings");
  mv::addon::reset_settings_cache_for_tests();
  mv::io::set_addon_settings_dir_override(utf8(s / "Add-on Settings"));  // never the real profile
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const mv::addon::open_store open(utf8(s / "Open Add-ons"));
  const std::vector<std::uint8_t> module = read_bytes(MV_SLOTS_TEST_MODULE_PATH);
  const std::string native = utf8(fs::path(MV_SLOTS_TEST_MODULE_PATH).filename());
  const std::string m = slots_manifest(native, module, kContributes);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / fs::path(native), module);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));
  const fs::path folder = s / "trip";
  fs::create_directories(folder);
  write_text(folder / "lisboa.jpg", "x");
  mv::addon::clear_location_cache();

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  svc.location = [](const std::string&) -> std::optional<mv_addon_location> {
    mv_addon_location l{};
    l.has = 1;
    l.lat = 38.72;
    l.lon = -9.14;
    return l;
  };
  auto loaded = mv::addon::loaded_addon::load(st, "slots-test", svc);
  REQUIRE(loaded);
  struct test_api {
    uint32_t struct_size;
    uint32_t reserved;
    void (*last_event)(char* out, uint32_t cap);
    uint64_t (*calls)(uint32_t which);
    mv_status (*setting)(const char* key, char* out, uint32_t cap);
  };
  const auto* probe = static_cast<const test_api*>((*loaded)->query("mv.slots_test.1"));
  REQUIRE(probe);
  const auto value = [&](const char* key) {
    char buf[128] = {};
    return probe->setting(key, buf, sizeof buf) == MV_OK ? std::string(buf) : std::string("<error>");
  };
  REQUIRE(value("suffix") == "\"\"");          // the page's default
  REQUIRE(value("not_on_page") == "<error>");  // MV_ERR_IO: not this add-on's key

  const auto theme = test_theme();
  const std::string coords = "38.72000\xC2\xB0 N, 9.14000\xC2\xB0 W";
  const auto first = reg.request_info(utf8(folder / "lisboa.jpg"), coords, theme, 300, 2);
  reg.request_badges(utf8(folder));
  reg.drain();
  REQUIRE(reg.info_json().find("\"Portugal\"") != std::string::npos);
  const auto info_calls = probe->calls(0);
  const auto badge_calls = probe->calls(3);

  // Through the same path the Settings pages use (mv_addon_setting_set).
  const auto set = mv::json::parse(mv::addon::addon_setting_set_json(open, &st, "slots-test", "suffix", "\"North\""));
  REQUIRE(set->find("ok")->b);
  reg.drain();
  CHECK(probe->calls(0) == info_calls + 1);  // exactly one new info request
  CHECK(probe->calls(3) == badge_calls + 1);  // and one badges request: it has badges
  CHECK(value("suffix") == "\"North\"");
  const std::string info = reg.info_json();
  CHECK(info.find("\"Portugal North\"") != std::string::npos);
  // The file's request keeps its generation (the chromes see one file); the
  // section's own moved, and a late answer for the old one is dropped.
  CHECK(mv::json::parse(info)->integer("generation") == static_cast<std::int64_t>(first));
  CHECK(reg.is_stale(first) == false);
  CHECK(reg.badges_for(utf8(folder / "lisboa.jpg")).size() == 1);

  // The same value again changes nothing and asks nobody.
  REQUIRE(mv::json::parse(mv::addon::addon_setting_set_json(open, &st, "slots-test", "suffix", "\"North\""))
              ->find("ok")
              ->b);
  reg.drain();
  CHECK(probe->calls(0) == info_calls + 1);

  // Values written for another publisher's add-on under this id are never
  // served through the host table: the add-on reads its default.
  const mv::addon::settings_values values(utf8(s / "Add-on Settings"));
  const auto installed = st.find("slots-test");
  REQUIRE(installed);
  REQUIRE(values.set("slots-test", std::string(64, 'a'), installed->m.settings, "suffix", "\"Stranger\""));
  reg.drain();
  CHECK(value("suffix") == "\"\"");
  CHECK(reg.info_json().find("Stranger") == std::string::npos);
  CHECK(reg.info_json().find("\"Portugal\"") != std::string::npos);

  // Forgetting (Remove without keeping) is a change too: back to defaults.
  REQUIRE(mv::addon::addon_setting_set_json(open, &st, "slots-test", "suffix", "\"South\"").find("true") !=
          std::string::npos);
  reg.drain();
  CHECK(reg.info_json().find("\"Portugal South\"") != std::string::npos);
  const auto before_forget = probe->calls(0);
  REQUIRE(mv::addon::addon_settings_forget("slots-test"));
  reg.drain();
  CHECK(probe->calls(0) == before_forget + 1);
  CHECK(reg.info_json().find("\"Portugal\"") != std::string::npos);

  // With nothing on screen, a change asks nothing.
  reg.clear_info();
  const auto idle = probe->calls(0);
  REQUIRE(mv::addon::addon_setting_set_json(open, &st, "slots-test", "suffix", "\"East\"").find("true") !=
          std::string::npos);
  reg.drain();
  CHECK(probe->calls(0) == idle);

  loaded->reset();
  mv::io::set_addon_settings_dir_override("");
  mv::addon::reset_settings_cache_for_tests();
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

// Issue #402: every window loads the slot add-ons, so a later window finds them
// by their signed manifests alone, hashing no file (it must not hash the AI
// pack to find Places); the load itself still verifies the one it loads.
TEST_CASE("slot add-ons are found by manifest without hashing; the load still verifies",
          "[addon][slots]") {
  keypair k;
  scratch_dir s("addons_slots_listed");
  const fs::path root = s / "addons";
  fs::create_directories(root);
  mv::addon::store st(utf8(root), k.pub, MV_ADDON_HOST_API);
  const std::vector<std::uint8_t> module = read_bytes(MV_SLOTS_TEST_MODULE_PATH);
  const std::string native = utf8(fs::path(MV_SLOTS_TEST_MODULE_PATH).filename());
  const std::string m = slots_manifest(native, module, kContributes);
#if defined(__APPLE__)
  const fs::path dir = root / "Slots Test" / "1.0.0";  // the store's folder: the name on the Mac
#else
  const fs::path dir = root / "slots-test" / "1.0.0";  // the id elsewhere
#endif
  fs::create_directories(dir);
  write_text(dir / "manifest.json", m);
  write_bytes(dir / "manifest.json.sig", k.sign(m));
  // The library differs from what the manifest hashed: only a verify sees it.
  std::vector<std::uint8_t> tampered = module;
  tampered[tampered.size() / 2] ^= 0xFF;
  write_bytes(dir / fs::path(native), tampered);
  // A download in progress is not an install.
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));

  const auto listed = st.manifests();
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].m.id == "slots-test");
  CHECK(listed[0].state == mv::addon::install_state::ok);
  CHECK(mv::addon::loads_for_slots(listed[0].m));

  // list() hashes, and finds what manifests() never looked at.
  bool seen = false;
  for (const mv::addon::installed& i : st.list()) {
    if (i.id != "slots-test") continue;
    seen = true;
    CHECK(i.state == mv::addon::install_state::invalid);
  }
  CHECK(seen);
  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  auto loaded = mv::addon::loaded_addon::load(st, "slots-test", svc);
  CHECK_FALSE(loaded);
  CHECK_FALSE(reg.attached("slots-test"));
}

TEST_CASE("only add-ons that fill slots, and not Import, the AI family or a piece, load for them",
          "[addon][slots]") {
  keypair k;
  const std::vector<std::uint8_t> bytes{1, 2, 3};
  auto parsed = [&](const std::string& id) {
    const std::string m = slots_manifest("lib.bin", bytes, kContributes, 3, 3, id);
    const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
    REQUIRE(d.trusted());
    return d.m;
  };
  CHECK(mv::addon::loads_for_slots(parsed("places")));
  CHECK_FALSE(mv::addon::loads_for_slots(parsed("import")));
  CHECK_FALSE(mv::addon::loads_for_slots(parsed("ai")));
  CHECK_FALSE(mv::addon::loads_for_slots(parsed("ai-faces")));
  mv::addon::manifest piece = parsed("places");
  piece.part_of = "places";
  CHECK_FALSE(mv::addon::loads_for_slots(piece));
  mv::addon::manifest none = parsed("places");
  none.slots = {};
  CHECK_FALSE(mv::addon::loads_for_slots(none));
}
#endif
