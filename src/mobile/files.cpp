// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/files.h"

#include "core/fold.h"
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

kind kind_of(std::string_view name) noexcept {
  const auto* row = io::find_media_extension(io::extension_of(name));
  if (!row) return kind::none;
  switch (row->kind) {
    case io::media_kind::still: return kind::photo;
    case io::media_kind::raw: return kind::raw;
    case io::media_kind::video: return kind::clip;
    default: return kind::none;  // audio and documents: not something this phone shows
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

}  // namespace mv::mobile::files
