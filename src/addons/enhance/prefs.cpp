// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/prefs.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

bool scalar(const json::value& v) {
  return v.k == json::kind::number || v.k == json::kind::string || v.k == json::kind::boolean;
}

}  // namespace

const char* target_name(clip_target t) noexcept {
  switch (t) {
    case clip_target::picture: return "picture";
    case clip_target::sound: return "sound";
    case clip_target::both: return "both";
  }
  return "picture";
}

std::string prefs_json(const prefs& p) {
  json::writer w;
  w.begin_object();
  w.key("values").begin_object();
  for (const model_kind k : kModelKinds) {
    w.key(kind_name(k)).begin_object();
    for (const auto& [id, v] : p.of(k)) {
      w.key(id);
      write_scalar(w, v);
    }
    w.end_object();
  }
  w.end_object();
  w.key("target").string(target_name(p.target));
  w.key("wanted").begin_array();
  for (const model_kind k : kModelKinds) {
    if (p.want(k)) w.string(kind_name(k));
  }
  w.end_array();
  w.end_object();
  return w.take();
}

prefs prefs_from(std::string_view json_text) {
  prefs p;
  const auto doc = json::parse(json_text, 8);
  if (!doc || doc->k != json::kind::object) return p;
  if (const json::value* values = doc->find("values"); values && values->k == json::kind::object) {
    for (const auto& [name, kv] : values->o) {
      const auto k = kind_from(name);
      if (!k || kv.k != json::kind::object) continue;
      for (const auto& [id, v] : kv.o) {
        if (id.size() <= 32 && scalar(v)) p.of(*k)[id] = v;
      }
    }
  }
  if (const std::string* t = doc->str("target")) {
    for (const clip_target c : {clip_target::picture, clip_target::sound, clip_target::both}) {
      if (*t == target_name(c)) p.target = c;
    }
  }
  if (const json::value* wanted = doc->find("wanted"); wanted && wanted->k == json::kind::array) {
    for (const json::value& v : wanted->a) {
      if (const auto k = v.k == json::kind::string ? kind_from(v.s) : std::nullopt) p.want(*k) = true;
    }
  }
  return p;
}

prefs load_prefs(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return {};
  std::ostringstream text;
  text << in.rdbuf();
  return prefs_from(text.str());
}

bool save_prefs(const std::string& path, const prefs& p) {
  const fs::path target = fs_path(path);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = prefs_json(p);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) return false;
  }
  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

}  // namespace mv::enhance
