// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Enhance add-on through the real loader (#297 slices 1 and 2).
//
// Its manifest (src/addons/enhance/manifest.json) is accepted with a weights
// pin under `remote` and declares the sheet, its settings page, the context
// entry and the command. Then mv_enhance is installed in a signed store whose
// version folder carries a model.json but not the weights, loaded through the
// v3 host table, and answers its settings page and its sheet. Nothing is
// requested from the network here: the click that would fetch is the engine
// suite's (tests/test_enhance.cpp, fake transport).
#include "catch_compat.h"

#include <sodium.h>

#include <chrono>
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
using mv::addon::rejection;
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
  for (int i = 0; i < 400; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

std::string shipped_contributes(std::string* description) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_ENHANCE_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  if (description) *description = *doc->str("description");
  const mv::json::value* c = doc->find("contributes");
  REQUIRE(c);
  mv::json::writer w;
  w.begin_object();
  for (const auto& [key, value] : c->o) {
    w.key(key);
    mv::addon::screen::write_value(w, value);
  }
  w.end_object();
  return w.take();
}

struct packed_file {
  std::string path;
  std::vector<std::uint8_t> bytes;
  std::string licence;
};

const char* kAudioSpec = R"({"kind": "audio", "name": "Speech cleanup", "weights": ["denoise.onnx"],
  "run_bytes": 400000000, "knobs": [{"id": "noise", "label": "Noise reduction", "type": "slider",
  "min": 0, "max": 1, "default": 0.8}]})";

