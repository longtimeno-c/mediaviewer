// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Final Cut Pro add-on through the real loader (docs/design/23, #485): its
// manifest contributes nothing, so no window loads it; `MediaViewer
// --search-agent` loads it and reaches the agent through mv.fcp.1. The
// listener itself needs launchd (a Mach service): mv-search-client drives that
// (docs/DEVELOPMENT.md, "Final Cut Pro search").
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_fcp.h>
#include <sodium.h>

#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;

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

std::string fcp_manifest(const std::string& native, const std::vector<std::uint8_t>& lib) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_FCP_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  REQUIRE_FALSE(doc->find("contributes"));
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("fcp");
  w.key("name").string("Final Cut Pro");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(lib.size()));
  w.key("native").string(native);
  w.key("chrome").string("");
  w.key("archive").begin_object().key("path").string("fcp.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  w.begin_object().key("path").string(native).key("sha256").string(mv::addon::sha256_hex(lib))
      .key("size").integer(static_cast<std::int64_t>(lib.size())).key("licence").string("GPL-3.0-or-later")
      .end_object();
  w.end_array();
  w.key("description").string(*doc->str("description"));
  w.end_object();
  return w.take();
}

}  // namespace

TEST_CASE("fcp: the add-on loads for --search-agent only and answers mv.fcp.1", "[addon][fcp]") {
  keypair k;
  scratch_dir s("addons_fcp");
  fs::create_directories(s / "addons");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  const std::string native = utf8(fs::path(MV_FCP_MODULE_PATH).filename());
  const std::vector<std::uint8_t> lib = read_bytes(MV_FCP_MODULE_PATH);
  REQUIRE(lib.size() > 1000);
  const std::string m = fcp_manifest(native, lib);

  // No chrome and no contributions: trusted, and no window loads it.
  const auto d = mv::addon::check_manifest(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()), k.sign(m), k.pub,
      MV_ADDON_HOST_API);
  REQUIRE(d.trusted());
  CHECK_FALSE(mv::addon::loads_for_slots(d.m));

  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / native, lib);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  mv::addon::host_services svc;
  svc.post = [](const mv_addon_event&) {};
  svc.should_yield = [] { return false; };
  auto loaded = mv::addon::loaded_addon::load(st, "fcp", std::move(svc));
  REQUIRE(loaded);
  CHECK(std::string((*loaded)->api().id) == "fcp");
  CHECK((*loaded)->query("mv.agent.1") == nullptr);
  const auto* api = static_cast<const mv_fcp_api*>((*loaded)->query(MV_FCP_INTERFACE));
  REQUIRE(api);
  REQUIRE(api->struct_size >= sizeof(mv_fcp_api));
  REQUIRE(api->run_agent);

  // A host that lends no search, or an older table, is refused before anything listens.
  CHECK(api->run_agent((*loaded)->api().addon, "dev.mediaviewer.fcp.search", nullptr, 0) == MV_ERR_INVALID_ARG);
  mv_fcp_services none{};
  none.struct_size = sizeof none;
  CHECK(api->run_agent((*loaded)->api().addon, "dev.mediaviewer.fcp.search", &none, 0) == MV_ERR_INVALID_ARG);
  mv_fcp_services old{};
  old.struct_size = sizeof old - 8;
  CHECK(api->run_agent((*loaded)->api().addon, "dev.mediaviewer.fcp.search", &old, 0) == MV_ERR_INVALID_ARG);
  CHECK(api->run_agent((*loaded)->api().addon, nullptr, &none, 0) == MV_ERR_INVALID_ARG);
}
