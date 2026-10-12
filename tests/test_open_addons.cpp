// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Open add-ons (docs/design/25; PR 55 verify: a package from a fresh key installs,
// and every way of getting it wrong is refused with its reason).
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "addon/package.h"
#include "addon/theme.h"
#include "import_fixture.h"
#include "open_addon_fixture.h"

using namespace mv::test;
using mv::addon::install_state;
using mv::addon::open_store;
using mv::addon::package_relation;
using mv::addon::rejection;

namespace {

std::vector<zip_entry> package_entries(const publisher_key& k,
                                       const std::vector<packed_file>& files,
                                       const manifest_options& o = {}) {
  const std::string manifest = open_manifest_json(k, files, o);
  std::vector<zip_entry> entries;
  entries.push_back(stored("manifest.json", bytes_from(manifest)));
  entries.push_back(stored("manifest.json.sig", k.sign(manifest)));
  for (const packed_file& f : files) entries.push_back(stored(f.path, bytes_from(f.text)));
  return entries;
}

std::string theme_with(const std::string& title, const std::string& canvas = "#1c1b1a") {
  return R"({"schema":1,"dark":{"canvas":")" + canvas +
         R"(","surface":"#262523","title":")" + title +
         R"(","body":"#b9b4aa","disabled":"#6f6b64","hairline":"#3a3835","accent":"#e0793a"}})";
}

}  // namespace

TEST_CASE("a package from a fresh key installs and lists", "[open-addon][store]") {
  scratch_dir scratch("open_install");
  const open_store store(utf8(scratch / "Open Add-ons"));
  publisher_key k;
  const auto package = make_package(k, theme_files());

  const auto offer = store.inspect(package);
  REQUIRE(offer.ok());
  REQUIRE(offer.relation == package_relation::fresh);
  REQUIRE(offer.m.id == "acme.film-tones");
  REQUIRE(offer.m.publisher_name == "Acme Pictures");
  REQUIRE(offer.m.publisher_key == k.hex());
  REQUIRE(offer.m.themes.size() == 1);
  REQUIRE(offer.sha256 == mv::addon::sha256_hex(package));
  REQUIRE(mv::addon::key_fingerprint(k.hex()).size() == 19);

  // Looking installs nothing, and creates nothing.
  REQUIRE_FALSE(fs::exists(scratch / "Open Add-ons"));
  REQUIRE(store.list().empty());
  REQUIRE_FALSE(fs::exists(scratch / "Open Add-ons"));

  auto installed = store.install(package, offer.sha256);
  REQUIRE(installed);
  REQUIRE(installed->state == install_state::ok);
  REQUIRE(installed->version == "1.2.0");
  REQUIRE(fs::exists(scratch / "Open Add-ons" / "acme.film-tones" / "1.2.0" / "themes" / "dusk.json"));
  REQUIRE(fs::exists(scratch / "Open Add-ons" / "acme.film-tones" / "publisher.json"));
  REQUIRE_FALSE(fs::exists(scratch / "Open Add-ons" / ".staging" / "0"));

  const auto listed = store.list();
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].id == "acme.film-tones");
  REQUIRE(listed[0].state == install_state::ok);

  auto theme = store.theme_json("acme.film-tones", "dusk");
  REQUIRE(theme);
  const auto doc = mv::json::parse(*theme);
  REQUIRE(doc);
  REQUIRE(*doc->str("name") == "Dusk");
  const mv::json::value* tokens = doc->find("theme");
  REQUIRE(tokens);
  REQUIRE(*tokens->str("font") == "Avenir Next");
  REQUIRE(*tokens->find("dark")->str("accent") == "#e0793aff");
  REQUIRE(*tokens->find("light")->str("canvas") == "#f4f1eaff");
  REQUIRE_FALSE(store.theme_json("acme.film-tones", "dawn"));

  REQUIRE(store.remove("acme.film-tones"));
  REQUIRE(store.list().empty());
  REQUIRE_FALSE(fs::exists(scratch / "Open Add-ons" / "acme.film-tones"));
  REQUIRE_FALSE(store.remove("acme.film-tones"));
}

