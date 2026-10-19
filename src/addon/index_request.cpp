// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/index_request.h"

#include <deque>
#include <mutex>

#include "core/json.h"

namespace mv::addon {
namespace {

bool absolute(std::string_view p) {
  if (p.empty()) return false;
  if (p[0] == '/') return true;
  if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
      (p[2] == '\\' || p[2] == '/')) {
    return true;
  }
  return p.size() >= 3 && p[0] == '\\' && p[1] == '\\';
}

bool whole_disk(std::string_view p) {
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.remove_suffix(1);
  return p == "/" || (p.size() == 2 && p[1] == ':') || (p.size() == 3 && p[1] == ':');
}

bool photos(std::string_view p) {
  if (p.size() < 7) return false;
  std::string head(p.substr(0, 7));
  for (char& c : head) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return head == "photos:";
}

struct state {
  std::mutex m;
  index_sink sink;
  std::deque<index_request> pending;
};

state& g() {
  static state s;
  return s;
}

void hand(const index_sink& sink, const index_request& r) {
  for (const index_folder_request& f : r.folders) (void)sink(f);
}

}  // namespace

std::string to_json(const index_request& r) {
  json::writer w;
  w.begin_object().key("version").integer(1).key("id").string(r.id).key("folders").begin_array();
  for (const index_folder_request& f : r.folders) {
    w.begin_object().key("path").string(f.path).key("recursive").boolean(f.recursive).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<index_request> index_request_from_json(std::string_view text) {
  if (text.size() > 256 * 1024) return std::nullopt;
  const auto v = json::parse(text, 8);
  if (!v || v->k != json::kind::object || v->integer("version").value_or(0) != 1) return std::nullopt;
  index_request r;
  if (const std::string* id = v->str("id")) {
    if (id->size() > 64) return std::nullopt;
    r.id = *id;
  }
  const json::value* folders = v->find("folders");
  if (!folders || folders->k != json::kind::array || folders->a.empty() || folders->a.size() > kMaxIndexFolders) {
    return std::nullopt;
  }
  for (const json::value& f : folders->a) {
    const std::string* path = f.str("path");
    if (!path || path->size() > 4096 || !absolute(*path) || photos(*path) || whole_disk(*path)) return std::nullopt;
    if (path->find('\0') != std::string::npos) return std::nullopt;
    r.folders.push_back({*path, f.boolean("recursive").value_or(true)});
  }
  return r;
}

std::string index_notification_name(std::string_view bundle_id) {
  std::string name(bundle_id.empty() ? std::string_view("io.github.longtimeno-c.mediaviewer") : bundle_id);
  name += ".agent.index";
#if defined(MV_ADDON_DEV_PUBLIC_KEY_HEX)
  name += ".dev";
#endif
  return name;
}

const char* delivery_name(index_delivery d) noexcept {
  switch (d) {
    case index_delivery::started: return "started";
    case index_delivery::queued: return "queued";
    case index_delivery::refused: return "refused";
  }
  return "refused";
}

void publish_index_requests(index_sink sink) {
  std::deque<index_request> kept;
  {
    std::lock_guard lock(g().m);
    g().sink = sink;
    kept.swap(g().pending);
  }
  // Outside the lock: the sink calls into the pack.
  for (const index_request& r : kept) hand(sink, r);
}

void retract_index_requests() noexcept {
  std::lock_guard lock(g().m);
  g().sink = nullptr;
}

index_delivery deliver_index_request(std::string_view json) {
  auto r = index_request_from_json(json);
  if (!r) return index_delivery::refused;
  index_sink sink;
  {
    std::lock_guard lock(g().m);
    if (!g().sink) {
      g().pending.push_back(std::move(*r));
      while (g().pending.size() > kMaxPendingIndexRequests) g().pending.pop_front();
      return index_delivery::queued;
    }
    sink = g().sink;
  }
  hand(sink, *r);
  return index_delivery::started;
}

}  // namespace mv::addon
