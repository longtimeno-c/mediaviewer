// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #482 T1: model tiers as data (docs/plans/model-tiers.md). The profile reader's
// rules, and that the manifest's Default tier is the folders packs staged
// before tiers.json carried, so a pack staged either way opens the same towers
// and nothing re-indexes.
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "addons/ai/tiers.h"
#include "core/json.h"

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

constexpr const char* kTwoTiers = R"({
 "default": "default",
 "profiles": {
  "default": {"name": "Default",
   "pictures": {"high": "models/clip-l14", "fast": "models/clip-b32"},
   "speech": {"high": "models/whisper-small", "fast": "models/whisper-base"},
   "sounds": "models/clap-general", "faces": "models/faces", "documents": "models/doc-text"},
  "intel": {"name": "Intel",
   "pictures": {"fast": "models/clip-b32"},
   "speech": {"high": null, "fast": "models/whisper-base"},
   "sounds": "models/clap-general", "faces": "models/faces", "documents": "models/doc-text"}
 }
})";

std::string with_default(const std::string& profile_body) {
  return R"({"default": "t", "profiles": {"t": {"name": "T", )" + profile_body + "}}}";
}

}  // namespace

TEST_CASE("a tier profile names a folder per role", "[ai][tiers]") {
  auto d = mv::ai::parse_tier_profile(kTwoTiers, "");
  REQUIRE(d);
  CHECK(d->id == "default");
  CHECK(d->name == "Default");
  CHECK(d->picture_high == "models/clip-l14");
  CHECK(d->picture_fast == "models/clip-b32");
  CHECK(d->speech_high == "models/whisper-small");
  CHECK(d->speech_fast == "models/whisper-base");
  CHECK(d->sounds == "models/clap-general");
  CHECK(d->faces == "models/faces");
  CHECK(d->documents == "models/doc-text");

  // One tower is a tier too: the empty slot stays empty.
  auto i = mv::ai::parse_tier_profile(kTwoTiers, "intel");
  REQUIRE(i);
  CHECK(i->picture_high.empty());
  CHECK(i->picture_fast == "models/clip-b32");
  CHECK(i->speech_high.empty());

  auto missing = mv::ai::parse_tier_profile(kTwoTiers, "maxxed");
  REQUIRE_FALSE(missing);
  CHECK(missing.error() == mv::status::not_found);
}

TEST_CASE("a tier profile cannot point outside its piece's models folder", "[ai][tiers]") {
  const std::string rest = R"("speech": {"fast": "models/whisper-base"}, "sounds": "models/clap-general",
                             "faces": "models/faces", "documents": "models/doc-text")";
  CHECK(mv::ai::parse_tier_profile(with_default(R"("pictures": {"fast": "models/clip-b32"}, )" + rest), ""));
  for (const char* bad : {"../clip-b32", "models/../../x", "models/..", "models/a/b", "models\\\\a",
                          "/etc/x", "models/", "C:models", "models/c:x", "clip-b32"}) {
    INFO(bad);
    CHECK_FALSE(mv::ai::parse_tier_profile(
        with_default(std::string(R"("pictures": {"fast": ")") + bad + R"("}, )" + rest), ""));
  }
  // No tower at all, an unknown slot, a number where a folder goes.
  CHECK_FALSE(mv::ai::parse_tier_profile(with_default(R"("pictures": {"high": null}, )" + rest), ""));
  CHECK_FALSE(mv::ai::parse_tier_profile(with_default(R"("pictures": {"huge": "models/x"}, )" + rest), ""));
  CHECK_FALSE(mv::ai::parse_tier_profile(with_default(R"("pictures": {"fast": 3}, )" + rest), ""));
  CHECK_FALSE(mv::ai::parse_tier_profile("{}", ""));
  CHECK_FALSE(mv::ai::parse_tier_profile("not json", ""));
}

TEST_CASE("a pack without tiers.json opens today's folders", "[ai][tiers]") {
  const fs::path dir = fs::temp_directory_path() / "mv_ai_tiers_test";
  fs::remove_all(dir);
  fs::create_directories(dir / "models");
  const auto utf8 = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  const mv::ai::tier_profile legacy = mv::ai::legacy_profile();
  CHECK(mv::ai::read_tier_profile(utf8(dir), "").picture_high == legacy.picture_high);

  // A file naming other folders wins; an unknown tier falls back to its default.
  std::ofstream(dir / "models" / "tiers.json", std::ios::binary) << kTwoTiers;
  CHECK(mv::ai::read_tier_profile(utf8(dir), "intel").picture_high.empty());
  CHECK(mv::ai::read_tier_profile(utf8(dir), "maxxed").id == "default");

  // A broken file still opens what the pack has.
  std::ofstream(dir / "models" / "tiers.json", std::ios::binary | std::ios::trunc) << "{";
  CHECK(mv::ai::read_tier_profile(utf8(dir), "").documents == legacy.documents);
  fs::remove_all(dir);
}

TEST_CASE("the manifest's Default tier is the folders packs carried before tiers", "[ai][tiers]") {
  const fs::path manifest = fs::path(MV_AI_TEST_DATA) / ".." / ".." / ".." / "tools" / "package" / "ai-models.json";
  const auto doc = mv::json::parse(read_text(manifest), 16);
  REQUIRE(doc);
  const mv::json::value* tiers = doc->find("tiers");
  REQUIRE(tiers);
  const std::string* def = tiers->str("default");
  REQUIRE(def);
  REQUIRE(*def == "default");
  const mv::json::value* p = tiers->find("profiles")->find(*def);
  REQUIRE(p);
  const auto folder = [](const mv::json::value* v) { return v && v->k == mv::json::kind::string ? v->s : std::string(); };
  const mv::ai::tier_profile legacy = mv::ai::legacy_profile();
  CHECK(folder(p->find("pictures")->find("high")) == legacy.picture_high);
  CHECK(folder(p->find("pictures")->find("fast")) == legacy.picture_fast);
  CHECK(folder(p->find("speech")->find("high")) == legacy.speech_high);
  CHECK(folder(p->find("speech")->find("fast")) == legacy.speech_fast);
  CHECK(folder(p->find("sounds")) == legacy.sounds);
  CHECK(folder(p->find("faces")) == legacy.faces);
  CHECK(folder(p->find("documents")) == legacy.documents);
}
