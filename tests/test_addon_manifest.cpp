// SPDX-License-Identifier: GPL-2.0-or-later
// The add-on mechanism (plan/18 "Add-ons"; PR 16 verify: "a tampered add-on
// file or manifest is refused").
#include "catch_compat.h"

#include <sodium.h>

#include <mediaviewer/mediaviewer_import.h>

#include <cstring>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::addon::rejection;

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

struct file_spec {
  std::string path;
  std::vector<std::uint8_t> bytes;
};

std::string manifest_json(const std::vector<file_spec>& files, const std::string& native,
                          const std::string& version = "1.2.3",
                          std::string platform = std::string(mv::addon::kPlatform),
                          int host_min = 1, int host_max = 1) {
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("import");
  w.key("name").string("Import");
  w.key("version").string(version);
  w.key("platform").string(platform);
  w.key("host_api").begin_object().key("min").integer(host_min).key("max").integer(host_max).end_object();
  w.key("installed_size").integer(3 * 1024 * 1024);
  w.key("native").string(native);
  w.key("chrome").string(native);
  w.key("archive").begin_object();
  w.key("path").string("mediaviewer-addon-import-test.zip");
  w.key("sha256").string(std::string(64, 'a'));
  w.key("size").integer(1);
  w.end_object();
  w.key("files").begin_array();
  for (const file_spec& f : files) {
    w.begin_object();
    w.key("path").string(f.path);
    w.key("sha256").string(mv::addon::sha256_hex(f.bytes));
    w.key("size").integer(static_cast<std::int64_t>(f.bytes.size()));
    w.key("licence").string("GPL-2.0-or-later");
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

void stage(const fs::path& dir, const std::vector<file_spec>& files, const std::string& manifest,
           const std::vector<std::uint8_t>& sig) {
  for (const file_spec& f : files) write_bytes(dir / fs::path(f.path), f.bytes);
  write_text(dir / "manifest.json", manifest);
  write_bytes(dir / "manifest.json.sig", sig);
}

}  // namespace

TEST_CASE("a manifest is trusted only with a good signature from the pinned key",
          "[addon][manifest]") {
  keypair k;
  const std::vector<file_spec> files{{"mv_import.bin", {1, 2, 3}}};
  const std::string m = manifest_json(files, "mv_import.bin");
  const auto sig = k.sign(m);

  auto ok = mv::addon::check_manifest(bytes_of(m), sig, k.pub, MV_ADDON_HOST_API);
  REQUIRE(ok.trusted());
  REQUIRE(ok.m.id == "import");
  REQUIRE(ok.m.version == "1.2.3");
  REQUIRE(ok.m.files.size() == 1);

  std::string tampered = m;
  tampered[tampered.find("1.2.3")] = '9';
  REQUIRE(mv::addon::check_manifest(bytes_of(tampered), sig, k.pub, MV_ADDON_HOST_API).why ==
          rejection::bad_signature);
  REQUIRE(mv::addon::check_manifest(bytes_of(m), {}, k.pub, MV_ADDON_HOST_API).why ==
          rejection::missing_signature);
  keypair other;
  REQUIRE(mv::addon::check_manifest(bytes_of(m), sig, other.pub, MV_ADDON_HOST_API).why ==
          rejection::bad_signature);
  const std::vector<std::uint8_t> zeros(32, 0);
  REQUIRE(mv::addon::check_manifest(bytes_of(m), sig, zeros, MV_ADDON_HOST_API).why ==
          rejection::key_not_configured);

  // The pinned production key is the updater's.
  const auto pinned = mv::addon::pinned_public_key();
  REQUIRE(pinned[0] == 0x04);
  REQUIRE(pinned[31] == 0x8c);
}

TEST_CASE("manifest policy: platform, host API range, unsafe paths", "[addon][manifest]") {
  keypair k;
  const std::vector<file_spec> files{{"lib/mv_import.bin", {1}}};
  {
    const std::string m = manifest_json(files, "lib/mv_import.bin", "1.0.0", "win-arm64");
    REQUIRE(mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, 1).why ==
            rejection::wrong_platform);
  }
  {
    const std::string m = manifest_json(files, "lib/mv_import.bin", "1.0.0",
                                        std::string(mv::addon::kPlatform), 2, 3);
    // "Import needs an update", not a crash.
    REQUIRE(mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, 1).why ==
            rejection::needs_update);
  }
  for (const char* bad : {"../evil.dll", "/abs.dll", "a\\b.dll", "C:x.dll", "a//b", "./a"}) {
    const std::vector<file_spec> f{{bad, {1}}};
    const std::string m = manifest_json(f, "x");
    REQUIRE(mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, 1).why !=
            rejection::none);
  }
  REQUIRE(mv::addon::safe_relative_path("Import.bundle/Contents/MacOS/Import"));
  // The name is the add-on's folder on the Mac: never a way out of Add-ons/.
  for (const char* bad : {"..", ".", ".hidden", "a/b", "a\\b", "C:"}) {
    std::string m = manifest_json(files, "lib/mv_import.bin");
    const std::string from = R"("name":"Import")";
    m.replace(m.find(from), from.size(), std::string(R"("name":")") + bad + "\"");
    REQUIRE(mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, 1).why !=
            rejection::none);
  }
}

