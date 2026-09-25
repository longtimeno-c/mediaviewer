// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/planner.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "addons/import/naming.h"
#include "addons/import/paths.h"
#include "core/json.h"

namespace mv::import {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string rel_join(const std::string& folder, const std::string& name) {
  return folder.empty() ? name : folder + "/" + name;
}

copy_callbacks cancel_only(const std::atomic<bool>& cancel) {
  copy_callbacks cb;
  cb.cancelled = [&cancel] { return cancel.load(std::memory_order_relaxed); };
  return cb;
}

// Hashes a source file the card memory did not know, and teaches it.
bool ensure_hash(const host& h, library_index& idx, const scan_result& scan, source_file& f,
                 const std::atomic<bool>& cancel) {
  if (f.have_hash) return true;
  auto d = h.hash(f.path, false, cancel_only(cancel));
  if (!d) return false;
  f.hash = *d;
  f.have_hash = true;
  if (!scan.volume_id.empty()) idx.card_record(scan.volume_id, f.vol_rel, f.size, f.mtime, f.hash, 0);
  return true;
}

// The hash of a file already at a destination: the index's when its size and
// mtime still match, else read once and indexed. A missing file drops its row.
std::optional<digest> destination_hash(const host& h, library_index& idx, const std::string& root,
                                       const std::string& rel, const std::atomic<bool>& cancel) {
  const std::string full = join_native(root, rel);
  auto st = h.stat(full);
  if (!st || st->is_directory) {
    idx.drop(root, rel);
    return std::nullopt;
  }
  if (auto row = idx.by_path(root, rel); row && row->size == st->size && row->mtime == st->mtime) {
    return row->hash;
  }
  auto d = h.hash(full, false, cancel_only(cancel));
  if (!d) return std::nullopt;
  idx.upsert(library_row{root, rel, st->size, st->mtime, *d});
  return *d;
}

struct dup_finder {
  const host& h;
  library_index& idx;
  scan_result& scan;
  const plan_result& plan;
  const std::atomic<bool>& cancel;
  // Sizes that occur more than once on this source: only those need hashing
  // to catch a duplicate inside the card itself.
  std::unordered_map<std::uint64_t, int> size_count;
  // Content already planned from this source: hash -> display.
  std::map<digest, std::string> seen;

  // What `f` duplicates, or empty.
  std::string match(source_file& f) {
    const std::string* scope_root =
        plan.settings.scope == dup_scope::destination ? &plan.destination : nullptr;
    std::vector<library_row> candidates = idx.by_size(f.size, scope_root);
    const bool twin_on_card = size_count[f.size] > 1;
    if (candidates.empty() && !twin_on_card) return {};
    if (!ensure_hash(h, idx, scan, f, cancel)) return {};
    for (const library_row& c : candidates) {
      if (c.hash != f.hash) continue;
      // The backup drive is indexed too, but a copy there is not the library:
      // the main destination still needs the file.
      if (!plan.backup.empty() && c.root == plan.backup) continue;
      // The index says so; the file must still be there (a stat, not a read).
      auto now = destination_hash(h, idx, c.root, c.rel, cancel);
      if (now && *now == f.hash) {
        return c.root == plan.destination ? c.rel : join_native(c.root, c.rel);
      }
    }
    if (auto it = seen.find(f.hash); it != seen.end()) return it->second;
    return {};
  }

  // Whether `root` already holds `f`'s content (the index, confirmed on disk).
  bool present_on(source_file& f, const std::string& root) {
    std::vector<library_row> candidates = idx.by_size(f.size, &root);
    if (candidates.empty() || !ensure_hash(h, idx, scan, f, cancel)) return false;
    for (const library_row& c : candidates) {
      if (c.hash != f.hash) continue;
      auto now = destination_hash(h, idx, c.root, c.rel, cancel);
      if (now && *now == f.hash) return true;
    }
    return false;
  }