std::string enhance_manifest(const std::string& native, const std::vector<packed_file>& files) {
  std::string description;
  const std::string contributes = shipped_contributes(&description);
  std::int64_t size = 0;
  for (const auto& f : files) size += static_cast<std::int64_t>(f.bytes.size());
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("enhance");
  w.key("name").string("Enhance");
  w.key("version").string("0.1.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(size);
  w.key("native").string(native);
  w.key("chrome").string("");
  w.key("archive").begin_object().key("path").string("enhance.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  for (const auto& f : files) {
    w.begin_object().key("path").string(f.path).key("sha256").string(mv::addon::sha256_hex(f.bytes))
        .key("size").integer(static_cast<std::int64_t>(f.bytes.size())).key("licence").string(f.licence)
        .end_object();
  }
  w.end_array();
  // The weights: pinned, not shipped.
  w.key("remote").begin_array();
  w.begin_object().key("url").string("https://huggingface.co/mv-test/denoise/resolve/0123abcd/denoise.onnx")
      .key("path").string("audio/denoise.onnx").key("size").integer(50'000)
      .key("sha256").string(std::string(64, 'b')).key("licence").string("MIT").end_object();
  w.end_array();
  w.key("description").string(description);
  w.key("contributes").raw(contributes);
  w.end_object();
  return w.take();
}

mv_slot_theme light_theme() {
  mv_slot_theme t{};
  t.canvas = 0xffffffff;
  t.surface = 0xf5f5f7ff;
  t.title = 0x1d1d1fff;
  t.body = 0x3c3c43ff;
  t.disabled = 0x8e8e93ff;
  t.hairline = 0xd1d1d6ff;
  t.accent = 0x0a84ffff;
  return t;
}

// The tree the registry holds for one of Enhance's screens, as text.
std::string screen_tree(const slot_registry& reg, const std::string& screen) {
  const auto doc = mv::json::parse(reg.screens_json(), 64);
  if (!doc || doc->k != mv::json::kind::array) return {};
  for (const auto& s : doc->a) {
    const std::string* addon = s.str("addon");
    const std::string* id = s.str("id");
    const mv::json::value* tree = s.find("tree");
    if (addon && *addon == "enhance" && id && *id == screen && tree && tree->k == mv::json::kind::object) {
      mv::json::writer w;
      mv::addon::screen::write_value(w, *tree);
      return w.take();
    }
  }
  return {};
}

}  // namespace

TEST_CASE("enhance: the shipped manifest declares the sheet, its settings page, the context entry and the command",
          "[addon][enhance]") {
  keypair k;
  const std::vector<packed_file> files{{"libmv_enhance.dylib", {1, 2, 3}, "GPL-3.0-or-later"},
                                       {"models/audio/model.json", {'{', '}'}, "GPL-3.0-or-later"}};
  const std::string m = enhance_manifest("libmv_enhance.dylib", files);
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  // The weights are a pin, not a file of the zip, and the zip still verifies without them.
  REQUIRE(d.m.remote.size() == 1);
  CHECK(d.m.remote[0].path == "audio/denoise.onnx");
  CHECK(d.m.remote_urls[0].find('?') == std::string::npos);
  const auto& c = d.m.slots;
  REQUIRE(c.screens.size() == 2);
  CHECK(c.screens[0].id == "enhance");
  CHECK(c.screens[0].place == mv::addon::screen_place::sheet);
  CHECK(c.screens[1].id == "settings");
  CHECK(c.screens[1].place == mv::addon::screen_place::settings);
  REQUIRE(c.items.size() == 1);
  CHECK(c.items[0].id == "enhance");
  CHECK(c.items[0].target == mv::addon::item_target::file);
  REQUIRE(d.m.commands.size() == 1);
  CHECK(d.m.commands[0].id == "open");
  CHECK(d.m.commands[0].windows.empty());  // no default key (docs/design/16)
  CHECK(d.m.chrome.empty());
  CHECK(mv::addon::loads_for_slots(d.m));
}

#if defined(MV_ENHANCE_MODULE_PATH)
TEST_CASE("enhance: the add-on answers its settings page and sheet through the v3 host table",
          "[addon][enhance]") {
  keypair k;
  scratch_dir s("addons_enhance");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_ENHANCE_MODULE_PATH).filename());
  const std::vector<packed_file> files{{native, read_bytes(MV_ENHANCE_MODULE_PATH), "GPL-3.0-or-later"},
                                       {"models/audio/model.json",
                                        std::vector<std::uint8_t>(kAudioSpec, kAudioSpec + std::strlen(kAudioSpec)),
                                        "GPL-3.0-or-later"}};
  REQUIRE(files[0].bytes.size() > 1000);
  const std::string m = enhance_manifest(native, files);
  auto staged = st.make_staging();
  REQUIRE(staged);
  for (const auto& f : files) write_bytes(fs::path(*staged) / fs::path(f.path), f.bytes);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  const fs::path song = s / "song.mp3";
  write_bytes(song, {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0});

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  svc.data_dir = utf8(s / "data");
  fs::create_directories(s / "data");
  auto loaded = mv::addon::loaded_addon::load(st, "enhance", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  CHECK(reg.addon_calls() == 0);  // nothing opened, nothing read, until asked

  // The settings page: the audio model this build ships, not installed, with
  // its size; the kinds it has no model for are named.
  REQUIRE(reg.open_screen("enhance", "settings", "", light_theme(), 520, 600, 2) == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return !screen_tree(reg, "settings").empty(); }));
  const std::string page = screen_tree(reg, "settings");
  INFO(page);
  CHECK(page.find("\"install.audio\"") != std::string::npos);
  CHECK(page.find("Download the audio model (50 MB)") == std::string::npos);  // 50 kB, shown as 1 MB
  CHECK(page.find("(1 MB)") != std::string::npos);
  CHECK(page.find("no video model") != std::string::npos);
  CHECK(page.find("no photo model") != std::string::npos);
  // No weights were fetched: the click is the only door.
  CHECK_FALSE(fs::exists(s / "data/models/audio/denoise.onnx.part"));

  // The command opens the sheet on the file it names.
  REQUIRE(reg.run_command("enhance", "open", "[\"" + json_path(song) + "\"]") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return screen_tree(reg, "enhance").find("song.mp3") != std::string::npos; }));
  const std::string sheet = screen_tree(reg, "enhance");
  INFO(sheet);
  // No host decoder in this test: the audio cannot be read, and Run is greyed.
  CHECK(sheet.find("cannot be read") != std::string::npos);
  CHECK(sheet.find("\"run\"") != std::string::npos);
  CHECK(sheet.find("\"disabled\":true") != std::string::npos);

  // Close: the add-on hears it, and its Close button hides the sheet.
  REQUIRE(reg.send_event("enhance", "enhance", "close", MV_SLOT_EVENT_CLICK, "") == status::ok);
  reg.drain();
  loaded->reset();
}
#endif