TEST_CASE("one changed byte is refused, in a file or in the manifest", "[open-addon][store]") {
  scratch_dir scratch("open_tamper");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  {
    auto entries = package_entries(k, theme_files());
    entries[2].bytes[40] ^= 1;  // the theme
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::file_mismatch);
  }
  {
    auto entries = package_entries(k, theme_files());
    auto& manifest = entries[0].bytes;
    const std::string text(manifest.begin(), manifest.end());
    manifest[text.find("1.2.0")] = '9';
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::bad_signature);
  }
  {
    auto entries = package_entries(k, theme_files());
    entries[1].bytes[3] ^= 1;  // the signature
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::bad_signature);
  }
  {
    // Signed by someone other than the key the manifest names.
    publisher_key other;
    auto entries = package_entries(k, theme_files());
    const std::string text(entries[0].bytes.begin(), entries[0].bytes.end());
    entries[1].bytes = other.sign(text);
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::bad_signature);
  }
  {
    auto entries = package_entries(k, theme_files());
    entries.erase(entries.begin() + 1);
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::missing_signature);
  }
  {
    auto entries = package_entries(k, theme_files());
    entries.erase(entries.begin() + 2);
    REQUIRE(store.inspect(make_zip(entries)).why == rejection::file_missing);
  }
  {
    auto entries = package_entries(k, theme_files());
    entries.push_back(stored("extra.dll", {1, 2, 3}));
    const auto offer = store.inspect(make_zip(entries));
    REQUIRE(offer.why == rejection::unexpected_file);
    REQUIRE(offer.detail == "extra.dll");
  }
  REQUIRE_FALSE(fs::exists(scratch / "open"));
}

TEST_CASE("only the strict ZIP is a package", "[open-addon][package]") {
  publisher_key k;
  const auto good = package_entries(k, theme_files());
  REQUIRE(mv::addon::read_package(make_zip(good)).ok());

  SECTION("compressed") {
    auto entries = good;
    entries[2].method = 8;
    REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::bad_package);
  }
  SECTION("encrypted") {
    auto entries = good;
    entries[2].flags = 1;
    REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::bad_package);
  }
  SECTION("a data descriptor") {
    auto entries = good;
    entries[2].flags = 1u << 3;
    REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::bad_package);
  }
  SECTION("UTF-8 names are the one flag allowed") {
    auto entries = good;
    entries[2].flags = 1u << 11;
    REQUIRE(mv::addon::read_package(make_zip(entries)).ok());
  }
  SECTION("an extra field") {
    auto entries = good;
    entries[2].extra = {0x55, 0x54, 0, 0};
    REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::bad_package);
  }
  SECTION("ZIP64") {
    zip_options o;
    o.zip64_marker = true;
    REQUIRE(mv::addon::read_package(make_zip(good, o)).why == rejection::bad_package);
  }
  SECTION("a comment") {
    zip_options o;
    o.comment = "hello";
    REQUIRE(mv::addon::read_package(make_zip(good, o)).why == rejection::bad_package);
  }
  SECTION("bytes between the entries and the directory") {
    zip_options o;
    o.gap = {1, 2, 3, 4};
    REQUIRE(mv::addon::read_package(make_zip(good, o)).why == rejection::bad_package);
  }
  SECTION("bytes after the end") {
    zip_options o;
    o.trailing = {0};
    REQUIRE(mv::addon::read_package(make_zip(good, o)).why == rejection::bad_package);
  }
  SECTION("unsafe names") {
    for (const char* bad : {"../evil", "/abs", "a\\b", "C:x", "a//b", "./a", "folder/", "a/../b",
                            // Windows: devices, trailing dot or space, characters Win32 refuses.
                            "themes/COM1", "nul.json", "themes/Con.txt/x", "lpt9", "aux .json",
                            "com\xC2\xB9.json", "themes/dusk.", "themes /dusk.json", "a<b", "a>b",
                            "a\"b", "a|b", "a?b", "a*b"}) {
      auto entries = good;
      entries.push_back(stored(bad, {1}));
      REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::unsafe_path);
    }
  }
  SECTION("two names that are one file where case folds") {
    auto entries = good;
    entries.push_back(stored("Themes/Dusk.json", {1}));
    REQUIRE(mv::addon::read_package(make_zip(entries)).why == rejection::bad_package);
  }
  SECTION("too many entries") {
    std::vector<zip_entry> many;
    for (std::size_t i = 0; i <= mv::addon::kPackageMaxEntries; ++i) {
      many.push_back(stored("f" + std::to_string(i), {}));
    }
    REQUIRE(mv::addon::read_package(make_zip(many)).why == rejection::too_large);
  }
  SECTION("truncated anywhere") {
    const auto whole = make_zip(good);
    for (std::size_t cut = 0; cut < whole.size(); cut += 7) {
      const std::vector<std::uint8_t> part(whole.begin(), whole.begin() + static_cast<long>(cut));
      REQUIRE_FALSE(mv::addon::read_package(part).ok());
    }
  }
  SECTION("any one byte of the structure changed never reads outside the package") {
    // Not every flip is refused (a flipped byte of file content is the
    // manifest's to catch); none may crash or read out of bounds (ASan).
    const auto whole = make_zip(good);
    for (std::size_t i = 0; i < whole.size(); ++i) {
      auto flipped = whole;
      flipped[i] ^= 0xFF;
      const auto listing = mv::addon::read_package(flipped);
      for (const auto& e : listing.entries) {
        REQUIRE(e.bytes.data() >= flipped.data());
        REQUIRE(e.bytes.data() + e.bytes.size() <= flipped.data() + flipped.size());
      }
    }
  }
}