  void remember(source_file& f) {
    if (size_count[f.size] > 1 && ensure_hash(h, idx, scan, f, cancel)) {
      seen.emplace(f.hash, "this source: " + f.rel);
    }
  }
};

bool in_range(const preset& p, const std::string& day) {
  std::int64_t d = 0;
  std::int64_t from = 0;
  std::int64_t to = 0;
  if (!parse_day(day, d)) return false;
  if (!p.range_from.empty() && parse_day(p.range_from, from) && d < from) return false;
  if (!p.range_to.empty() && parse_day(p.range_to, to) && d > to) return false;
  return true;
}

bool passes_types(const preset& p, const scan_result& scan, const unit& u) {
  for (std::uint32_t i : u.files) {
    if (type_bit(scan.files[i].type) & p.types) return true;
  }
  return false;
}

const char* state_name(unit_state s) {
  switch (s) {
    case unit_state::duplicate: return "duplicate";
    case unit_state::imported: return "imported";
    case unit_state::filtered: return "filtered";
    default: return "new";
  }
}

const char* kind_name(unit_kind k) {
  switch (k) {
    case unit_kind::raw_jpeg: return "raw_jpeg";
    case unit_kind::live_photo: return "live_photo";
    default: return "single";
  }
}

const char* type_name(file_type t) {
  switch (t) {
    case file_type::raw: return "raw";
    case file_type::jpeg: return "jpeg";
    case file_type::heic: return "heic";
    case file_type::video: return "video";
    case file_type::sidecar: return "sidecar";
    default: return "other";
  }
}

}  // namespace

std::string normalize_root(std::string root) {
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) {
    if (root.size() == 3 && root[1] == ':') break;  // "D:\"
    root.pop_back();
  }
  return root;
}

bool apply_selection(plan_result& plan, int unit, const std::string* day, bool selected) {
  bool changed = false;
  for (std::size_t i = 0; i < plan.units.size(); ++i) {
    plan_unit& pu = plan.units[i];
    if (unit >= 0 && static_cast<std::size_t>(unit) != i) continue;
    if (unit < 0 && day && pu.day != *day) continue;
    const bool want = selected && pu.state != unit_state::filtered &&
                      !(pu.state == unit_state::duplicate && plan.settings.skip_duplicates);
    if (pu.selected != want) {
      pu.selected = want;
      changed = true;
    }
    // A skipped duplicate the backup lacks follows the selection too.
    const bool backup = selected && pu.backup_needed && !pu.selected;
    if (pu.backup_only != backup) {
      pu.backup_only = backup;
      changed = true;
    }
  }
  return changed;
}

void assign_names(const host& h, library_index& idx, scan_result& scan, plan_result& plan,
                  const std::atomic<bool>& cancel) {
  const preset& p = plan.settings;
  const bool uses_seq = !p.rename.empty() && template_uses_seq(p.rename);
  std::map<std::string, std::uint32_t> next_seq;
  std::set<std::string> claimed;  // lower-cased relative targets
  std::vector<std::string> roots{plan.destination};
  if (!plan.backup.empty()) roots.push_back(plan.backup);

  for (plan_unit& pu : plan.units) {
    if (cancel.load(std::memory_order_relaxed)) return;
    const unit& u = scan.units[pu.unit];
    source_file& primary = scan.files[u.files.front()];
    const std::string old_stem(stem_of(primary.name));

    layout_input li;
    li.taken = u.taken;
    li.camera = u.camera;
    li.has_raw = u.has_raw;
    li.primary_type = primary.type;
    const auto slash = primary.rel.find_last_of('/');
    li.source_rel_dir = slash == std::string::npos ? std::string_view()
                                                   : std::string_view(primary.rel).substr(0, slash);
    pu.folder = layout_folder(p, li);

    // Units that will be copied somewhere: every destination, or the backup
    // alone for a duplicate the backup lacks.
    const bool work = pu.selected || pu.backup_only;
    const std::vector<std::string> backup_root{plan.backup};
    const std::vector<std::string>& targets = pu.selected ? roots : backup_root;

    pu.seq = 0;
    std::string base = old_stem;
    if (!p.rename.empty()) {
      if (uses_seq && work) {
        auto [it, inserted] = next_seq.try_emplace(pu.day, 0u);
        if (inserted) it->second = idx.last_seq(plan.destination, pu.day);
        pu.seq = ++it->second;
      }
      base = render_stem(p.rename, rename_input{u.taken, u.camera, pu.seq, old_stem});
    }

    pu.renamed_for_clash = false;
    for (int n = 1; n < 10000; ++n) {
      const std::string stem = n == 1 ? base : clash_stem(base, n);
      std::vector<std::string> names;
      names.reserve(u.files.size());
      for (std::uint32_t i : u.files) names.push_back(with_stem(scan.files[i].name, old_stem, stem));

      bool clash = false;
      for (const std::string& name : names) {
        clash = clash || claimed.count(lower_ascii(rel_join(pu.folder, name))) != 0;
      }
      // Only a unit that will be copied looks at the disk; the rest are shown
      // where they would go.
      bool settled = false;  // decided without a copy: a duplicate, or already backed up
      if (!clash && work) {
        for (std::size_t m = 0; m < names.size() && !clash; ++m) {
          const std::string rel = rel_join(pu.folder, names[m]);
          for (const std::string& root : targets) {
            if (!h.stat(join_native(root, rel))) continue;
            // Something is there already. Same bytes: it is this file, a
            // duplicate the index had not seen. Different bytes: a clash.
            source_file& f = scan.files[u.files[m]];
            auto there = destination_hash(h, idx, root, rel, cancel);
            const bool same = there && f.type != file_type::sidecar &&
                              ensure_hash(h, idx, scan, f, cancel) && *there == f.hash &&
                              p.skip_duplicates && m == 0;
            if (same && root == plan.destination) {
              pu.state = unit_state::duplicate;
              pu.matched = rel;
              pu.selected = false;
              // The backup still gets it, unless something is already at
              // that name there (then it is shown as the duplicate it is).
              pu.backup_needed = !plan.backup.empty() &&
                                 !h.stat(join_native(plan.backup, rel)).has_value();
              pu.backup_only = pu.backup_needed;
              settled = true;
            } else if (same && root == plan.backup && !pu.selected) {
              pu.backup_needed = false;  // the backup has it after all
              pu.backup_only = false;
              settled = true;
            }
            clash = true;
            break;
          }
        }
        if (settled) {
          pu.names = std::move(names);
          if (pu.backup_only) {
            for (const std::string& name : pu.names) claimed.insert(lower_ascii(rel_join(pu.folder, name)));
          }
          break;
        }
      }
      if (!clash) {
        for (const std::string& name : names) claimed.insert(lower_ascii(rel_join(pu.folder, name)));
        pu.names = std::move(names);
        pu.renamed_for_clash = n > 1;
        break;
      }
    }
  }
}

