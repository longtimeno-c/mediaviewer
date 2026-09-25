// SPDX-License-Identifier: GPL-2.0-or-later
#include "addons/import/scanner.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

#include "addons/import/naming.h"

namespace mv::import {
namespace {

// Cards nest DCIM/100CANON/…; a network share might go deeper. Bounded so a
// share that loops through a junction cannot walk forever.
constexpr int kMaxDepth = 12;

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string parent_rel(std::string_view rel) {
  const auto slash = rel.find_last_of('/');
  return slash == std::string_view::npos ? std::string() : std::string(rel.substr(0, slash));
}

bool is_sep(char c) { return c == '/' || c == '\\'; }

// `path` relative to `root`, '/'-separated; the file name if not under it.
std::string relative_to(const std::string& root, const std::string& path) {
  std::size_t n = root.size();
  while (n > 0 && is_sep(root[n - 1])) --n;
  if (path.size() > n && lower_ascii(path.substr(0, n)) == lower_ascii(root.substr(0, n)) &&
      is_sep(path[n])) {
    std::string rel = path.substr(n + 1);
    for (char& c : rel) {
      if (c == '\\') c = '/';
    }
    return rel;
  }
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void fill_volume(const host& h, const std::string& path, scan_result& s) {
  if (auto v = h.volume_of(path)) {
    s.volume_id = v->volume_id;
    s.label = v->label_utf8;
    s.device_key = v->device_key;
    s.network = v->network != 0;
    s.removable = v->removable != 0;
    if (s.root.empty()) s.root = v->root_utf8;
  }
}

std::string volume_root(const host& h, const std::string& path) {
  if (auto v = h.volume_of(path)) return v->root_utf8;
  return {};
}

// Dates, cameras, sizes and the card memory, once the units exist.
void finish_units(const host& h, library_index& idx, scan_result& s,
                  const std::atomic<bool>& cancel) {
  for (source_file& f : s.files) {
    if (s.volume_id.empty()) break;
    if (auto row = idx.card_lookup(s.volume_id, f.vol_rel, f.size, f.mtime)) {
      f.have_hash = true;
      f.hash = row->hash;
    }
  }
  for (unit& u : s.units) {
    if (cancel.load(std::memory_order_relaxed)) return;
    const source_file& primary = s.files[u.files.front()];
    mv_addon_capture cap{};
    if (h.capture(primary.path, cap) && cap.has_date) {
      u.taken = cap.taken_unix;
      u.has_date = true;
    }
    u.camera = cap.camera_utf8;
    std::int64_t earliest = INT64_MAX;
    bool all_imported = !s.volume_id.empty();
    for (std::uint32_t i : u.files) {
      const source_file& f = s.files[i];
      u.bytes += f.size;
      earliest = std::min(earliest, f.mtime);
      if (f.type == file_type::raw) u.has_raw = true;
      if (all_imported) {
        const auto row = idx.card_lookup(s.volume_id, f.vol_rel, f.size, f.mtime);
        all_imported = row && row->imported_at > 0;
      }
    }
    u.imported_before = all_imported;
    // No capture time: the file time, as local wall clock, labelled by has_date.
    u.file_time = earliest == INT64_MAX ? 0 : local_wall_from_utc(earliest);
    if (!u.has_date) u.taken = u.file_time;
  }
  // The grid and the copy both go in capture order, then name.
  std::stable_sort(s.units.begin(), s.units.end(), [&s](const unit& a, const unit& b) {
    if (a.taken != b.taken) return a.taken < b.taken;
    return s.files[a.files.front()].rel < s.files[b.files.front()].rel;
  });
}

}  // namespace

void group_units(const host& h, std::vector<source_file>& all,
                 const std::vector<std::uint32_t>& folder_files, std::vector<unit>& out) {
  std::vector<std::uint32_t> media;
  std::vector<std::uint32_t> sidecars;
  for (std::uint32_t i : folder_files) {
    if (all[i].type == file_type::sidecar) {
      sidecars.push_back(i);
    } else if (all[i].type != file_type::none) {
      media.push_back(i);
    }
  }
  std::vector<std::string> names;
  names.reserve(media.size());
  for (std::uint32_t i : media) names.push_back(all[i].name);
  std::vector<std::uint32_t> partner;
  std::vector<std::uint32_t> kind;
  if (!h.pair(names, partner, kind)) {
    partner.assign(names.size(), UINT32_MAX);
    kind.assign(names.size(), 0);
  }

  const std::size_t first_unit = out.size();
  std::vector<char> used(media.size(), 0);
  for (std::size_t k = 0; k < media.size(); ++k) {
    if (used[k]) continue;
    used[k] = 1;
    unit u;
    const std::uint32_t p = partner[k];
    if (p != UINT32_MAX && p < media.size() && !used[p] && partner[p] == k) {
      used[p] = 1;
      u.kind = kind[k] == 1 ? unit_kind::raw_jpeg : unit_kind::live_photo;
      // The still is primary: JPEG/HEIC over RAW, the still over the motion.
      const file_type tk = all[media[k]].type;
      const bool k_primary = tk != file_type::raw && tk != file_type::video;
      u.files = k_primary ? std::vector<std::uint32_t>{media[k], media[p]}
                          : std::vector<std::uint32_t>{media[p], media[k]};
    } else {
      u.files = {media[k]};
    }
    out.push_back(std::move(u));
  }

  // Sidecars ride with the unit whose stem they carry: IMG_0001.xmp and
  // IMG_0001.CR3.xmp, Canon's MVI_0001.THM, GoPro's GL010001.LRV beside
  // GX010001.MP4 is a different stem and stays out; Sony's C0001M01.XML
  // beside C0001.MP4 is matched by its "M01" suffix.
  std::map<std::string, std::size_t> by_stem;
  for (std::size_t u = first_unit; u < out.size(); ++u) {
    for (std::uint32_t i : out[u].files) by_stem.emplace(lower_ascii(stem_of(all[i].name)), u);
  }
  for (std::uint32_t i : sidecars) {
    std::string stem = lower_ascii(stem_of(all[i].name));
    auto it = by_stem.find(stem);
    if (it == by_stem.end() && stem.size() > 3 && stem.compare(stem.size() - 3, 3, "m01") == 0) {
      it = by_stem.find(stem.substr(0, stem.size() - 3));
    }
    if (it != by_stem.end()) out[it->second].files.push_back(i);
    // A sidecar with no file of its own is not imported: never alone.
  }
}

result<scan_result> scan_folder(const host& h, library_index& idx, const std::string& root,
                                const std::atomic<bool>& cancel) {
  scan_result s;
  s.root = root;
  fill_volume(h, root, s);
  const std::string vroot = volume_root(h, root);

  std::map<std::string, std::vector<std::uint32_t>> folders;
  const auto walked = h.walk(root, kMaxDepth, [&](const mv_addon_file_entry& e) {
    if (cancel.load(std::memory_order_relaxed)) return false;
    const file_type t = classify(e.name_utf8);
    if (t == file_type::none) return true;
    source_file f;
    f.path = e.path_utf8;
    f.rel = e.relative_utf8;
    f.vol_rel = vroot.empty() ? f.rel : relative_to(vroot, f.path);
    f.name = e.name_utf8;
    f.size = e.size;
    f.mtime = e.mtime_unix;
    f.type = t;
    folders[parent_rel(f.rel)].push_back(static_cast<std::uint32_t>(s.files.size()));
    s.files.push_back(std::move(f));
    return true;
  });
  if (!walked) return err(walked.error());
  for (const auto& [dir, files] : folders) group_units(h, s.files, files, s.units);
  finish_units(h, idx, s, cancel);
  if (cancel.load(std::memory_order_relaxed)) return err(status::cancelled);
  return s;
}

result<scan_result> scan_files(const host& h, library_index& idx,
                               const std::vector<std::string>& paths,
                               const std::atomic<bool>& cancel) {
  scan_result s;
  s.explicit_files = true;
  if (paths.empty()) return err(status::invalid_arg);
  fill_volume(h, paths.front(), s);
  const std::string vroot = volume_root(h, paths.front());

  std::set<std::string> wanted;
  std::map<std::string, std::string> dirs;  // folder path -> its rel from the volume
  for (const std::string& p : paths) {
    wanted.insert(p);
    const auto slash = p.find_last_of("/\\");
    if (slash == std::string::npos) continue;
    const std::string dir = p.substr(0, slash == 0 ? 1 : slash);
    dirs.emplace(dir, vroot.empty() ? std::string() : parent_rel(relative_to(vroot, p)));
  }
  for (const auto& [dir, dir_rel] : dirs) {
    std::vector<std::uint32_t> folder;
    const auto walked = h.walk(dir, 0, [&](const mv_addon_file_entry& e) {
      const file_type t = classify(e.name_utf8);
      if (t == file_type::none) return true;
      source_file f;
      f.path = e.path_utf8;
      f.rel = e.relative_utf8;
      f.vol_rel = dir_rel.empty() ? f.rel : dir_rel + "/" + f.rel;
      f.name = e.name_utf8;
      f.size = e.size;
      f.mtime = e.mtime_unix;
      f.type = t;
      folder.push_back(static_cast<std::uint32_t>(s.files.size()));
      s.files.push_back(std::move(f));
      return !cancel.load(std::memory_order_relaxed);
    });
    if (!walked) return err(walked.error());
    std::vector<unit> units;
    group_units(h, s.files, folder, units);
    for (unit& u : units) {
      const bool keep = std::any_of(u.files.begin(), u.files.end(), [&](std::uint32_t i) {
        return wanted.count(s.files[i].path) != 0;
      });
      if (keep) s.units.push_back(std::move(u));
    }
  }
  finish_units(h, idx, s, cancel);
  if (cancel.load(std::memory_order_relaxed)) return err(status::cancelled);
  return s;
}

}  // namespace mv::import
