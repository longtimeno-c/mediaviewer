// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/tiers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json.h"

namespace mv::ai {
namespace {

// "models/<name>": one level under the piece's models folder, nothing that
// could climb out of it.
bool folder_ok(std::string_view f) {
  constexpr std::string_view kPrefix = "models/";
  if (f.size() <= kPrefix.size() || f.substr(0, kPrefix.size()) != kPrefix) return false;
  const std::string_view name = f.substr(kPrefix.size());
  return name != "." && name != ".." && name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos && name.find(':') == std::string_view::npos;
}

// A role's folder: a string, or absent / null (empty). Anything else is corrupt.
bool read_folder(const json::value* v, std::string& out) {
  if (!v || v->k == json::kind::null) return true;
  if (v->k != json::kind::string || !folder_ok(v->s)) return false;
  out = v->s;
  return true;
}

bool read_slots(const json::value* v, std::string& high, std::string& fast) {
  if (!v || v->k != json::kind::object) return false;
  for (const auto& member : v->o) {
    if (member.first != "high" && member.first != "fast") return false;
  }
  return read_folder(v->find("high"), high) && read_folder(v->find("fast"), fast) &&
         !(high.empty() && fast.empty());
}

}  // namespace

static result<tier_profile> profile_of(const json::value* p, const std::string& id) {
  if (!p) return err(status::not_found);
  if (p->k != json::kind::object) return err(status::corrupt);
  tier_profile t;
  t.id = id;
  const std::string* name = p->str("name");
  if (!name || name->empty()) return err(status::corrupt);
  t.name = *name;
  if (!read_slots(p->find("pictures"), t.picture_high, t.picture_fast) ||
      !read_slots(p->find("speech"), t.speech_high, t.speech_fast) ||
      !read_folder(p->find("sounds"), t.sounds) || !read_folder(p->find("faces"), t.faces) ||
      !read_folder(p->find("documents"), t.documents)) {
    return err(status::corrupt);
  }
  if (const json::value* f = p->find("folders")) {
    if (f->k != json::kind::object) return err(status::corrupt);
    for (const auto& [piece, list] : f->o) {
      if (list.k != json::kind::array) return err(status::corrupt);
      auto& out = t.folders[piece];
      for (const json::value& v : list.a) {
        if (v.k != json::kind::string || !folder_ok(v.s)) return err(status::corrupt);
        out.push_back(v.s);
      }
    }
  }
  if (const json::value* c = p->find("clips"); c && c->k == json::kind::object) {
    t.min_gap_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(c->integer("min_gap_ms").value_or(0), 0, 60000));
    t.max_gap_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(c->integer("max_gap_ms").value_or(0), 0, 600000));
  }
  t.sounds_off_by_default = p->boolean("sounds_off_by_default").value_or(false);
  return t;
}

result<tier_profile> parse_tier_profile(std::string_view json_text, std::string_view tier) {
  const auto doc = json::parse(json_text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const json::value* profiles = doc->find("profiles");
  if (!profiles || profiles->k != json::kind::object) return err(status::corrupt);
  std::string id(tier);
  if (id.empty()) {
    const std::string* d = doc->str("default");
    if (!d) return err(status::corrupt);
    id = *d;
  }
  return profile_of(profiles->find(id), id);
}

result<tier_set> parse_tier_set(std::string_view json_text) {
  const auto doc = json::parse(json_text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const json::value* profiles = doc->find("profiles");
  const std::string* d = doc->str("default");
  if (!profiles || profiles->k != json::kind::object || !d) return err(status::corrupt);
  tier_set s;
  s.default_id = *d;
  for (const auto& [id, v] : profiles->o) {
    if (auto t = profile_of(&v, id)) s.tiers.push_back(std::move(*t));
  }
  if (s.tiers.empty()) return err(status::corrupt);
  return s;
}

tier_set read_tier_set(const std::string& core_dir) {
  std::string path = core_dir;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
  path += "models/tiers.json";
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (in) {
    std::ostringstream text;
    text << in.rdbuf();
    if (auto s = parse_tier_set(text.str())) return std::move(*s);
  }
  tier_set s;
  s.default_id = "default";
  s.tiers.push_back(legacy_profile());
  return s;
}

tier_profile legacy_profile() {
  tier_profile t;
  t.id = "default";
  t.name = "Default";
  t.picture_high = "models/clip-l14";
  t.picture_fast = "models/clip-b32";
  t.speech_high = "models/whisper-small";
  t.speech_fast = "models/whisper-base";
  t.sounds = "models/clap-general";
  t.faces = "models/faces";
  t.documents = "models/doc-text";
  return t;
}

tier_profile read_tier_profile(const std::string& core_dir, std::string_view tier) {
  std::string path = core_dir;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
  path += "models/tiers.json";
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!in) return legacy_profile();
  std::ostringstream text;
  text << in.rdbuf();
  auto t = parse_tier_profile(text.str(), tier);
  if (!t && !tier.empty()) t = parse_tier_profile(text.str(), {});
  return t ? std::move(*t) : legacy_profile();
}

}  // namespace mv::ai