result<plan_result> make_plan(const host& h, library_index& idx, scan_result& scan,
                              const preset& settings, const std::set<std::string>& marked,
                              const std::string& default_library,
                              const std::atomic<bool>& cancel) {
  plan_result plan;
  plan.scan_id = scan.id;
  plan.settings = settings;
  plan.destination = normalize_root(settings.destination.empty() ? default_library
                                                                 : settings.destination);
  plan.backup = settings.backup.empty() ? std::string() : normalize_root(settings.backup);
  if (plan.destination.empty()) return err(status::invalid_arg);
  if (!plan.backup.empty() && plan.backup == plan.destination) plan.backup.clear();

  dup_finder dups{h, idx, scan, plan, cancel, {}, {}};
  for (const source_file& f : scan.files) {
    if (f.type != file_type::sidecar) ++dups.size_count[f.size];
  }

  plan.units.reserve(scan.units.size());
  for (std::uint32_t i = 0; i < scan.units.size(); ++i) {
    if (cancel.load(std::memory_order_relaxed)) return err(status::cancelled);
    const unit& u = scan.units[i];
    plan_unit pu;
    pu.unit = i;
    pu.day = day_key(settings.dates == date_source::file_time ? u.file_time : u.taken);

    const bool filtered = !passes_types(settings, scan, u) ||
                          (settings.selection == selection_mode::date_range &&
                           !in_range(settings, pu.day));
    if (filtered) {
      pu.state = unit_state::filtered;
    } else {
      // A duplicate only if every file in the unit (sidecars aside) is.
      std::string first_match;
      bool all_dup = true;
      for (std::uint32_t fi : u.files) {
        source_file& f = scan.files[fi];
        if (f.type == file_type::sidecar) continue;
        std::string m = dups.match(f);
        if (m.empty()) {
          all_dup = false;
          break;
        }
        if (first_match.empty()) first_match = std::move(m);
      }
      if (all_dup && !first_match.empty()) {
        pu.state = unit_state::duplicate;
        pu.matched = std::move(first_match);
        // Skipped on the main destination; the backup is checked on its own.
        if (!plan.backup.empty() && settings.skip_duplicates) {
          for (std::uint32_t fi : u.files) {
            source_file& f = scan.files[fi];
            if (f.type != file_type::sidecar && !dups.present_on(f, plan.backup)) {
              pu.backup_needed = true;
              break;
            }
          }
        }
      } else {
        pu.state = u.imported_before ? unit_state::imported : unit_state::fresh;
        for (std::uint32_t fi : u.files) {
          if (scan.files[fi].type != file_type::sidecar) dups.remember(scan.files[fi]);
        }
      }
    }

    const bool importable = pu.state == unit_state::fresh || pu.state == unit_state::imported ||
                            (pu.state == unit_state::duplicate && !settings.skip_duplicates);
    switch (settings.selection) {
      case selection_mode::new_only:
        pu.selected = pu.state == unit_state::fresh;
        break;
      case selection_mode::all:
      case selection_mode::date_range:
        pu.selected = importable;
        break;
      case selection_mode::marked:
        pu.selected = importable && std::any_of(u.files.begin(), u.files.end(), [&](std::uint32_t fi) {
                        return marked.count(scan.files[fi].path) != 0;
                      });
        break;
    }
    // An explicit list (the viewer's marks, Ctrl+Shift+F7) means "these".
    if (scan.explicit_files && importable) pu.selected = true;
    if (pu.backup_needed) {
      // Selected for the backup exactly when it would have been selected had
      // it not been a duplicate on the main destination.
      const bool as_new = !u.imported_before;
      switch (settings.selection) {
        case selection_mode::new_only: pu.backup_only = as_new; break;
        case selection_mode::all:
        case selection_mode::date_range: pu.backup_only = true; break;
        case selection_mode::marked:
          pu.backup_only = std::any_of(u.files.begin(), u.files.end(), [&](std::uint32_t fi) {
            return marked.count(scan.files[fi].path) != 0;
          });
          break;
      }
      if (scan.explicit_files) pu.backup_only = true;
    }
    plan.units.push_back(std::move(pu));
  }

  assign_names(h, idx, scan, plan, cancel);
  if (cancel.load(std::memory_order_relaxed)) return err(status::cancelled);
  return plan;
}

