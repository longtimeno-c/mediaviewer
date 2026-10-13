// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #151: the add-on store exports (mediaviewer_addon.h), called the way the
// Windows chrome calls them, against scratch folders.
//
// What CI can reach: every argument check, the buffer rule, the empty store,
// hashing, staging, the refusals (an unsigned or wrongly signed first-party
// add-on, a tampered folder), removal, quit, and the whole open add-on path
// (inspect, install, list, theme, tamper, remove), since an open add-on is
// signed by its own publisher. What it cannot: installing or loading a
// first-party add-on. Those must verify against the pinned release key, and
// CI never holds its secret half (cmake/import.cmake), so a loaded add-on's
// commands, unload and quit are covered by the store and host tests
// (mv_import_tests) instead.
#include "catch_compat.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include "abi/test_hooks.h"
#include "core/json.h"
#include "import_fixture.h"
#include "mediaviewer/mediaviewer.h"
#include "mediaviewer/mediaviewer_addon.h"
#include "open_addon_fixture.h"

namespace {

namespace fs = std::filesystem;
using mv::test::scratch_dir;
using mv::test::utf8;

// The DLL's add-on folders point into a scratch folder for the test's life,
// never at the user's real add-ons.
struct addon_dirs {
  scratch_dir scratch{"abi_addons"};
  fs::path addons = scratch / "Add-ons";
  fs::path open = scratch / "Open Add-ons";
  addon_dirs() {
    mv::abi::set_user_dirs_for_tests(utf8(scratch / "thumbs").c_str(), utf8(addons).c_str(),
                                     utf8(open).c_str());
  }
  ~addon_dirs() { mv::abi::set_user_dirs_for_tests("", "", ""); }
  addon_dirs(const addon_dirs&) = delete;
  addon_dirs& operator=(const addon_dirs&) = delete;
};

struct session_guard {
  mv_session_t handle = nullptr;
  session_guard() {
    mv_session_config config{};
    config.worker_count = 1;
    REQUIRE(mv_session_create(&config, &handle) == MV_OK);
    REQUIRE(handle != nullptr);
  }
  ~session_guard() {
    if (handle) (void)mv_session_release(handle);
  }
  session_guard(const session_guard&) = delete;
  session_guard& operator=(const session_guard&) = delete;
};

using json_fn = mv_status(MV_CALL*)(char*, uint32_t, uint32_t*);

// The buffer rule: ask with no buffer for the size, then fetch.
std::string fetch(json_fn fn) {
  uint32_t needed = 0;
  REQUIRE(fn(nullptr, 0, &needed) == MV_ERR_INVALID_ARG);
  REQUIRE(needed > 0);
  std::vector<char> buf(needed);
  REQUIRE(fn(buf.data(), needed, &needed) == MV_OK);
  return std::string(buf.data());
}

mv::json::value parse(const std::string& text) {
  auto doc = mv::json::parse(text);
  REQUIRE(doc);
  return *doc;
}

fs::path from_utf8(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }

// A first-party add-on folder as a download would leave it, but signed by a
// key that is not the pinned one.
void write_unsigned_addon(const fs::path& version_dir, const std::string& id,
                          const std::string& name, std::int64_t installed_size = 10) {
  const std::string manifest = R"({"schema":1,"id":")" + id + R"(","name":")" + name +
                               R"(","version":")" + utf8(version_dir.filename()) +
                               R"(","installed_size":)" + std::to_string(installed_size) + "}";
  mv::test::publisher_key stranger;
  mv::test::write_text(version_dir / "manifest.json", manifest);
  mv::test::write_bytes(version_dir / "manifest.json.sig", stranger.sign(manifest));
}

}  // namespace