TEST_CASE("manifest schema 2: ids, text, links, APIs", "[open-addon][manifest]") {
  publisher_key k;
  const auto check = [&](const manifest_options& o) {
    const std::string m = open_manifest_json(k, theme_files(), o);
    return mv::addon::check_open_manifest(bytes_from(m), k.sign(m)).why;
  };
  REQUIRE(check({}) == rejection::none);

  for (const char* id : {"import", "ai-faces", "mediaviewer.theme", "Acme.tones", "acme..tones",
                         "acme.", ".acme", "acme.-tones", "acme.tones-", "con.tones", "acme.nul",
                         "acme.com1", "acme/tones", "acme tones", "a.b c"}) {
    manifest_options o;
    o.id = id;
    INFO(id);
    REQUIRE(check(o) == rejection::malformed);
  }
  for (const char* id : {"acme.tones", "a.b", "acme.film-tones.dark", "x1.y2", "acme.console"}) {
    manifest_options o;
    o.id = id;
    INFO(id);
    REQUIRE(check(o) == rejection::none);
  }
  REQUIRE_FALSE(mv::addon::valid_open_id(std::string(65, 'a') + ".b"));

  {
    // A name that hides or reorders what is written beside it.
    manifest_options o;
    o.name = "Film\xE2\x80\xAETones";  // U+202E, right-to-left override
    REQUIRE(check(o) == rejection::malformed);
    o.name = "Film\xE2\x80\x8BTones";  // U+200B, zero width space
    REQUIRE(check(o) == rejection::malformed);
    o.name = "Film\nTones";
    REQUIRE(check(o) == rejection::malformed);
    o.name = " Film Tones";
    REQUIRE(check(o) == rejection::malformed);
    o.name = "Film Tones";
    o.publisher = "MediaViewer\xE2\x80\x8F";  // U+200F
    REQUIRE(check(o) == rejection::malformed);
    o.publisher = "Caf\xC3\xA9 Pictures";  // plain non-ASCII is fine
    REQUIRE(check(o) == rejection::none);
  }
  for (const char* url : {"http://acme.example/a.mvaddon", "ftp://acme.example/a",
                          "https://user:pw@acme.example/a", "https:///a", "https://acme.example/a b",
                          "file:///etc/passwd", "javascript:alert(1)"}) {
    manifest_options o;
    o.update_url = url;
    INFO(url);
    REQUIRE(check(o) == rejection::malformed);
    manifest_options p;
    p.publisher_url = url;
    REQUIRE(check(p) == rejection::malformed);
  }
  {
    manifest_options o;
    o.update_url = "";
    o.publisher_url = "";
    REQUIRE(check(o) == rejection::none);
  }
  {
    manifest_options o;
    o.api_min = 2;
    o.api_max = 3;
    const std::string m = open_manifest_json(k, theme_files(), o);
    const auto d = mv::addon::check_open_manifest(bytes_from(m), k.sign(m));
    REQUIRE(d.why == rejection::needs_update);
    REQUIRE(d.m.name == "Film Tones");  // so the app can say which add-on
  }
  {
    manifest_options o;
    o.schema = 1;
    REQUIRE(check(o) == rejection::unsupported_schema);
    o.schema = 3;
    REQUIRE(check(o) == rejection::unsupported_schema);
  }
  {
    // A key this host does not know: a mistake when the manifest claims no
    // newer API, and a later host's business when it does.
    manifest_options o;
    o.extra_key = "colour_scheme";
    REQUIRE(check(o) == rejection::malformed);
    o.api_max = 2;
    REQUIRE(check(o) == rejection::none);
    manifest_options c;
    c.extra_contributes = "settings";
    REQUIRE(check(c) == rejection::malformed);
    c.api_max = 2;
    REQUIRE(check(c) == rejection::none);
  }
  {
    // MediaViewer's own key signs no schema 2 add-on.
    static constexpr char kHex[] = "0123456789abcdef";
    std::string pinned;
    for (const std::uint8_t b : mv::addon::pinned_public_key()) {
      pinned.push_back(kHex[b >> 4]);
      pinned.push_back(kHex[b & 0xF]);
    }
    std::string m = open_manifest_json(k, theme_files());
    m.replace(m.find(k.hex()), 64, pinned);
    REQUIRE(mv::addon::check_open_manifest(bytes_from(m), k.sign(m)).why ==
            rejection::bad_signature);
  }
}

