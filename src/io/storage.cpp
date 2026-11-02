// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/storage.h"

#include <cstdio>
#include <utility>

#include "io/file_port.h"
#include "io/paths.h"

namespace mv::io {
namespace {

void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

storage_row measured(std::string id, std::string path) {
  const tree_usage u = tree_size(path);
  return storage_row{std::move(id), std::move(path), u.bytes, u.files};
}

// One row per child folder of `root` (an add-on store), dot folders (a
// download being staged) left out.
void add_children(std::vector<storage_row>& rows, std::string_view prefix, const std::string& root) {
  auto children = list_children(root, true);
  if (!children) return;
  for (const dir_child& c : children.value()) {
    if (!c.is_directory || c.name.empty() || c.name.front() == '.') continue;
    std::string id(prefix);
    id += c.name;
    rows.push_back(measured(std::move(id), join_path(root, c.name)));
  }
}

}  // namespace

bool is_search_cache_file(std::string_view name) noexcept {
  return name.substr(0, 13) == "meta_cache.db";
}

std::vector<storage_row> measure_storage(const std::vector<storage_place>& extra) {
  std::vector<storage_row> rows;
  if (auto thumbs = thumb_cache_dir()) {
    // One listing, split: Clear empties the thumbnails and leaves the search
    // cache, so the page shows them apart.
    storage_row pictures{"thumbnails", thumbs.value(), 0, 0};
    storage_row search{"search_cache", thumbs.value(), 0, 0};
    if (auto listed = list_children(thumbs.value())) {
      for (const dir_child& f : listed.value()) {
        storage_row& row = is_search_cache_file(f.name) ? search : pictures;
        row.bytes += f.size;
        ++row.files;
      }
    }
    rows.push_back(std::move(pictures));
    rows.push_back(std::move(search));
  }
  if (auto dir = clipboard_dir()) rows.push_back(measured("clipboard", dir.value()));
  if (auto dir = metadata_snapshot_dir()) rows.push_back(measured("metadata_snapshots", dir.value()));
  for (const storage_place& p : extra) rows.push_back(measured(p.id, p.path));
  if (auto dir = addons_dir()) add_children(rows, "addon:", dir.value());
  if (auto dir = open_addons_dir()) add_children(rows, "open_addon:", dir.value());
  if (auto dir = addon_settings_dir()) rows.push_back(measured("addon_settings", dir.value()));
  return rows;
}

std::string storage_json(const std::vector<storage_row>& rows) {
  std::string out = "{\"rows\":[";
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const storage_row& r = rows[i];
    if (i != 0) out.push_back(',');
    out += "{\"id\":";
    append_json_string(out, r.id);
    out += ",\"path\":";
    append_json_string(out, r.path);
    out += ",\"bytes\":";
    out += std::to_string(r.bytes);
    out += ",\"files\":";
    out += std::to_string(r.files);
    out.push_back('}');
    total += r.bytes;
  }
  out += "],\"total\":";
  out += std::to_string(total);
  out.push_back('}');
  return out;
}

}  // namespace mv::io
