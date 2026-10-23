// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Enhance add-on through the real loader (docs/plans/enhance.md E1;
// docs/design/25 PRs 58-59).
//
// Its manifest (src/addons/enhance/manifest.json, which tools/package/
// addon-pack.py packs) is accepted and declares exactly its Models page in
// Settings. Then mv_enhance is installed in a signed store with the fixture
// catalogue (tests/data/enhance/fixture-models.json) staged as
// enhance-models.py stage lays it out: models/enhance.json, a
// models/<id>/model.json per model, and the pins in the manifest's `remote`.
// It loads with no model present and makes nothing; its page opens, draws
// the fixture's models and takes a Delete. Only on Linux, where there is no
// transport (fetch_none.cpp), is Download clicked: it stops before any
// request. macOS and Windows would reach Hugging Face, so they never click it
// here; tests/test_enhance.cpp drives the store against a fake one everywhere.
#include "catch_compat.h"

#include <sodium.h>

#include <chrono>
#include <cstdint>
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
namespace scr = mv::addon::screen;

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

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 1000; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

struct packed_file {
  std::string path;
  std::vector<std::uint8_t> bytes;
  std::string licence;
};

std::string json_text(const mv::json::value& v) {
  mv::json::writer w;
  scr::write_value(w, v);
  return w.take();
}

// The fixture catalogue as `enhance-models.py stage --upstream remote` writes
// it: the version folder's files, and the `remote` array for the manifest.
struct staged_catalogue {
  std::vector<packed_file> files;
  std::string remote;
};

staged_catalogue stage_fixture() {
  const std::vector<std::uint8_t> raw = read_bytes(fs::path(MV_ENHANCE_TEST_DATA) / "fixture-models.json");
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  const mv::json::value* kinds = doc->find("kinds");
  const mv::json::value* models = doc->find("models");
  REQUIRE(kinds);
  REQUIRE(models);
  staged_catalogue out;
  mv::json::writer index;
  index.begin_object().key("schema").integer(1).key("kinds").begin_object();
  for (const char* k : {"still", "video", "audio"}) {
    const mv::json::value* e = kinds->find(k);
    REQUIRE(e);
    index.key(k).begin_object();
    for (const char* field : {"name", "unit", "cpu_budget_ms", "models"}) {
      REQUIRE(e->find(field));
      index.key(field).raw(json_text(*e->find(field)));
    }
    index.end_object();
  }
  index.end_object().end_object();
  out.files.push_back({"models/enhance.json", bytes(index.take()), "GPL-3.0-or-later"});
  mv::json::writer remote;
  remote.begin_array();
  for (const auto& [folder, entry] : models->o) {
    REQUIRE(entry.find("model"));
    REQUIRE(entry.find("files"));
    out.files.push_back({folder + "/model.json", bytes(json_text(*entry.find("model"))), "GPL-3.0-or-later"});
    for (const mv::json::value& f : entry.find("files")->a) {
      const std::string url =
          "https://huggingface.co/" + *f.str("repo") + "/resolve/" + *f.str("revision") + "/" + *f.str("file");
      remote.begin_object().key("url").string(url).key("path").string(folder + "/" + *f.str("as"));
      remote.key("size").integer(*f.integer("size")).key("sha256").string(*f.str("sha256"));
      remote.key("licence").string(*f.str("licence")).end_object();
    }
  }
  remote.end_array();
  out.remote = remote.take();
  return out;
}

std::string enhance_manifest(const std::string& native, const std::vector<packed_file>& files,
                             const std::string& remote) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_ENHANCE_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  REQUIRE(doc->find("contributes"));
  std::int64_t size = 0;
  for (const auto& f : files) size += static_cast<std::int64_t>(f.bytes.size());
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("enhance");
  w.key("name").string("Enhance");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(size);
  w.key("native").string(native);
  w.key("chrome").string("");  // the hosts render its page; it has no chrome of its own
  w.key("archive").begin_object().key("path").string("enhance.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  for (const auto& f : files) {
    w.begin_object().key("path").string(f.path).key("sha256").string(mv::addon::sha256_hex(f.bytes))
        .key("size").integer(static_cast<std::int64_t>(f.bytes.size())).key("licence").string(f.licence)
        .end_object();
  }
  w.end_array();
  w.key("remote").raw(remote);
  w.key("description").string(*doc->str("description"));
  w.key("contributes").raw(json_text(*doc->find("contributes")));
  w.end_object();
  return w.take();
}

// The Models page as the host holds it, read back through the vocabulary.
scr::tree page(const slot_registry& reg) {
  const auto doc = mv::json::parse(reg.screens_json());
  REQUIRE(doc);
  REQUIRE(doc->a.size() == 1);
  const mv::json::value* t = doc->a[0].find("tree");
  REQUIRE(t);
  REQUIRE(t->k == mv::json::kind::object);
  scr::tree out;
  REQUIRE(out.set(json_text(*t)) == scr::error::none);
  return out;
}

