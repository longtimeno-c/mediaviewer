// SPDX-License-Identifier: GPL-2.0-or-later
// Portable half of multi-folder browsing (plan/10 PR 26): natural ordering,
// housekeeping filter, folder summaries. The directory scan itself is the
// platform primitive scan_subdirs() (dir_win.cpp / dir_mac.cpp).
#include <algorithm>
#include <cstddef>
#include <utility>

#include "io/dir.h"

namespace mv::io {
namespace {

char fold(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (fold(a[i]) != fold(b[i])) return false;
  }
  return true;
}

// Walks `dir` for a first media file, preferring the folder's own, then each
// child in natural order. `visits` is shared across the whole walk.
bool find_cover(std::string_view dir, int depth_left, int& visits, dir_entry& out) {
  if (visits <= 0) return false;
  --visits;
  if (auto files = list_still_files(dir); files && !files.value().empty()) {
    out = std::move(files.value().front());
    return true;
  }
  if (depth_left <= 0) return false;
  auto subs = list_subfolders(dir);
  if (!subs) return false;
  for (const subdir_entry& sub : subs.value()) {
    if (find_cover(sub.path_utf8, depth_left - 1, visits, out)) return true;
    if (visits <= 0) break;
  }
  return false;
}

}  // namespace

bool less_natural(std::string_view a, std::string_view b) noexcept {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (is_digit(a[i]) && is_digit(b[j])) {
      std::size_t ie = i, je = j;
      while (ie < a.size() && is_digit(a[ie])) ++ie;
      while (je < b.size() && is_digit(b[je])) ++je;
      // Compare by value: strip leading zeros, then length, then digits.
      std::size_t is = i, js = j;
      while (is + 1 < ie && a[is] == '0') ++is;
      while (js + 1 < je && b[js] == '0') ++js;
      const std::size_t la = ie - is, lb = je - js;
      if (la != lb) return la < lb;
      for (std::size_t k = 0; k < la; ++k) {
        if (a[is + k] != b[js + k]) return a[is + k] < b[js + k];
      }
      // Equal value: fewer leading zeros first, so the order stays total.
      if ((is - i) != (js - j)) return (is - i) < (js - j);
      i = ie;
      j = je;
      continue;
    }
    const char ca = fold(a[i]), cb = fold(b[j]);
    if (ca != cb) return ca < cb;
    ++i;
    ++j;
  }
  if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
  // Case-only differences: keep the order total and stable.
  return a < b;
}

bool is_housekeeping_dir(std::string_view name) noexcept {
  if (name.empty()) return true;
  if (name.front() == '.' || name.front() == '@' || name.front() == '#') return true;
  return iequals(name, "$RECYCLE.BIN") || iequals(name, "System Volume Information") ||
         iequals(name, "lost+found") || iequals(name, "Thumbs") || iequals(name, "__MACOSX");
}

result<std::vector<subdir_entry>> list_subfolders(std::string_view utf8_dir) {
  auto scanned = scan_subdirs(utf8_dir);
  if (!scanned) return scanned;
  std::vector<subdir_entry> out = std::move(scanned).value();
  std::erase_if(out, [](const subdir_entry& e) { return is_housekeeping_dir(e.name_utf8); });
  std::sort(out.begin(), out.end(), [](const subdir_entry& a, const subdir_entry& b) {
    return less_natural(a.name_utf8, b.name_utf8);
  });
  return out;
}

result<folder_summary> summarize_dir(std::string_view utf8_dir, int max_depth, int max_visits) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  auto files = list_still_files(utf8_dir);
  if (!files) return err(files.error());
  auto subs = list_subfolders(utf8_dir);
  if (!subs) return err(subs.error());

  folder_summary out;
  out.media_count = static_cast<std::uint32_t>(files.value().size());
  out.subdir_count = static_cast<std::uint32_t>(subs.value().size());
  if (!files.value().empty()) {
    out.has_cover = true;
    out.cover = std::move(files.value().front());
    return out;
  }
  int visits = max_visits;
  for (const subdir_entry& sub : subs.value()) {
    if (find_cover(sub.path_utf8, max_depth - 1, visits, out.cover)) {
      out.has_cover = true;
      break;
    }
    if (visits <= 0) break;
  }
  return out;
}

}  // namespace mv::io