TEST_CASE("a package that names code is refused, whatever API it claims",
          "[open-addon][manifest]") {
  publisher_key k;
  for (const char* key : {"native", "chrome", "scripts", "main"}) {
    for (const int api_max : {1, 9}) {
      INFO(key << " api.max " << api_max);
      manifest_options top;
      top.extra_key = key;
      top.api_max = api_max;
      const std::string a = open_manifest_json(k, theme_files(), top);
      REQUIRE(mv::addon::check_open_manifest(bytes_from(a), k.sign(a)).why ==
              rejection::code_not_allowed);
      manifest_options under;
      under.extra_contributes = key;
      under.api_max = api_max;
      const std::string b = open_manifest_json(k, theme_files(), under);
      REQUIRE(mv::addon::check_open_manifest(bytes_from(b), k.sign(b)).why ==
              rejection::code_not_allowed);
    }
  }
}

TEST_CASE("an id belongs to the key that first installed it", "[open-addon][store]") {
  scratch_dir scratch("open_publisher");
  const open_store store(utf8(scratch / "open"));
  publisher_key first;
  publisher_key second;
  const auto package = make_package(first, theme_files());
  REQUIRE(store.install(package, store.inspect(package).sha256));

  manifest_options newer;
  newer.version = "9.0.0";
  const auto takeover = make_package(second, theme_files(), newer);
  const auto offer = store.inspect(takeover);
  REQUIRE(offer.why == rejection::other_publisher);
  REQUIRE(offer.relation == package_relation::other_publisher);
  REQUIRE(offer.m.publisher_key == second.hex());  // the sheet can say whose it was
  rejection why = rejection::none;
  REQUIRE_FALSE(store.install(takeover, offer.sha256, &why));
  REQUIRE(why == rejection::other_publisher);
  REQUIRE(store.list().at(0).version == "1.2.0");

  // Removing the installed one is the way through.
  REQUIRE(store.remove("acme.film-tones"));
  REQUIRE(store.inspect(takeover).ok());
}