std::string text_of(const scr::tree& t, const std::string& id) {
  const scr::node* n = t.find(id);
  const std::string* s = n ? n->props.str("text") : nullptr;
  return s ? *s : std::string();
}

bool has(const scr::tree& t, const std::string& id) { return t.find(id) != nullptr; }

}  // namespace

TEST_CASE("enhance: the shipped manifest declares its Models page in Settings and nothing else",
          "[enhance][addon]") {
  keypair k;
  const staged_catalogue cat = stage_fixture();
  std::vector<packed_file> files{{"libmv_enhance.dylib", {1, 2, 3}, "GPL-3.0-or-later"}};
  files.insert(files.end(), cat.files.begin(), cat.files.end());
  const std::string m = enhance_manifest("libmv_enhance.dylib", files, cat.remote);
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  const auto& c = d.m.slots;
  REQUIRE(c.screens.size() == 1);
  CHECK(c.screens[0].id == "models");
  CHECK(c.screens[0].title == "Enhance");
  CHECK(c.screens[0].place == mv::addon::screen_place::settings);
  CHECK(c.api_min == 8);
  CHECK(c.items.empty());
  CHECK(c.info_sections.empty());
  CHECK(c.search_providers.empty());
  CHECK(c.badges.empty());
  CHECK(d.m.commands.empty());  // E2 adds the command
  CHECK(d.m.chrome.empty());
  // The fixture's three pins, each under its model's folder.
  REQUIRE(d.m.remote.size() == 3);
  for (std::size_t i = 0; i < d.m.remote.size(); ++i) {
    CHECK(d.m.remote[i].path.rfind("models/fixture-", 0) == 0);
    CHECK(d.m.remote_urls[i].rfind("https://huggingface.co/", 0) == 0);
  }
}

#if defined(MV_ENHANCE_MODULE_PATH)
TEST_CASE("enhance: the add-on loads with no model, opens its Models page and takes a click",
          "[enhance][addon]") {
  keypair k;
  scratch_dir s("addons_enhance");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_ENHANCE_MODULE_PATH).filename());
  const staged_catalogue cat = stage_fixture();
  std::vector<packed_file> files{{native, read_bytes(MV_ENHANCE_MODULE_PATH), "GPL-3.0-or-later"}};
  REQUIRE(files[0].bytes.size() > 1000);
  files.insert(files.end(), cat.files.begin(), cat.files.end());
  const std::string m = enhance_manifest(native, files, cat.remote);
  auto staged = st.make_staging();
  REQUIRE(staged);
  for (const auto& f : files) write_bytes(fs::path(*staged) / fs::path(f.path), f.bytes);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));
  auto info = st.find("enhance");
  REQUIRE(info);
  const fs::path data = info->data_dir;

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  mv_slot_theme theme{};

#if defined(__linux__)
  const auto threads = [] {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator("/proc/self/task")) ++n;
    return n;
  };
  const std::size_t threads_before = threads();
#endif
  // Loaded: nothing asked of it, nothing made in its data folder, no thread.
  auto loaded = mv::addon::loaded_addon::load(st, "enhance", svc);
  REQUIRE(loaded);
  REQUIRE(std::string((*loaded)->api().id) == "enhance");
  REQUIRE((*loaded)->slots_attached());
  CHECK(reg.addon_calls() == 0);
  CHECK(mv::json::parse(reg.screens_json())->a.empty());
  CHECK_FALSE(fs::exists(data / "cache"));
#if defined(__linux__)
  CHECK(threads() == threads_before + 1);  // the registry's worker for it, and none of its own
#endif

  // Its page: this machine, the three kinds, the fixture's models largest
  // first, and the line about Hugging Face. Opening it fetches, hashes and
  // writes nothing.
  REQUIRE(reg.open_screen("enhance", "models", "", theme, 0, 0, 2) == status::ok);
  reg.drain();
  {
    const auto open = mv::json::parse(reg.screens_json());
    REQUIRE(open->a.size() == 1);
    CHECK(*open->a[0].str("place") == "settings");
    const scr::tree t = page(reg);
    CHECK(text_of(t, "machine").find(" of memory") != std::string::npos);
    CHECK(text_of(t, "kind.still") == "Photos");
    CHECK(text_of(t, "kind.video") == "Video");
    CHECK(text_of(t, "kind.audio") == "Audio");
    CHECK(text_of(t, "title.still.fixture-large") == "Large \xC2\xB7 Fixture (Whisper turbo encoder, 1.27 GB)");
    CHECK(text_of(t, "title.still.fixture-standard") == "Standard \xC2\xB7 Fixture (SFace, 39 MB)");
    CHECK(text_of(t, "title.still.fixture-small") == "Small \xC2\xB7 Fixture (YuNet, 0.2 MB)");
    CHECK(text_of(t, "title.audio.fixture-small") == "Small \xC2\xB7 Fixture (YuNet, 0.2 MB)");
    CHECK(text_of(t, "none.video") == "No model yet.");
    CHECK(text_of(t, "detail.still.fixture-small").find("Downloads 233 KB from Hugging Face") == 0);
    CHECK(text_of(t, "footnote").find("Models download from Hugging Face only when you click Download.") == 0);
    CHECK(has(t, "dl.still.fixture-small"));
    CHECK_FALSE(has(t, "delete.still"));
    CHECK(reg.screens_json().find(utf8(s.root())) == std::string::npos);  // no path in the page
  }
  CHECK_FALSE(fs::exists(data / "cache"));
  CHECK_FALSE(fs::exists(data / "enhance.json"));
