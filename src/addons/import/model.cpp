// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/model.h"

#include <initializer_list>

#include "core/json.h"

namespace mv::import {
namespace {

bool ext_is(std::string_view ext, std::initializer_list<const char*> list) noexcept {
  for (const char* e : list) {
    const std::string_view want(e);
    if (want.size() != ext.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < want.size() && same; ++i) {
      char c = ext[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      same = c == want[i];
    }
    if (same) return true;
  }
  return false;
}

const char* selection_name(selection_mode m) {
  switch (m) {
    case selection_mode::all: return "all";
    case selection_mode::marked: return "marked";
    case selection_mode::date_range: return "date_range";
    default: return "new";
  }
}

const char* layout_name(layout_kind k) {
  switch (k) {
    case layout_kind::year_month_day: return "YYYY/MM/DD";
    case layout_kind::day: return "YYYY-MM-DD";
    case layout_kind::card: return "card";
    case layout_kind::flat: return "flat";
    default: return "YYYY/YYYY-MM-DD";
  }
}

}  // namespace

file_type classify(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return file_type::none;
  const std::string_view ext = name.substr(dot);
  if (ext_is(ext, {".cr2", ".cr3", ".crw", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".orf", ".raf",
                   ".rw2", ".pef", ".ptx", ".srw", ".rwl", ".dng", ".3fr", ".fff", ".iiq", ".mef",
                   ".mos", ".raw", ".x3f", ".erf", ".kdc", ".dcr"})) {
    return file_type::raw;
  }
  if (ext_is(ext, {".jpg", ".jpeg", ".jpe"})) return file_type::jpeg;
  if (ext_is(ext, {".heic", ".heif", ".hif"})) return file_type::heic;
  // D5's video set, plus the camera containers that carry it (MTS / M2TS are
  // MPEG-TS; 3GP is MP4's sibling).
  if (ext_is(ext, {".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".ts", ".mts", ".m2ts",
                   ".3gp", ".mpg", ".mpeg"})) {
    return file_type::video;
  }
  if (ext_is(ext, {".png", ".tif", ".tiff", ".webp", ".gif", ".bmp", ".avif", ".ico"})) {
    return file_type::other;
  }
  if (ext_is(ext, {".xmp", ".thm", ".lrv", ".xml"})) return file_type::sidecar;
  return file_type::none;
}

bool parse_preset(std::string_view text, preset& out) {
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return false;
  preset p;
  const auto str = [&](const char* key, std::string& field) -> bool {
    const json::value* v = doc->find(key);
    if (!v) return true;
    if (v->k != json::kind::string) return false;
    field = v->s;
    return true;
  };
  const auto flag = [&](const char* key, bool& field) -> bool {
    const json::value* v = doc->find(key);
    if (!v) return true;
    if (v->k != json::kind::boolean) return false;
    field = v->b;
    return true;
  };
  if (!str("name", p.name) || !str("range_from", p.range_from) || !str("range_to", p.range_to) ||
      !str("destination", p.destination) || !str("backup", p.backup) ||
      !str("rename", p.rename) || !flag("layout_camera", p.layout_camera) ||
      !flag("layout_type", p.layout_type) || !flag("skip_duplicates", p.skip_duplicates) ||
      !flag("full_verify", p.full_verify) || !flag("companions", p.companions) ||
      !flag("eject_after", p.eject_after) || !flag("open_after", p.open_after) ||
      !flag("notify", p.notify) || !flag("fast", p.fast)) {
    return false;
  }
  if (p.name.empty()) return false;

  if (const json::value* v = doc->find("selection")) {
    if (v->k != json::kind::string) return false;
    if (v->s == "new") p.selection = selection_mode::new_only;
    else if (v->s == "all") p.selection = selection_mode::all;
    else if (v->s == "marked") p.selection = selection_mode::marked;
    else if (v->s == "date_range") p.selection = selection_mode::date_range;
    else return false;
  }
  if (const json::value* v = doc->find("layout")) {
    if (v->k != json::kind::string) return false;
    if (v->s == "YYYY/YYYY-MM-DD") p.layout = layout_kind::year_day;
    else if (v->s == "YYYY/MM/DD") p.layout = layout_kind::year_month_day;
    else if (v->s == "YYYY-MM-DD") p.layout = layout_kind::day;
    else if (v->s == "card") p.layout = layout_kind::card;
    else if (v->s == "flat") p.layout = layout_kind::flat;
    else return false;
  }
  if (const json::value* v = doc->find("dates")) {
    if (v->k != json::kind::string) return false;
    if (v->s == "taken") p.dates = date_source::taken;
    else if (v->s == "file_time") p.dates = date_source::file_time;
    else return false;
  }
  if (const json::value* v = doc->find("scope")) {
    if (v->k != json::kind::string) return false;
    if (v->s == "destination") p.scope = dup_scope::destination;
    else if (v->s == "library") p.scope = dup_scope::library;
    else return false;
  }
  if (const json::value* v = doc->find("types")) {
    if (v->k != json::kind::array) return false;
    p.types = 0;
    for (const json::value& t : v->a) {
      if (t.k != json::kind::string) return false;
      if (t.s == "raw") p.types |= kTypeRaw;
      else if (t.s == "jpeg") p.types |= kTypeJpeg;
      else if (t.s == "heic") p.types |= kTypeHeic;
      else if (t.s == "video") p.types |= kTypeVideo;
      else if (t.s == "other") p.types |= kTypeOther;
      else return false;
    }
  }
  out = std::move(p);
  return true;
}

std::string preset_to_json(const preset& p) {
  json::writer w;
  w.begin_object();
  w.key("name").string(p.name);
  w.key("selection").string(selection_name(p.selection));
  w.key("range_from").string(p.range_from);
  w.key("range_to").string(p.range_to);
  w.key("types").begin_array();
  if (p.types & kTypeRaw) w.string("raw");
  if (p.types & kTypeJpeg) w.string("jpeg");
  if (p.types & kTypeHeic) w.string("heic");
  if (p.types & kTypeVideo) w.string("video");
  if (p.types & kTypeOther) w.string("other");
  w.end_array();
  w.key("destination").string(p.destination);
  w.key("backup").string(p.backup);
  w.key("layout").string(layout_name(p.layout));
  w.key("layout_camera").boolean(p.layout_camera);
  w.key("layout_type").boolean(p.layout_type);
  w.key("dates").string(p.dates == date_source::taken ? "taken" : "file_time");
  w.key("rename").string(p.rename);
  w.key("skip_duplicates").boolean(p.skip_duplicates);
  w.key("scope").string(p.scope == dup_scope::library ? "library" : "destination");
  w.key("full_verify").boolean(p.full_verify);
  w.key("companions").boolean(p.companions);
  w.key("eject_after").boolean(p.eject_after);
  w.key("open_after").boolean(p.open_after);
  w.key("notify").boolean(p.notify);
  w.key("fast").boolean(p.fast);
  w.end_object();
  return w.take();
}

}  // namespace mv::import