TEST_CASE("updates go forward only", "[open-addon][store]") {
  scratch_dir scratch("open_versions");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto v120 = make_package(k, theme_files());
  REQUIRE(store.install(v120, store.inspect(v120).sha256));

  manifest_options o;
  o.version = "1.10.0";
  const auto v1100 = make_package(k, theme_files(), o);
  const auto update = store.inspect(v1100);
  REQUIRE(update.ok());
  REQUIRE(update.relation == package_relation::update);
  REQUIRE(update.installed_version == "1.2.0");
  REQUIRE(store.install(v1100, update.sha256));
  REQUIRE(store.list().at(0).version == "1.10.0");
  // A data add-on holds nothing open: the version it replaced is gone.
  REQUIRE_FALSE(fs::exists(scratch / "open" / "acme.film-tones" / "1.2.0"));

  const auto back = store.inspect(v120);
  REQUIRE(back.why == rejection::downgrade);
  REQUIRE(back.relation == package_relation::downgrade);
  rejection why = rejection::none;
  REQUIRE_FALSE(store.install(v120, back.sha256, &why));
  REQUIRE(why == rejection::downgrade);

  const auto again = store.inspect(v1100);
  REQUIRE(again.ok());
  REQUIRE(again.relation == package_relation::repair);
  REQUIRE(store.install(v1100, again.sha256));
}

TEST_CASE("what is installed is the package that was looked at", "[open-addon][store]") {
  scratch_dir scratch("open_changed");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto shown = make_package(k, theme_files());
  manifest_options o;
  o.name = "Film Tones Pro";
  const auto swapped = make_package(k, theme_files(), o);
  const auto offer = store.inspect(shown);
  REQUIRE(offer.ok());

  rejection why = rejection::none;
  REQUIRE_FALSE(store.install(swapped, offer.sha256, &why));
  REQUIRE(why == rejection::changed);
  REQUIRE_FALSE(store.install(shown, "", &why));
  REQUIRE(why == rejection::changed);
  REQUIRE(store.list().empty());
}

TEST_CASE("an installed add-on is checked every time it is listed", "[open-addon][store]") {
  scratch_dir scratch("open_reverify");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto package = make_package(k, theme_files());
  REQUIRE(store.install(package, store.inspect(package).sha256));
  const fs::path addon = scratch / "open" / "acme.film-tones";

  SECTION("a changed file") {
    std::string text = good_theme();
    text[text.find("#e0793a")] = '!';
    write_text(addon / "1.2.0" / "themes" / "dusk.json", text);
    const auto listed = store.list();
    REQUIRE(listed.size() == 1);
    REQUIRE(listed[0].state == install_state::invalid);
    REQUIRE(listed[0].why == rejection::file_mismatch);
    REQUIRE_FALSE(store.theme_json("acme.film-tones", "dusk"));
  }
  SECTION("a file beside the listed ones") {
    write_bytes(addon / "1.2.0" / "payload.dylib", {1, 2, 3});
    REQUIRE(store.list().at(0).why == rejection::unexpected_file);
  }
  SECTION("the record of consent gone: a folder put there by hand") {
    fs::remove(addon / "publisher.json");
    const auto listed = store.list();
    REQUIRE(listed.at(0).state == install_state::invalid);
    REQUIRE(listed.at(0).why == rejection::not_approved);
    // The way in is the sheet: the package installs as fresh.
    const auto offer = store.inspect(package);
    REQUIRE(offer.ok());
    REQUIRE(offer.relation == package_relation::fresh);
    REQUIRE(store.install(package, offer.sha256));
    REQUIRE(store.list().at(0).state == install_state::ok);
  }
  SECTION("the record names another key") {
    publisher_key other;
    write_text(addon / "publisher.json",
               "{\"schema\":1,\"key\":\"" + other.hex() + "\",\"name\":\"x\",\"approved_unix\":1}");
    REQUIRE(store.list().at(0).why == rejection::other_publisher);
  }
  SECTION("a folder under the wrong id or version") {
    fs::rename(addon / "1.2.0", addon / "1.3.0");
    REQUIRE(store.list().at(0).why == rejection::unsafe_path);
  }
}