#if defined(__linux__)
  CHECK(threads() == threads_before + 2);  // and the page's poster; no download worker before a click
#endif
  REQUIRE(reg.close_screen("enhance", "models") == status::ok);
  reg.drain();
  CHECK(mv::json::parse(reg.screens_json())->a.empty());
  loaded->reset();
  CHECK_FALSE(reg.attached("enhance"));

  // The next launch, with a still model chosen in an earlier one and none of
  // its files here: nothing is requested; the page says it stopped, with
  // Resume and Delete, and Delete forgets the choice.
  write_text(data / "enhance.json", R"({"schema":1,"models":{"still":"fixture-small"}})");
  slot_registry reg2;
  svc.slots = &reg2;
  auto again = mv::addon::loaded_addon::load(st, "enhance", svc);
  REQUIRE(again);
  REQUIRE(reg2.open_screen("enhance", "models", "", theme, 0, 0, 2) == status::ok);
  reg2.drain();
  {
    const scr::tree t = page(reg2);
    CHECK(text_of(t, "state.still.fixture-small") == "The download stopped.");
    CHECK(has(t, "resume.still"));
    CHECK(has(t, "delete.still"));
    CHECK(has(t, "dl.audio.fixture-small"));
  }
  CHECK(reg2.send_event("enhance", "models", "cancel.still", MV_SLOT_EVENT_CLICK, "") ==
        status::invalid_arg);  // not on the page: the host refuses it
  REQUIRE(reg2.send_event("enhance", "models", "delete.still", MV_SLOT_EVENT_CLICK, "") == status::ok);
  REQUIRE(eventually([&] {
    reg2.drain();
    return has(page(reg2), "dl.still.fixture-small");
  }));
  {
    const std::vector<std::uint8_t> kept = read_bytes(data / "enhance.json");
    CHECK(std::string(kept.begin(), kept.end()).find("fixture-small") == std::string::npos);
  }
  CHECK_FALSE(fs::exists(data / "cache" / "models"));

#if defined(__linux__)
  // No transport on Linux: Download passes the cap, the fit and the lock,
  // stops at the transport, before any request, and leaves no file but the
  // lock's. The choice is kept, with Resume.
  REQUIRE(reg2.send_event("enhance", "models", "dl.still.fixture-small", MV_SLOT_EVENT_CLICK, "") == status::ok);
  REQUIRE(eventually([&] {
    reg2.drain();
    return text_of(page(reg2), "state.still.fixture-small") == "Downloads are not available on this computer.";
  }));
  {
    const scr::tree t = page(reg2);
    CHECK(has(t, "resume.still"));
    CHECK(has(t, "delete.still"));
    const std::vector<std::uint8_t> kept = read_bytes(data / "enhance.json");
    CHECK(std::string(kept.begin(), kept.end()).find(R"("still":"fixture-small")") != std::string::npos);
  }
  CHECK(list_tree(data / "cache") == std::vector<std::string>{"download.lock"});
  CHECK(eventually([&] { return reg2.busy_what().empty(); }));  // nothing runs
  REQUIRE(reg2.send_event("enhance", "models", "delete.still", MV_SLOT_EVENT_CLICK, "") == status::ok);
  REQUIRE(eventually([&] {
    reg2.drain();
    return has(page(reg2), "dl.still.fixture-small");
  }));
  CHECK(list_tree(data / "cache") == std::vector<std::string>{"download.lock"});
#endif

  // Closed, then unloaded: the poster and any download stop before the
  // library goes, and the page goes with the add-on.
  REQUIRE(reg2.close_screen("enhance", "models") == status::ok);
  reg2.drain();
  again->reset();
  CHECK_FALSE(reg2.attached("enhance"));
  CHECK(mv::json::parse(reg2.screens_json())->a.empty());
}
#endif