std::string plan_to_json(const scan_result& scan, const plan_result& plan, double bytes_per_second) {
  struct day_row {
    std::uint32_t units = 0;
    std::uint32_t fresh = 0;
    std::uint32_t selected = 0;
    std::uint32_t imported = 0;
  };
  std::map<std::string, day_row> days;
  std::map<std::string, std::uint32_t> folders;
  std::uint64_t selected_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint32_t selected_units = 0;
  std::uint32_t selected_files = 0;
  std::uint32_t duplicates = 0;
  std::uint32_t imported = 0;
  std::uint32_t fresh = 0;
  std::uint32_t backup_only = 0;
  for (const plan_unit& pu : plan.units) {
    const unit& u = scan.units[pu.unit];
    day_row& d = days[pu.day];
    ++d.units;
    total_bytes += u.bytes;
    if (pu.state == unit_state::fresh) {
      ++d.fresh;
      ++fresh;
    }
    if (pu.state == unit_state::duplicate) ++duplicates;
    if (pu.state == unit_state::imported) {
      ++imported;
      ++d.imported;
    }
    // Backup-only work is work: it counts toward what the Import button
    // copies and the ETA.
    if (pu.backup_only) ++backup_only;
    if (pu.selected || pu.backup_only) {
      ++d.selected;
      ++selected_units;
      selected_files += static_cast<std::uint32_t>(u.files.size());
      selected_bytes += u.bytes;
      ++folders[pu.folder];
    }
  }

  json::writer w;
  w.begin_object();
  w.key("plan_id").integer(static_cast<std::int64_t>(plan.id));
  w.key("scan_id").integer(static_cast<std::int64_t>(plan.scan_id));
  w.key("source").begin_object();
  w.key("root").string(scan.root);
  w.key("label").string(scan.label);
  w.key("volume_id").string(scan.volume_id);
  w.key("network").boolean(scan.network);
  w.end_object();
  w.key("destination").string(plan.destination);
  w.key("backup").string(plan.backup);
  w.key("preset").string(plan.settings.name);
  w.key("totals").begin_object();
  w.key("units").integer(static_cast<std::int64_t>(plan.units.size()));
  w.key("bytes").integer(static_cast<std::int64_t>(total_bytes));
  w.key("new").integer(fresh);
  w.key("duplicates").integer(duplicates);
  w.key("imported").integer(imported);
  w.key("selected_units").integer(selected_units);
  w.key("selected_files").integer(selected_files);
  w.key("selected_bytes").integer(static_cast<std::int64_t>(selected_bytes));
  w.key("backup_only").integer(backup_only);
  // The ETA comes from throughput measured on this device before, never a
  // guess: without a measurement it is -1 and the chrome says so.
  w.key("bytes_per_second").number(bytes_per_second);
  w.key("eta_seconds").integer(bytes_per_second > 0
                                   ? static_cast<std::int64_t>(static_cast<double>(selected_bytes) /
                                                               bytes_per_second)
                                   : -1);
  w.end_object();

  w.key("days").begin_array();
  for (const auto& [day, d] : days) {
    w.begin_object();
    w.key("day").string(day);
    w.key("units").integer(d.units);
    w.key("new").integer(d.fresh);
    w.key("imported").integer(d.imported);
    w.key("selected").integer(d.selected);
    w.end_object();
  }
  w.end_array();

  w.key("folders").begin_array();
  for (const auto& [folder, count] : folders) {
    w.begin_object();
    w.key("folder").string(folder);
    w.key("units").integer(count);
    w.end_object();
  }
  w.end_array();

  w.key("units").begin_array();
  for (std::size_t i = 0; i < plan.units.size(); ++i) {
    const plan_unit& pu = plan.units[i];
    const unit& u = scan.units[pu.unit];
    const source_file& primary = scan.files[u.files.front()];
    w.begin_object();
    w.key("i").integer(static_cast<std::int64_t>(i));
    w.key("day").string(pu.day);
    w.key("kind").string(kind_name(u.kind));
    w.key("type").string(type_name(primary.type));
    w.key("state").string(state_name(pu.state));
    w.key("selected").boolean(pu.selected);
    w.key("path").string(primary.path);
    w.key("rel").string(primary.rel);
    w.key("taken").integer(u.taken);
    w.key("dated").boolean(u.has_date && plan.settings.dates == date_source::taken);
    w.key("camera").string(u.camera);
    w.key("bytes").integer(static_cast<std::int64_t>(u.bytes));
    w.key("folder").string(pu.folder);
    w.key("names").begin_array();
    for (const std::string& n : pu.names) w.string(n);
    w.end_array();
    w.key("sources").begin_array();
    for (std::uint32_t fi : u.files) w.string(scan.files[fi].name);
    w.end_array();
    w.key("matched").string(pu.matched);
    w.key("renamed_for_clash").boolean(pu.renamed_for_clash);
    w.key("backup_only").boolean(pu.backup_only);
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

std::string preview_json(const preset& p, const std::string& default_library) {
  // Three camera-shaped samples: a RAW+JPEG pair, a phone HEIC, a clip.
  struct sample {
    const char* names[2];
    std::int64_t taken;
    const char* camera;
    file_type type;
    bool raw;
    const char* dir;
  };
  static constexpr sample kSamples[] = {
      {{"IMG_0001.JPG", "IMG_0001.CR3"}, 1789992000, "Canon EOS R5", file_type::jpeg, true, "DCIM/100CANON"},
      {{"IMG_4211.HEIC", nullptr}, 1790078400, "iPhone 16 Pro", file_type::heic, false, "DCIM/100APPLE"},
      {{"C0001.MP4", nullptr}, 1790164800, "ILCE-7M4", file_type::video, false, "PRIVATE/M4ROOT/CLIP"},
  };
  json::writer w;
  w.begin_object();
  w.key("destination").string(normalize_root(p.destination.empty() ? default_library : p.destination));
  w.key("samples").begin_array();
  std::uint32_t seq = 0;
  for (const sample& s : kSamples) {
    layout_input li;
    li.taken = s.taken;
    li.camera = s.camera;
    li.has_raw = s.raw;
    li.primary_type = s.type;
    li.source_rel_dir = s.dir;
    const std::string folder = layout_folder(p, li);
    const std::string old_stem(stem_of(s.names[0]));
    const std::string stem = p.rename.empty()
                                 ? old_stem
                                 : render_stem(p.rename, rename_input{s.taken, s.camera, ++seq, old_stem});
    w.begin_object();
    w.key("folder").string(folder);
    w.key("names").begin_array();
    for (const char* n : s.names) {
      if (n) w.string(with_stem(n, old_stem, stem));
    }
    w.end_array();
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

}  // namespace mv::import
