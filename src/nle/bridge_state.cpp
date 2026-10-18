// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/bridge_state.h"

#include "core/json.h"

namespace mv::nle {

std::string to_json(const bridge_state& s) {
  json::writer w;
  w.begin_object();
  w.key("version").integer(1);
  w.key("project").string(s.project);
  w.key("event").string(s.event);
  w.key("library").string(s.library);
  w.key("fcpxml_version").string(s.fcpxml_version);
  w.key("dragged_ms").integer(s.dragged_ms);
  w.key("changed_ms").integer(s.changed_ms);
  w.key("active_project").string(s.active_project);
  // Milliseconds, as an integer: the writer's numbers are fixed-point.
  w.key("playhead_ms").integer(s.playhead_s < 0 ? -1 : static_cast<std::int64_t>(s.playhead_s * 1000.0 + 0.5));
  w.key("playhead_ms_at").integer(s.playhead_ms_at);
  w.key("folders").begin_array();
  for (const std::string& f : s.folders) w.string(f);
  w.end_array();
  w.key("versions").begin_array();
  for (const bridge_version& v : s.versions) {
    w.begin_object();
    w.key("name").string(v.name);
    w.key("file").string(v.file);
    w.key("at_ms").integer(v.at_ms);
    w.key("base").string(v.base);
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

std::optional<bridge_state> bridge_state_from_json(std::string_view text) {
  const auto v = json::parse(text);
  if (!v || v->k != json::kind::object) return std::nullopt;
  if (v->integer("version").value_or(0) != 1) return std::nullopt;
  bridge_state s;
  auto str = [&](const char* k, std::string& out) {
    if (const std::string* x = v->str(k)) out = *x;
  };
  str("project", s.project);
  str("event", s.event);
  str("library", s.library);
  str("fcpxml_version", s.fcpxml_version);
  str("active_project", s.active_project);
  s.dragged_ms = v->integer("dragged_ms").value_or(0);
  s.changed_ms = v->integer("changed_ms").value_or(0);
  const std::int64_t ph = v->integer("playhead_ms").value_or(-1);
  s.playhead_s = ph < 0 ? -1 : static_cast<double>(ph) / 1000.0;
  s.playhead_ms_at = v->integer("playhead_ms_at").value_or(0);
  if (const json::value* f = v->find("folders"); f && f->k == json::kind::array) {
    for (const json::value& e : f->a) {
      if (e.k == json::kind::string && !e.s.empty()) s.folders.push_back(e.s);
    }
  }
  if (const json::value* vs = v->find("versions"); vs && vs->k == json::kind::array) {
    for (const json::value& e : vs->a) {
      if (e.k != json::kind::object) continue;
      bridge_version b;
      if (const std::string* x = e.str("name")) b.name = *x;
      if (const std::string* x = e.str("file")) b.file = *x;
      if (const std::string* x = e.str("base")) b.base = *x;
      b.at_ms = e.integer("at_ms").value_or(0);
      if (!b.name.empty() && !b.file.empty()) s.versions.push_back(std::move(b));
    }
  }
  return s;
}

std::string version_file_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|') {
      out += '_';
    } else {
      out += c;
    }
  }
  while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(0, 1);
  if (out.empty()) out = "version";
  if (out.size() > 200) out.resize(200);
  return out + ".fcpxml";
}

std::string dragged_ago(std::int64_t dragged_ms, std::int64_t now_ms) {
  const std::int64_t s = (now_ms - dragged_ms) / 1000;
  if (s < 60) return "dragged just now";
  if (s < 3600) return "dragged " + std::to_string(s / 60) + " min ago";
  if (s < 86400) return "dragged " + std::to_string(s / 3600) + " h ago";
  if (s < 2 * 86400) return "dragged yesterday";
  return "dragged " + std::to_string(s / 86400) + " days ago";
}

}  // namespace mv::nle
