// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/files.h"

#include "core/fold.h"
#include <algorithm>

#include "io/media_extensions.h"
#include "meta/meta.h"
#include "meta/search.h"

namespace mv::mobile::files {

namespace {

bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && space(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

}  // namespace

namespace {

bool equal_nocase(std::string_view a, std::string_view b) { return a.size() == b.size() && starts_with_nocase(a, b); }

// Files a camera or an editor leaves beside the pictures: never something to open on a phone.
bool companion(std::string_view name) noexcept {
  for (std::string_view whole : {"Thumbs.db", "desktop.ini", "Icon\r"}) {
    if (equal_nocase(name, whole)) return true;
  }
  const auto ext = io::extension_of(name);
  // Editors' sidecars; GoPro and DJI previews and telemetry (THM, LRV, LRF, SRT); Canon's CTG; the
  // AVCHD and Sony card indexes (CPI, BDM, MPL, BIM).
  for (std::string_view e : {".xmp", ".thm", ".lrv", ".aae", ".pp3", ".dop", ".on1", ".lrf", ".srt", ".ctg", ".cpi",
                             ".bdm", ".mpl", ".bim"}) {
    if (equal_nocase(ext, e)) return true;
  }
  // Sony XAVC's metadata beside each clip: C0001M01.XML.
  if (equal_nocase(ext, ".xml") && name.size() >= 7 && equal_nocase(name.substr(name.size() - 7, 3), "M01")) return true;
  return false;
}

}  // namespace

kind kind_of(std::string_view name) noexcept {
  if (companion(name)) return kind::none;
  const auto* row = io::find_media_extension(io::extension_of(name));
  if (!row) return kind::other;
  switch (row->kind) {
    case io::media_kind::still: return kind::photo;
    case io::media_kind::raw: return kind::raw;
    case io::media_kind::video: return kind::clip;
    default: return kind::other;  // audio and documents: listed, opened by the phone's own preview
  }
}

std::vector<entry> shown(std::span<const driftwire::serve::list_entry> listing) {
  std::vector<entry> out;
  out.reserve(listing.size());
  for (const auto& e : listing) {
    if (e.name.empty() || e.name.front() == '.') continue;  // `.mediaviewer`, and what walkers skip
    entry x{e.name, e.size, e.mtime, e.dir, kind::none};
    if (!e.dir) {
      x.k = kind_of(e.name);
      if (x.k == kind::none) continue;
    }
    out.push_back(std::move(x));
  }
  return out;
}

std::string camera_line(std::string_view make_in, std::string_view model_in) {
  const std::string_view make = trim(make_in), model = trim(model_in);
  if (make.empty() || starts_with_nocase(model, make)) return std::string(model);
  if (model.empty()) return std::string(make);
  return std::string(make) + " " + std::string(model);
}

std::vector<std::string> fields_of(std::string_view make, std::string_view model, std::string_view lens,
                                   std::string_view comment, std::span<const tag> tags) {
  meta::metadata m;
  m.s.camera = camera_line(make, model);
  m.s.lens = std::string(trim(lens));
  m.s.comment = std::string(trim(comment));
  m.properties.reserve(tags.size());
  for (const tag& t : tags) {
    meta::property p;
    p.raw_tag = t.raw_tag;
    p.raw = t.value;
    p.value = t.value;
    m.properties.push_back(std::move(p));
  }
  return meta::search_facts_of(m).fields;
}

void index::set_names(std::vector<std::string> names) {
  std::vector<std::string> folded;
  folded.reserve(names.size());
  for (const auto& n : names) folded.push_back(text::fold(n));
  std::lock_guard lock(m_);
  folded_names_ = std::move(folded);
  fields_.assign(folded_names_.size(), {});
  folded_fields_.assign(folded_names_.size(), {});
  has_fields_.assign(folded_names_.size(), false);
  tagged_ = 0;
}

void index::set_fields(std::size_t i, std::span<const std::string> fields) {
  std::vector<std::string> copy(fields.begin(), fields.end());
  std::string folded = meta::folded_text(copy);
  std::lock_guard lock(m_);
  if (i >= fields_.size()) return;
  if (!has_fields_[i]) ++tagged_;
  has_fields_[i] = true;
  fields_[i] = std::move(copy);
  folded_fields_[i] = std::move(folded);
}

std::vector<index::hit> index::match(std::string_view typed) const {
  std::vector<hit> out;
  const std::string needle = text::fold(trim(typed));
  if (needle.empty()) return out;
  const std::vector<std::string> words = meta::search_words(typed);
  std::size_t letters = 0;
  for (const auto& w : words) letters += w.size();
  std::lock_guard lock(m_);
  std::vector<bool> by_name(folded_names_.size(), false);
  for (std::size_t i = 0; i < folded_names_.size(); ++i) {
    if (folded_names_[i].find(needle) == std::string::npos) continue;
    by_name[i] = true;
    out.push_back({i, {}});
  }
  if (letters < 2) return out;  // one folded letter would light up every camera make
  for (std::size_t i = 0; i < folded_fields_.size(); ++i) {
    if (by_name[i] || folded_fields_[i].empty()) continue;
    const int field = meta::match_fields(folded_fields_[i], words);
    if (field < 0) continue;
    out.push_back({i, meta::why_of(fields_[i], field)});
  }
  return out;
}

std::size_t index::count() const {
  std::lock_guard lock(m_);
  return folded_names_.size();
}

std::size_t index::tagged() const {
  std::lock_guard lock(m_);
  return tagged_;
}

std::vector<place> places_for(std::string_view share, std::string_view sub, std::span<const served_root> roots) {
  std::vector<std::string> parts;
  for (std::size_t at = 0; at <= sub.size();) {
    const std::size_t slash = std::min(sub.find('/', at), sub.size());
    if (slash > at) parts.emplace_back(sub.substr(at, slash - at));
    at = slash + 1;
  }
  const auto join_from = [&](std::size_t first) {
    std::string out;
    for (std::size_t i = first; i < parts.size(); ++i) out += (out.empty() ? "" : "/") + parts[i];
    return out;
  };
  std::vector<place> out;
  const auto add = [&](const std::string& root, std::string prefix) {
    place p{root, std::move(prefix)};
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(std::move(p));
  };
  // The share's own name, then a folder of the share's tree by its name: the label says where it is.
  for (const auto& r : roots) {
    if (r.label == share) add(r.id, join_from(0));
  }
  for (const auto& r : roots) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (parts[i] == r.label) add(r.id, join_from(i + 1));
    }
  }
  // Then any folder as the share itself, as a folder deeper in it, or as what holds the share.
  for (const auto& r : roots) {
    for (std::size_t i = 0; i <= parts.size(); ++i) add(r.id, join_from(i));
    add(r.id, parts.empty() ? std::string(share) : std::string(share) + "/" + join_from(0));
  }
  return out;
}

}  // namespace mv::mobile::files