TEST_CASE("first-party ids and stray folders are never listed as open add-ons",
          "[open-addon][store]") {
  scratch_dir scratch("open_stray");
  fs::create_directories(scratch / "open" / "import" / "1.0.0");
  fs::create_directories(scratch / "open" / ".staging" / "77");
  fs::create_directories(scratch / "open" / "Not An Id" / "1.0.0");
  const open_store store(utf8(scratch / "open"));
  REQUIRE(store.list().empty());
  REQUIRE_FALSE(store.find("import"));
  store.startup_cleanup();
  REQUIRE_FALSE(fs::exists(scratch / "open" / ".staging"));
}

TEST_CASE("a theme nobody could read is refused", "[open-addon][theme]") {
  using mv::addon::parse_theme;
  using mv::addon::theme_fault;
  REQUIRE(parse_theme(good_theme()).ok());
  REQUIRE(parse_theme(theme_with("#f2efe9")).ok());

  REQUIRE(parse_theme(theme_with("#2a2927")).fault == theme_fault::low_contrast);   // title on canvas
  REQUIRE(parse_theme(theme_with("#f2efe9", "#f0f0f0")).fault == theme_fault::low_contrast);
  REQUIRE(parse_theme(theme_with("#f2efe910")).fault == theme_fault::low_contrast); // nearly clear text
  REQUIRE(parse_theme(theme_with("#f2efe9", "#1c1b1a80")).fault == theme_fault::translucent);
  REQUIRE(parse_theme(theme_with("f2efe9")).fault == theme_fault::bad_colour);
  REQUIRE(parse_theme(theme_with("#f2efe")).fault == theme_fault::bad_colour);
  REQUIRE(parse_theme(theme_with("#gggggg")).fault == theme_fault::bad_colour);
  REQUIRE(parse_theme(R"({"schema":1,"dark":{"canvas":"#000000"}})").fault ==
          theme_fault::missing_token);
  REQUIRE(parse_theme(R"({"schema":1,"dark":{"canvas":"#000000","glow":"#ffffff"}})").fault ==
          theme_fault::unknown_token);
  REQUIRE(parse_theme(R"({"schema":1})").fault == theme_fault::malformed);
  REQUIRE(parse_theme(R"({"schema":2,"dark":{}})").fault == theme_fault::malformed);
  REQUIRE(parse_theme("").fault == theme_fault::malformed);
  REQUIRE(parse_theme(std::string(mv::addon::kThemeMaxBytes + 1, ' ')).fault ==
          theme_fault::malformed);

  // Black on white is 21 : 1; a colour on itself is 1 : 1.
  REQUIRE(mv::addon::contrast_ratio({0, 0, 0, 255}, {255, 255, 255, 255}) > 20.9);
  REQUIRE(mv::addon::contrast_ratio({90, 90, 90, 255}, {90, 90, 90, 255}) < 1.01);

  // And a package carrying one is refused whole, saying which theme and why.
  scratch_dir scratch("open_theme");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto package = make_package(k, {packed_file{"themes/dusk.json", theme_with("#2a2927")}});
  const auto offer = store.inspect(package);
  REQUIRE(offer.why == rejection::invalid_theme);
  REQUIRE(offer.detail == "dusk: low_contrast");
  REQUIRE_FALSE(store.install(package, offer.sha256));
}

// The Settings → Appearance palettes are compiled into both chromes
// (BuiltinThemes.swift and BuiltinThemes.cs). One literal per theme, identical
// in both files, so a contrast failure or a host that drifted fails here.
namespace {

std::filesystem::path builtin_theme_root() {
  namespace fs = std::filesystem;
  fs::path dir = fs::current_path();
  for (int up = 0; up < 8 && !dir.empty(); ++up) {
    if (fs::is_regular_file(dir / "src/addon/theme.h")) return dir;
    dir = dir.parent_path();
  }
  return {};
}

std::string builtin_theme_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::string> builtin_theme_rows(const std::string& text) {
  std::vector<std::string> rows;
  const std::string mark = "\"mvbuiltin ";
  for (std::size_t at = 0; (at = text.find(mark, at)) != std::string::npos;) {
    at += 1;  // keep "mvbuiltin ..." without the opening quote
    const std::size_t end = text.find('"', at);
    if (end == std::string::npos) break;
    rows.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return rows;
}

std::vector<std::string> split_ws(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == ' ') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool hex6(const std::string& text) {
  if (text.size() != 7 || text[0] != '#') return false;
  for (std::size_t i = 1; i < text.size(); ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("the built-in themes pass the contrast floor on both hosts", "[open-addon][theme]") {
  namespace fs = std::filesystem;
  const fs::path root = builtin_theme_root();
  INFO("cwd " << fs::current_path());
  REQUIRE(fs::is_directory(root));
  const fs::path swift =
      root / "src.swift/MediaViewerChrome/Sources/MediaViewerChrome/BuiltinThemes.swift";
  const fs::path csharp = root / "src.managed/MediaViewer.Chrome/BuiltinThemes.cs";
  const std::string swift_text = builtin_theme_text(swift);
  const std::string csharp_text = builtin_theme_text(csharp);
  REQUIRE(swift_text.find("mediaviewer.builtin") != std::string::npos);
  REQUIRE(csharp_text.find("mediaviewer.builtin") != std::string::npos);
  // The prefix is reserved, so a built-in selection can never name an add-on.
  REQUIRE_FALSE(mv::addon::valid_open_id("mediaviewer.builtin"));

  const std::vector<std::string> mac = builtin_theme_rows(swift_text);
  const std::vector<std::string> windows = builtin_theme_rows(csharp_text);
  REQUIRE(mac == windows);
  const char* ids[] = {
      "dark", "light", "midnight", "cyberpunk", "retrowave", "forest", "starfield",
      "terminal", "organs", "ume", "gpt", "claude", "cute", "eclipse", "porcelain",
      "arcade", "blueprint", "monolith", "yoyo"};
  const char* names[] = {
      "Dark", "Light", "Midnight", "Cyberpunk", "Retrowave", "Forest", "Starfield",
      "Terminal", "Organs", "Ume", "GPT", "Claude", "Cute", "Eclipse", "Porcelain",
      "Arcade", "Blueprint", "Monolith", "Yoyo"};
  const char* kinds[] = {
      "dark", "light", "dark", "dark", "dark", "dark", "dark",
      "dark", "dark", "dark", "dark", "dark", "light", "dark", "light",
      "dark", "dark", "dark", "dark"};
  const char* effects[] = {
      "none", "dots", "rain", "synapse", "embers", "petals", "starfield",
      "fireflies", "rain", "petals", "none", "none", "sparkles", "starfield", "dots",
      "synapse", "dots", "perlin-flow", "fireflies"};
  REQUIRE(mac.size() == 19);
  REQUIRE(mac.size() == sizeof ids / sizeof ids[0]);
  REQUIRE(mac.size() == sizeof names / sizeof names[0]);
  REQUIRE(mac.size() == sizeof kinds / sizeof kinds[0]);
  REQUIRE(mac.size() == sizeof effects / sizeof effects[0]);
  const char* tokens[] = {"canvas", "surface", "title", "body", "disabled", "hairline", "accent"};
  for (std::size_t i = 0; i < mac.size(); ++i) {
    const std::vector<std::string> parts = split_ws(mac[i]);
    // Colours, then a host-only ground effect, its intensity, speed and size.
    // The effect is not a token: parse_theme below sees the seven colours and
    // nothing else.
    REQUIRE(parts.size() == 16);
    REQUIRE(parts[0] == "mvbuiltin");
    REQUIRE(parts[1] == ids[i]);
    REQUIRE(parts[2] == names[i]);
    REQUIRE(parts[3] == kinds[i]);
    REQUIRE(parts[11] == effects[i]);
    REQUIRE(hex6(parts[12]));
    for (int tail = 13; tail <= 15; ++tail) {
      REQUIRE(parts[tail].size() == 4);
      REQUIRE(parts[tail][1] == '.');
    }
    std::string palette = "{";
    for (int t = 0; t < 7; ++t) {
      REQUIRE(hex6(parts[4 + t]));
      if (t > 0) palette += ',';
      palette += '"';
      palette += tokens[t];
      palette += "\":\"";
      palette += parts[4 + t];
      palette += "ff\"";
    }
    palette += '}';
    const std::string doc = std::string("{\"schema\":1,\"") + parts[3] + "\":" + palette + "}";
    const mv::addon::theme_result parsed = mv::addon::parse_theme(doc);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.t.font.empty());
    if (parts[3] == "dark") {
      REQUIRE(parsed.t.dark.has_value());
      REQUIRE_FALSE(parsed.t.light.has_value());
    } else {
      REQUIRE(parsed.t.light.has_value());
      REQUIRE_FALSE(parsed.t.dark.has_value());
    }
  }
}

TEST_CASE("a theme's path must be one of the package's files", "[open-addon][manifest]") {
  publisher_key k;
  const std::vector<packed_file> files{packed_file{"themes/other.json", good_theme()}};
  const std::string m = open_manifest_json(k, files);  // lists themes/dusk.json as the theme
  REQUIRE(mv::addon::check_open_manifest(bytes_from(m), k.sign(m)).why == rejection::unsafe_path);
}

TEST_CASE("any one byte of a package changed: refused, or the same add-on",
          "[open-addon][store]") {
  // The sweep a fuzzer would start from, run on every build under ASan. A
  // flipped byte is either caught (the signature, a hash, the structure) or
  // sits in a ZIP field nothing is installed from (a date, a version made
  // by); then what would be installed is unchanged, and the consent the
  // person gave is still for other bytes (the package's own SHA-256 moved).
  scratch_dir scratch("open_sweep");
  const open_store store(utf8(scratch / "open"));
  publisher_key k;
  const auto whole = make_package(k, theme_files());
  const auto original = store.inspect(whole);
  REQUIRE(original.ok());
  std::size_t refused = 0;
  for (std::size_t i = 0; i < whole.size(); ++i) {
    for (const std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      auto changed = whole;
      changed[i] ^= mask;
      const auto offer = store.inspect(changed);
      if (!offer.ok()) {
        ++refused;
        continue;
      }
      REQUIRE(offer.sha256 != original.sha256);
      REQUIRE(offer.m.id == original.m.id);
      REQUIRE(offer.m.version == original.m.version);
      REQUIRE(offer.m.publisher_key == original.m.publisher_key);
      REQUIRE(offer.m.files.size() == original.m.files.size());
      REQUIRE(offer.m.files[0].sha256 == original.m.files[0].sha256);
      rejection why = rejection::none;
      REQUIRE_FALSE(store.install(changed, original.sha256, &why));
      REQUIRE(why == rejection::changed);
    }
  }
  // Nearly every byte is load-bearing.
  REQUIRE(refused > whole.size() * 3 * 9 / 10);
  REQUIRE_FALSE(fs::exists(scratch / "open"));
}
