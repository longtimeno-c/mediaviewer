// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/drive_copy.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "addons/driftwire/cli/drives.h"
#include "core/json.h"

namespace mv::driftwire::cli::drive_copy {
namespace {

std::string fold(std::string_view s, bool on) {
  std::string out(s);
  if (on) {
    // ASCII only: NTFS's and APFS's full case tables are larger, but a
    // non-ASCII name that differs in case is then a new file and an old one
    // in the bin, which loses nothing.
    for (char& c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string_view parent_of(std::string_view p) {
  const auto slash = p.rfind('/');
  return slash == std::string_view::npos ? std::string_view{} : p.substr(0, slash);
}

std::string_view base_of(std::string_view p) {
  const auto slash = p.rfind('/');
  return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

bool ends_with(std::string_view s, std::string_view tail) {
  return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

std::size_t depth_of(std::string_view p) { return static_cast<std::size_t>(std::count(p.begin(), p.end(), '/')); }

std::string_view top_of(std::string_view p) {
  const auto slash = p.find('/');
  return slash == std::string_view::npos ? p : p.substr(0, slash);
}

}  // namespace

plan make_plan(const plan_input& in) {
  plan out;
  std::unordered_map<std::string, const entry*> src, prev, cpy;
  for (const auto& e : in.source) src.emplace(fold(e.path, in.fold_case), &e);
  if (in.has_previous) {
    for (const auto& e : in.previous_source) prev.emplace(fold(e.path, in.fold_case), &e);
  }
  std::vector<const entry*> partials;
  for (const auto& e : in.copy) {
    if (e.kind == entry_kind::file && ends_with(e.path, kPartSuffix)) {
      partials.push_back(&e);
      continue;
    }
    cpy.emplace(fold(e.path, in.fold_case), &e);
  }

  // A copy path as it will be spelled once the renames before it have run:
  // each part the source also has takes the source's spelling.
  auto live = [&](std::string_view copy_path) {
    std::string result;
    std::size_t start = 0;
    while (start <= copy_path.size()) {
      auto slash = copy_path.find('/', start);
      if (slash == std::string_view::npos) slash = copy_path.size();
      const std::string_view prefix = copy_path.substr(0, slash);
      const auto it = src.find(fold(prefix, in.fold_case));
      const std::string_view part = copy_path.substr(start, slash - start);
      if (!result.empty()) result.push_back('/');
      result += it != src.end() ? base_of(it->second->path) : part;
      start = slash + 1;
    }
    return result;
  };

  std::vector<action> renames, deletions, holds, links;
  for (const auto* p : partials) out.actions.push_back({action_kind::drop_partial, live(p->path), "", p->size});

  // The copy's entries the source no longer has (or has as the other kind).
  std::unordered_set<std::string> gone;
  for (const auto& [key, e] : cpy) {
    const auto it = src.find(key);
    const bool same_kind = it != src.end() && (it->second->kind == entry_kind::folder) == (e->kind == entry_kind::folder);
    if (!same_kind) gone.insert(key);
  }
  // Bytes under each gone folder, so a folder goes to the bin as one item.
  std::unordered_map<std::string, std::uint64_t> folder_bytes;
  for (const auto& [key, e] : cpy) {
    if (e->kind != entry_kind::file || !gone.count(key)) continue;
    for (std::string_view up = parent_of(key); !up.empty(); up = parent_of(up)) {
      const std::string k(up);
      if (gone.count(k)) folder_bytes[k] += e->size;
    }
  }
  for (const auto& key : gone) {
    bool under_gone = false;
    for (std::string_view up = parent_of(key); !up.empty(); up = parent_of(up)) {
      if (gone.count(std::string(up))) {
        under_gone = true;
        break;
      }
    }
    if (under_gone) continue;  // its folder goes as one item
    const entry* e = cpy.at(key);
    const bool folder = e->kind == entry_kind::folder;
    action a{action_kind::to_bin, live(e->path), "", folder ? folder_bytes[key] : e->size, bin_reason::deleted, folder};
    deletions.push_back(std::move(a));
  }

  struct pending_copy {
    std::string top;
    action replaced;  // kind to_bin when the old one goes first
    bool has_replaced = false;
    action copy;
  };
  std::vector<pending_copy> copies;
  for (const auto& e : in.source) {
    const std::string key = fold(e.path, in.fold_case);
    const auto c = cpy.find(key);
    const bool matched = c != cpy.end() && !gone.count(key);
    if (matched && base_of(c->second->path) != base_of(e.path)) {
      renames.push_back({action_kind::rename_case, e.path,
                         std::string(parent_of(e.path)).append(parent_of(e.path).empty() ? "" : "/")
                             .append(base_of(c->second->path))});
    }
    if (e.kind == entry_kind::link) {
      ++out.links_skipped;
      links.push_back({action_kind::skip_link, e.path, ""});
      continue;
    }
    if (e.kind == entry_kind::folder) continue;  // the copier makes folders as it copies into them
    if (matched && c->second->size == e.size && c->second->mtime == e.mtime) continue;

    bool settling = in.now - e.mtime < in.settle_seconds;
    if (!settling && in.has_previous) {
      const auto p = prev.find(key);
      settling = p != prev.end() && (p->second->size != e.size || p->second->mtime != e.mtime);
    }
    if (settling) {
      ++out.held;
      holds.push_back({action_kind::hold, e.path, "", e.size});
      continue;
    }
    pending_copy pc;
    pc.top = fold(top_of(e.path), in.fold_case);
    if (matched) {
      pc.has_replaced = true;
      pc.replaced = {action_kind::to_bin, e.path, "", c->second->size, bin_reason::replaced, false};
    }
    pc.copy = {action_kind::copy, e.path, "", e.size};
    out.bytes_to_copy += e.size;
    ++out.files_to_copy;
    copies.push_back(std::move(pc));
  }

  // Renames parent first: a child's `from` is spelled with its parent's new name.
  std::stable_sort(renames.begin(), renames.end(),
                   [](const action& a, const action& b) { return depth_of(a.path) < depth_of(b.path); });
  std::sort(deletions.begin(), deletions.end(), [](const action& a, const action& b) { return a.path < b.path; });

  // Smallest top-level folders first, so most files are safe soonest.
  std::map<std::string, std::uint64_t> top_bytes;
  for (const auto& pc : copies) top_bytes[pc.top] += pc.copy.size;
  std::stable_sort(copies.begin(), copies.end(), [&](const pending_copy& a, const pending_copy& b) {
    const auto sa = top_bytes[a.top], sb = top_bytes[b.top];
    if (sa != sb) return sa < sb;
    if (a.top != b.top) return a.top < b.top;
    return a.copy.path < b.copy.path;
  });

  for (auto& a : renames) out.actions.push_back(std::move(a));
  for (auto& a : deletions) out.actions.push_back(std::move(a));
  for (auto& pc : copies) {
    if (pc.has_replaced) out.actions.push_back(std::move(pc.replaced));
    out.actions.push_back(std::move(pc.copy));
  }
  for (auto& a : holds) out.actions.push_back(std::move(a));
  for (auto& a : links) out.actions.push_back(std::move(a));
  out.fits = out.bytes_to_copy <= in.copy_free;
  return out;
}

// ---- the Recycle Bin ----------------------------------------------------------------------

namespace {

const char* reason_word(bin_reason r) { return r == bin_reason::replaced ? "replaced" : "deleted"; }

}  // namespace

bool safe_id(std::string_view id) noexcept {
  if (id.empty() || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
  });
}

bool safe_relative(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/') return false;
  if (path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) return false;
  if (path.find('\0') != std::string_view::npos) return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    auto slash = path.find('/', start);
    if (slash == std::string_view::npos) slash = path.size();
    const auto part = path.substr(start, slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = slash + 1;
  }
  return true;
}

std::string to_json(const bin& b) {
  json::writer w;
  w.begin_object().key("version").integer(1).key("items").begin_array();
  for (const auto& it : b.items) {
    w.begin_object()
        .key("id").string(it.id)
        .key("original").string(it.original)
        .key("bytes").integer(static_cast<std::int64_t>(it.bytes))
        .key("removed_at").integer(it.removed_at)
        .key("reason").string(reason_word(it.reason))
        .key("folder").boolean(it.folder)
        .end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<bin> bin_from_json(std::string_view text) {
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const json::value* items = doc->find("items");
  if (!items || items->k != json::kind::array) return std::nullopt;
  bin out;
  std::set<std::string> ids;
  for (const auto& v : items->a) {
    if (v.k != json::kind::object) return std::nullopt;
    const auto* id = v.str("id");
    const auto* original = v.str("original");
    const auto bytes = v.integer("bytes");
    const auto removed = v.integer("removed_at");
    const auto* reason = v.str("reason");
    if (!id || !original || !bytes || !removed || !reason || *bytes < 0) return std::nullopt;
    if (!safe_id(*id) || !safe_relative(*original) || !ids.insert(*id).second) return std::nullopt;
    bin_item it;
    it.id = *id;
    it.original = *original;
    it.bytes = static_cast<std::uint64_t>(*bytes);
    it.removed_at = *removed;
    if (*reason == "deleted") {
      it.reason = bin_reason::deleted;
    } else if (*reason == "replaced") {
      it.reason = bin_reason::replaced;
    } else {
      return std::nullopt;
    }
    if (const auto* f = v.find("folder")) {
      if (f->k != json::kind::boolean) return std::nullopt;
      it.folder = f->b;
    }
    out.items.push_back(std::move(it));
  }
  return out;
}

std::string next_id(const bin& b, std::int64_t now) {
  // UTC civil date from days since 1970 (Howard Hinnant's algorithm): no gmtime, no locale.
  std::int64_t days = now >= 0 ? now / 86400 : (now - 86399) / 86400;
  const std::int64_t secs = now - days * 86400;
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char stamp[128];  // room for any int64 in every field, so -Wformat-truncation has nothing to say
  std::snprintf(stamp, sizeof stamp, "%04lld%02lld%02lld-%02lld%02lld%02lld", static_cast<long long>(y),
                static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(secs / 3600),
                static_cast<long long>(secs / 60 % 60), static_cast<long long>(secs % 60));
  std::set<std::string> taken;
  for (const auto& it : b.items) taken.insert(it.id);
  for (int n = 1;; ++n) {
    std::string id = std::string(stamp) + "-" + std::to_string(n);
    if (!taken.count(id)) return id;
  }
}

namespace {

std::vector<const bin_item*> oldest_first(const bin& b) {
  std::vector<const bin_item*> v;
  for (const auto& it : b.items) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](const bin_item* a, const bin_item* c) {
    if (a->removed_at != c->removed_at) return a->removed_at < c->removed_at;
    return a->id < c->id;
  });
  return v;
}

}  // namespace

std::vector<std::string> expired(const bin& b, keep_for keep, std::int64_t now) {
  std::int64_t days = 0;
  switch (keep) {
    case keep_for::days7: days = 7; break;
    case keep_for::days30: days = 30; break;
    case keep_for::days90: days = 90; break;
    case keep_for::until_emptied: return {};
  }
  std::vector<std::string> out;
  for (const auto* it : oldest_first(b)) {
    if (now - it->removed_at >= days * 86400) out.push_back(it->id);
  }
  return out;
}

std::vector<std::string> trim_for_room(const bin& b, std::uint64_t drive_size, std::uint64_t free_now,
                                       std::uint64_t want_free, double share) {
  std::vector<std::string> out;
  if (free_now >= want_free) return out;
  std::uint64_t total = 0;
  for (const auto& it : b.items) total += it.bytes;
  const auto limit = static_cast<std::uint64_t>(static_cast<double>(drive_size) * share);
  std::uint64_t free_bytes = free_now;
  for (const auto* it : oldest_first(b)) {
    if (free_bytes >= want_free || total <= limit) break;
    out.push_back(it->id);
    free_bytes += it->bytes;
    total -= it->bytes;
  }
  return out;
}

std::string restore_path(std::string_view original, bool folder,
                         const std::function<bool(std::string_view)>& exists) {
  if (!exists(original)) return std::string(original);
  const std::string_view dir = parent_of(original);
  const std::string_view name = base_of(original);
  // A dot at the start (".profile") is part of the name, not an extension.
  const auto dot = folder ? std::string_view::npos : name.rfind('.');
  const bool has_ext = dot != std::string_view::npos && dot > 0;
  const std::string_view stem = has_ext ? name.substr(0, dot) : name;
  const std::string_view ext = has_ext ? name.substr(dot) : std::string_view{};
  for (int n = 1;; ++n) {
    std::string candidate(dir);
    if (!candidate.empty()) candidate.push_back('/');
    candidate += stem;
    candidate += n == 1 ? std::string(" (restored)") : " (restored " + std::to_string(n) + ")";
    candidate += ext;
    if (!exists(candidate)) return candidate;
  }
}

std::string bin_line(const bin& b, bool mac) {
  std::string out = mac ? "Trash: " : "Recycle Bin: ";
  if (b.items.empty()) return out + "empty";
  std::uint64_t total = 0;
  for (const auto& it : b.items) total += it.bytes;
  out += std::to_string(b.items.size());
  out += b.items.size() == 1 ? " item, " : " items, ";
  out += drives::size_words(total);
  return out;
}

}  // namespace mv::driftwire::cli::drive_copy