TEST_CASE("install verifies every file; a tampered or extra file is refused", "[addon][store]") {
  keypair k;
  scratch_dir s("addons");
  mv::addon::store st(utf8(s.root()), k.pub, MV_ADDON_HOST_API);
  const std::vector<file_spec> files{{"mv_import.bin", pattern(4096, 7)},
                                     {"LICENSES/THIRD-PARTY.md", {'o', 'k'}}};
  const std::string m = manifest_json(files, "mv_import.bin");

  SECTION("a good package installs, lists, and removes") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    auto inst = st.install(*staged);
    REQUIRE(inst);
    REQUIRE(inst->state == mv::addon::install_state::ok);
    REQUIRE_FALSE(fs::exists(*staged));  // consumed
    auto listed = st.list();
    REQUIRE(listed.size() == 1);
    REQUIRE(listed[0].version == "1.2.3");
    // Sideloaded tamper after install: the next load refuses it.
    write_bytes(fs::path(listed[0].dir) / "mv_import.bin", pattern(4096, 8));
    REQUIRE(st.list()[0].state == mv::addon::install_state::invalid);
    REQUIRE(st.remove("import", false));
    REQUIRE(st.list().empty());
  }
  SECTION("a changed file") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    write_bytes(fs::path(*staged) / "mv_import.bin", pattern(4096, 9));
    REQUIRE_FALSE(st.install(*staged));
    REQUIRE(st.list().empty());
  }
  SECTION("an extra file dropped beside them") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    write_text(fs::path(*staged) / "version.dll", "planted");
    REQUIRE_FALSE(st.install(*staged));
  }
  SECTION("a hidden file dropped beside them") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    // LoadLibrary / dlopen do not care that a dependency is hidden.
    write_text(fs::path(*staged) / ".version.dll", "planted");
    REQUIRE_FALSE(st.install(*staged));
  }
  SECTION("Finder's .DS_Store is not a tamper") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    write_text(fs::path(*staged) / "LICENSES/.DS_Store", "finder");
    REQUIRE(st.install(*staged));
  }
#if !defined(_WIN32)
  SECTION("a listed file that is a link") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));
    // Same bytes, but through a link the verify cannot pin down.
    const fs::path real = s.root() / "elsewhere.bin";
    write_bytes(real, pattern(4096, 7));
    fs::remove(fs::path(*staged) / "mv_import.bin");
    fs::create_symlink(real, fs::path(*staged) / "mv_import.bin");
    REQUIRE_FALSE(st.install(*staged));
  }
#endif
  SECTION("an older signed version does not replace a newer one") {
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, k.sign(m));  // 1.2.3
    REQUIRE(st.install(*staged));
    const std::string older = manifest_json(files, "mv_import.bin", "1.1.9");
    auto old_stage = st.make_staging();
    REQUIRE(old_stage);
    stage(*old_stage, files, older, k.sign(older));
    REQUIRE_FALSE(st.install(*old_stage));
    REQUIRE_FALSE(fs::exists(*old_stage));  // consumed all the same
    REQUIRE(st.list()[0].version == "1.2.3");
    // The same version again is a repair.
    auto again = st.make_staging();
    REQUIRE(again);
    stage(*again, files, m, k.sign(m));
    REQUIRE(st.install(*again));
    // A newer one updates.
    const std::string newer = manifest_json(files, "mv_import.bin", "1.10.0");
    auto new_stage = st.make_staging();
    REQUIRE(new_stage);
    stage(*new_stage, files, newer, k.sign(newer));
    REQUIRE(st.install(*new_stage));
    REQUIRE(st.list()[0].version == "1.10.0");
  }
  SECTION("a manifest signed by someone else") {
    keypair other;
    auto staged = st.make_staging();
    REQUIRE(staged);
    stage(*staged, files, m, other.sign(m));
    REQUIRE_FALSE(st.install(*staged));
  }
}

TEST_CASE("with no add-on installed nothing is written and nothing loads", "[addon][store]") {
  keypair k;
  scratch_dir s("addons_absent");
  mv::addon::store st(utf8(s.root()), k.pub, MV_ADDON_HOST_API);
  REQUIRE(st.list().empty());
  st.startup_cleanup();
  REQUIRE(list_tree(s.root()).empty());
  REQUIRE_FALSE(mv::addon::loaded_addon::load(st, "import", {}));
}

TEST_CASE("the real Import module installs, loads, answers, and shuts down", "[addon][load]") {
  keypair k;
  scratch_dir s("addons_real");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::vector<std::uint8_t> module = read_bytes(MV_IMPORT_MODULE_PATH);
  REQUIRE(module.size() > 1000);
  const std::string native = utf8(fs::path(MV_IMPORT_MODULE_PATH).filename());
  const std::vector<file_spec> files{{native, module}};
  const std::string m = manifest_json(files, native);
  auto staged = st.make_staging();
  REQUIRE(staged);
  stage(*staged, files, m, k.sign(m));
  REQUIRE(st.install(*staged));

  mv::addon::host_services svc;
  svc.default_library = utf8(s / "Pictures/MediaViewer");
  auto loaded = mv::addon::loaded_addon::load(st, "import", svc);
  REQUIRE(loaded);
  REQUIRE(std::string((*loaded)->api().id) == "import");
  REQUIRE((*loaded)->query("mv.nothing.1") == nullptr);
  const auto* api = static_cast<const mv_import_api*>((*loaded)->query(MV_IMPORT_INTERFACE));
  REQUIRE(api);
  REQUIRE(api->struct_size == sizeof(mv_import_api));

  char small[4];
  std::uint32_t needed = 0;
  REQUIRE(api->presets_json(api->ctx, small, sizeof small, &needed) == MV_ERR_INVALID_ARG);
  REQUIRE(needed > 4);
  std::string buf(needed, '\0');
  REQUIRE(api->presets_json(api->ctx, buf.data(), needed, &needed) == MV_OK);
  REQUIRE(buf.find("\"Default\"") != std::string::npos);

  // import.db lives in the add-on's own data folder.
  loaded->reset();
  REQUIRE(fs::exists(fs::path(st.list()[0].data_dir) / "import.db"));
  // Removing it can keep or delete that data.
  REQUIRE(st.remove("import", true));
  REQUIRE(st.list().empty());
}