TEST_CASE("the add-on exports refuse missing arguments", "[abi][addon]") {
  addon_dirs dirs;
  session_guard s;
  char buf[2048]{};
  uint32_t needed = 0;
  const void* iface = nullptr;
  uint64_t used = 0;
  uint64_t ceiling = 0;
  char hex[65]{};

  CHECK(mv_addon_check_manifest(nullptr, 10, nullptr, 0, buf, sizeof buf, &needed) ==
        MV_ERR_INVALID_ARG);
  CHECK(mv_addon_check_manifest("{}", 0, nullptr, 0, buf, sizeof buf, &needed) ==
        MV_ERR_INVALID_ARG);
  CHECK(mv_addon_sha256_file(nullptr, hex) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_sha256_file("x", nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_install(nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_family_usage(nullptr, &used, &ceiling) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_family_usage("ai", nullptr, &ceiling) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_family_usage("ai", &used, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_remove(nullptr, 1) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_unload(nullptr) == MV_ERR_INVALID_ARG);
  for (auto* load : {&mv_addon_load, &mv_addon_load_reader}) {
    CHECK(load(nullptr, "import", "mv.import.1", &iface, nullptr, 0) == MV_ERR_INVALID_ARG);
    CHECK(load(s.handle, nullptr, "mv.import.1", &iface, nullptr, 0) == MV_ERR_INVALID_ARG);
    CHECK(load(s.handle, "import", nullptr, &iface, nullptr, 0) == MV_ERR_INVALID_ARG);
    CHECK(load(s.handle, "import", "mv.import.1", nullptr, nullptr, 0) == MV_ERR_INVALID_ARG);
  }
  CHECK(iface == nullptr);

  CHECK(mv_open_addon_inspect(nullptr, buf, sizeof buf, &needed) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_inspect("", buf, sizeof buf, &needed) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_install(nullptr, "0", buf, sizeof buf, &needed) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_install("a.mvaddon", nullptr, buf, sizeof buf, &needed) ==
        MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_remove(nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_remove("") == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_theme_json(nullptr, "dusk", buf, sizeof buf, &needed) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_theme_json("acme.film-tones", nullptr, buf, sizeof buf, &needed) ==
        MV_ERR_INVALID_ARG);

  // None of that made a folder.
  CHECK_FALSE(fs::exists(dirs.addons));
  CHECK_FALSE(fs::exists(dirs.open));
}

TEST_CASE("an empty add-ons folder lists nothing and loads nothing", "[abi][addon]") {
  addon_dirs dirs;
  // The product's addons_dir() makes the folder; the open add-ons' is left alone.
  fs::create_directories(dirs.addons);
  session_guard s;

  CHECK(fetch(&mv_addon_installed_json) == "[]");
  CHECK(fetch(&mv_addon_commands_json) == "[]");
  CHECK(fetch(&mv_open_addon_list_json) == "[]");
  // A buffer one byte short is refused and says what it needs.
  char two[2]{};
  uint32_t needed = 0;
  CHECK(mv_addon_installed_json(two, sizeof two, &needed) == MV_ERR_INVALID_ARG);
  CHECK(needed == 3);

  uint64_t used = 99;
  uint64_t ceiling = 99;
  REQUIRE(mv_addon_family_usage("ai", &used, &ceiling) == MV_OK);
  CHECK(used == 0);
  CHECK(ceiling == 3'000'000'000ull);  // docs/design/17: the AI pack's 3 GB
  REQUIRE(mv_addon_family_usage("import", &used, &ceiling) == MV_OK);
  CHECK(used == 0);
  CHECK(ceiling == 0);

  // Nothing installed: nothing answers to the id.
  const void* iface = nullptr;
  char chrome[512]{};
  CHECK(mv_addon_load(s.handle, "import", "mv.import.1", &iface, chrome, sizeof chrome) ==
        MV_ERR_INVALID_ARG);
  CHECK(mv_addon_load_reader(s.handle, "ai", "mv.ai.1", &iface, chrome, sizeof chrome) ==
        MV_ERR_INVALID_ARG);
  CHECK(iface == nullptr);
  CHECK(mv_addon_remove("import", 0) == MV_ERR_INVALID_ARG);
  // Unloading what is not loaded is not an error (Remove calls it first).
  CHECK(mv_addon_unload("import") == MV_OK);

  char buf[256]{};
  CHECK(mv_open_addon_theme_json("acme.film-tones", "dusk", buf, sizeof buf, &needed) != MV_OK);
  CHECK(mv_open_addon_remove("acme.film-tones") == MV_ERR_INVALID_ARG);

  // Looking created nothing (mediaviewer_addon.h: "the folder is not created").
  CHECK_FALSE(fs::exists(dirs.open));
}

TEST_CASE("mv_addon_sha256_file hashes a file and reports one it cannot read", "[abi][addon]") {
  addon_dirs dirs;
  const fs::path file = dirs.scratch / "abc.bin";
  mv::test::write_text(file, "abc");
  char hex[65];
  std::memset(hex, 'x', sizeof hex);
  REQUIRE(mv_addon_sha256_file(utf8(file).c_str(), hex) == MV_OK);
  CHECK(std::string(hex) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(mv_addon_sha256_file(utf8(dirs.scratch / "missing.bin").c_str(), hex) == MV_ERR_IO);
}

TEST_CASE("mv_addon_check_manifest trusts only the pinned key and echoes nothing unsigned",
          "[abi][addon]") {
  addon_dirs dirs;
  const std::string manifest =
      R"({"schema":1,"id":"import","name":"Import","version":"9.9.9","installed_size":1})";
  mv::test::publisher_key stranger;
  const auto sig = stranger.sign(manifest);

  uint32_t needed = 0;
  REQUIRE(mv_addon_check_manifest(manifest.data(), static_cast<uint32_t>(manifest.size()),
                                  sig.data(), static_cast<uint32_t>(sig.size()), nullptr, 0,
                                  &needed) == MV_ERR_INVALID_ARG);
  std::vector<char> out(needed);
  REQUIRE(mv_addon_check_manifest(manifest.data(), static_cast<uint32_t>(manifest.size()),
                                  sig.data(), static_cast<uint32_t>(sig.size()), out.data(),
                                  needed, &needed) == MV_OK);
  auto d = parse(out.data());
  CHECK(d.boolean("ok") == false);
  CHECK(*d.str("why") == "bad_signature");
  // The id, name and archive of a manifest nobody signed are not repeated.
  CHECK(d.find("id") == nullptr);
  CHECK(d.find("name") == nullptr);
  CHECK(d.find("archive") == nullptr);

  char buf[512]{};
  REQUIRE(mv_addon_check_manifest(manifest.data(), static_cast<uint32_t>(manifest.size()),
                                  nullptr, 0, buf, sizeof buf, &needed) == MV_OK);
  d = parse(buf);
  CHECK(d.boolean("ok") == false);
  CHECK(*d.str("why") == "missing_signature");
  CHECK(d.find("id") == nullptr);
}

TEST_CASE("staging folders are made under the add-ons folder and consumed by install",
          "[abi][addon]") {
  addon_dirs dirs;
  char staged[1024]{};
  REQUIRE(mv_addon_make_staging(staged, sizeof staged) == MV_OK);
  const fs::path first = from_utf8(staged);
  CHECK(fs::is_directory(first));
  CHECK(first.parent_path().filename() == ".staging");
  CHECK(fs::equivalent(first.parent_path().parent_path(), dirs.addons));
  char tiny[4]{};
  CHECK(mv_addon_make_staging(tiny, sizeof tiny) == MV_ERR_INVALID_ARG);

  // Empty: no manifest. Refused, and the folder is gone either way.
  CHECK(mv_addon_install(staged) == MV_ERR_CORRUPT);
  CHECK_FALSE(fs::exists(first));

  // A complete folder signed by someone else: refused, consumed, not listed.
  char second[1024]{};
  REQUIRE(mv_addon_make_staging(second, sizeof second) == MV_OK);
  const fs::path staged2 = from_utf8(second);
  CHECK(staged2 != first);
  write_unsigned_addon(staged2, "import", "Import");
  CHECK(mv_addon_install(second) == MV_ERR_CORRUPT);
  CHECK_FALSE(fs::exists(staged2));
  CHECK(fetch(&mv_addon_installed_json) == "[]");
  CHECK_FALSE(fs::exists(dirs.addons / "import"));
  CHECK_FALSE(fs::exists(dirs.addons / "Import"));
}

TEST_CASE("an add-on folder nobody signed is listed as invalid, never loads, and removes",
          "[abi][addon]") {
  addon_dirs dirs;
  session_guard s;
  // Dropped in by hand (a sideload), as a tampered copy would be.
  write_unsigned_addon(dirs.addons / "import" / "1.0.0", "import", "Import");
  mv::test::write_text(dirs.addons / "import" / "data" / "import.db", "rows");
  mv::test::write_text(dirs.addons / "import" / "data" / "cache" / "derived.bin", "cache");
  // An unsigned piece of the AI family claiming a size: unsigned bytes never count.
  write_unsigned_addon(dirs.addons / "ai" / "1.0.0", "ai", "AI", 2'999'999'999);

  const auto listed = parse(fetch(&mv_addon_installed_json));
  REQUIRE(listed.a.size() == 2);
  for (const auto& entry : listed.a) {
    CHECK(*entry.str("state") == "invalid");
    CHECK(*entry.str("why") == "bad_signature");
    CHECK(entry.boolean("loaded") == false);
    // Its id is unsigned, so the list does not repeat it.
    CHECK(entry.str("id")->empty());
  }
  uint64_t used = 99;
  uint64_t ceiling = 0;
  REQUIRE(mv_addon_family_usage("ai", &used, &ceiling) == MV_OK);
  CHECK(used == 0);

  // A folder with no trusted id answers to no id: it cannot be loaded either way.
  const void* iface = nullptr;
  CHECK(mv_addon_load(s.handle, "import", "mv.import.1", &iface, nullptr, 0) == MV_ERR_INVALID_ARG);
  CHECK(mv_addon_load_reader(s.handle, "ai", "mv.ai.1", &iface, nullptr, 0) == MV_ERR_INVALID_ARG);
  CHECK(iface == nullptr);
  CHECK(fetch(&mv_addon_commands_json) == "[]");

  // Removal matches the id in any manifest, signed or not (store.cpp), so a
  // tampered copy can always be removed. Keeping data keeps the user's index
  // and drops the derived cache.
  REQUIRE(mv_addon_remove("import", 1) == MV_OK);
  CHECK_FALSE(fs::exists(dirs.addons / "import" / "1.0.0"));
  CHECK(fs::exists(dirs.addons / "import" / "data" / "import.db"));
  CHECK_FALSE(fs::exists(dirs.addons / "import" / "data" / "cache"));
  REQUIRE(mv_addon_remove("ai", 0) == MV_OK);
  CHECK_FALSE(fs::exists(dirs.addons / "ai"));
  CHECK(fetch(&mv_addon_installed_json) == "[]");
}

TEST_CASE("quit with nothing loaded returns at once and waits for nothing", "[abi][addon]") {
  addon_dirs dirs;
  REQUIRE(mv_addon_quit() == MV_OK);
  CHECK(mv_addon_quit_wait(0) == MV_OK);
  CHECK(mv_addon_quit_wait(50) == MV_OK);
  CHECK(fetch(&mv_addon_commands_json) == "[]");
}

TEST_CASE("an open add-on goes through inspect, install, theme, tamper and remove over the ABI",
          "[abi][addon][open-addon]") {
  addon_dirs dirs;
  mv::test::publisher_key k;
  const fs::path package = dirs.scratch / "film-tones.mvaddon";
  mv::test::write_bytes(package, mv::test::make_package(k, mv::test::theme_files()));
  const std::string path = utf8(package);

  // Inspect: what the consent sheet shows. It installs and creates nothing.
  uint32_t needed = 0;
  REQUIRE(mv_open_addon_inspect(path.c_str(), nullptr, 0, &needed) == MV_ERR_INVALID_ARG);
  std::vector<char> buf(needed);
  REQUIRE(mv_open_addon_inspect(path.c_str(), buf.data(), needed, &needed) == MV_OK);
  auto offer = parse(buf.data());
  CHECK(offer.boolean("ok") == true);
  CHECK(*offer.str("relation") == "fresh");
  CHECK(*offer.str("id") == "acme.film-tones");
  CHECK(*offer.find("publisher")->str("key") == k.hex());
  const std::string sha = *offer.str("sha256");
  char hex[65]{};
  REQUIRE(mv_addon_sha256_file(path.c_str(), hex) == MV_OK);
  CHECK(sha == hex);
  CHECK_FALSE(fs::exists(dirs.open));

  // Install takes the sha that was shown, and one result buffer of 1 KB.
  char out[1024]{};
  CHECK(mv_open_addon_install(path.c_str(), sha.c_str(), out, 1023, &needed) == MV_ERR_INVALID_ARG);
  CHECK(fetch(&mv_open_addon_list_json) == "[]");
  const std::string other(64, '0');
  REQUIRE(mv_open_addon_install(path.c_str(), other.c_str(), out, sizeof out, &needed) == MV_OK);
  auto refused = parse(out);
  CHECK(refused.boolean("ok") == false);
  CHECK(*refused.str("why") == "changed");
  CHECK(fetch(&mv_open_addon_list_json) == "[]");

  REQUIRE(mv_open_addon_install(path.c_str(), sha.c_str(), out, sizeof out, &needed) == MV_OK);
  auto done = parse(out);
  CHECK(done.boolean("ok") == true);
  CHECK(*done.str("id") == "acme.film-tones");
  CHECK(*done.str("version") == "1.2.0");

  auto listed = parse(fetch(&mv_open_addon_list_json));
  REQUIRE(listed.a.size() == 1);
  CHECK(*listed.a[0].str("state") == "ok");
  const std::string folder = *listed.a[0].str("folder");
  CHECK(folder == "acme.film-tones");
  // The two stores never see each other's add-ons.
  CHECK(fetch(&mv_addon_installed_json) == "[]");

  // The theme as both chromes read it.
  REQUIRE(mv_open_addon_theme_json("acme.film-tones", "dusk", nullptr, 0, &needed) ==
          MV_ERR_INVALID_ARG);
  std::vector<char> theme_buf(needed);
  REQUIRE(mv_open_addon_theme_json("acme.film-tones", "dusk", theme_buf.data(), needed,
                                   &needed) == MV_OK);
  auto theme = parse(theme_buf.data());
  CHECK(*theme.str("addon") == "acme.film-tones");
  CHECK(*theme.find("theme")->find("dark")->str("accent") == "#e0793aff");
  char small[256]{};
  CHECK(mv_open_addon_theme_json("acme.film-tones", "dawn", small, sizeof small, &needed) ==
        MV_ERR_NOT_FOUND);

  // One changed byte in an installed file: the add-on no longer verifies and
  // the chrome goes back to its default theme.
  const fs::path dusk = dirs.open / "acme.film-tones" / "1.2.0" / "themes" / "dusk.json";
  REQUIRE(fs::exists(dusk));
  auto bytes = mv::test::read_bytes(dusk);
  bytes[bytes.size() / 2] = static_cast<std::uint8_t>(bytes[bytes.size() / 2] ^ 0x01u);
  mv::test::write_bytes(dusk, bytes);
  CHECK(mv_open_addon_theme_json("acme.film-tones", "dusk", theme_buf.data(),
                                 static_cast<uint32_t>(theme_buf.size()), &needed) ==
        MV_ERR_CORRUPT);
  listed = parse(fetch(&mv_open_addon_list_json));
  REQUIRE(listed.a.size() == 1);
  CHECK(*listed.a[0].str("state") == "invalid");

  // The same package again is a repair.
  buf.assign(4096, '\0');
  REQUIRE(mv_open_addon_inspect(path.c_str(), buf.data(), 4096, &needed) == MV_OK);
  CHECK(*parse(buf.data()).str("relation") == "repair");
  REQUIRE(mv_open_addon_install(path.c_str(), sha.c_str(), out, sizeof out, &needed) == MV_OK);
  CHECK(parse(out).boolean("ok") == true);
  CHECK(*parse(fetch(&mv_open_addon_list_json)).a[0].str("state") == "ok");

  REQUIRE(mv_open_addon_remove(folder.c_str()) == MV_OK);
  CHECK(fetch(&mv_open_addon_list_json) == "[]");
  CHECK_FALSE(fs::exists(dirs.open / "acme.film-tones"));
  CHECK(mv_open_addon_remove(folder.c_str()) == MV_ERR_INVALID_ARG);
  CHECK(mv_open_addon_theme_json("acme.film-tones", "dusk", small, sizeof small, &needed) != MV_OK);
}

TEST_CASE("mv_volume_watch starts with a session and stops cleanly", "[abi][addon]") {
  CHECK(mv_volume_watch(nullptr, 1) == MV_ERR_INVALID_ARG);
  session_guard s;
  REQUIRE(mv_volume_watch(s.handle, 1) == MV_OK);
  // A second start replaces the first watch rather than stacking a second.
  REQUIRE(mv_volume_watch(s.handle, 1) == MV_OK);
  REQUIRE(mv_volume_watch(nullptr, 0) == MV_OK);
  REQUIRE(mv_volume_watch(nullptr, 0) == MV_OK);
  // Stopped, the watch holds no reference: the session goes with its last release.
  mv_present_set_busy(1);
  mv_present_set_busy(0);
}
