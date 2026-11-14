// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/library_check.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "addons/import/paths.h"
#include "addons/import/planner.h"
#include "core/json.h"

namespace mv::import {
namespace {

// Deep enough for any real library; a link is never followed (walk_files).
constexpr int kCheckDepth = 64;

bool digest_from_hex(std::string_view hex, digest& out) {
  if (hex.size() != out.size() * 2) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  digest d{};
  for (std::size_t i = 0; i < d.size(); ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    d[i] = static_cast<std::uint8_t>(hi << 4 | lo);
  }
  out = d;
  return true;
}

// A copy's temporary (io/verified_copy.h temp_name_for): never a library file.
bool is_temporary(std::string_view name) {
  const auto at = name.rfind(".mvtmp");
  if (at == std::string_view::npos) return false;
  for (std::size_t i = at + 6; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

// Rows of `size` in scope. "This destination" is its volume key and, until
// they are adopted, its absolute path (planner.h adopt_legacy_root).
std::vector<library_row> rows_in_scope(library_index& idx, std::uint64_t size, dup_scope scope,
                                       const std::string& key, const std::string& path) {
  if (scope == dup_scope::library) return idx.by_size(size, nullptr);
  std::vector<library_row> out = idx.by_size(size, &key);
  if (path != key) {
    std::vector<library_row> old = idx.by_size(size, &path);
    out.insert(out.end(), std::make_move_iterator(old.begin()), std::make_move_iterator(old.end()));
  }
  return out;
}

bool parse_scope(const json::value& doc, dup_scope& out) {
  const json::value* v = doc.find("scope");
  if (!v) return true;
  if (v->k != json::kind::string) return false;
  if (v->s == "destination") {
    out = dup_scope::destination;
  } else if (v->s == "library") {
    out = dup_scope::library;
  } else {
    return false;
  }
  return true;
}

bool parse_size(const json::value* v, std::uint64_t& out) {
  if (!v || v->k != json::kind::number || !v->is_integer || v->i < 0) return false;
  out = static_cast<std::uint64_t>(v->i);
  return true;
}

// Where a library hash `h` of `size` is in scope, besides the files of this
// walk (`here`): other destinations' rows, confirmed where they are now.
void confirm_elsewhere(const host& h, library_index& idx, const dest_locator& loc,
                       const std::string& key, const std::string& dest, std::uint64_t size,
                       const digest& hash, const std::atomic<bool>& cancel,
                       std::vector<std::string>& where) {
  for (const library_row& c : idx.by_size(size, nullptr)) {
    if (c.hash != hash || c.root == key || c.root == dest) continue;
    const auto live = loc.live_root(c.root);
    if (!live) continue;  // offline: nothing can be confirmed, and the row stays
    auto now = destination_hash(h, idx, *live, c.root, c.rel, cancel);
    if (!now || *now != hash) continue;
    std::string full = join_native(*live, c.rel);
    if (std::find(where.begin(), where.end(), full) == where.end()) where.push_back(std::move(full));
  }
}

}  // namespace

result<check_request> parse_check_request(std::string_view json_text) {
  const auto doc = json::parse(json_text);
  if (!doc || doc->k != json::kind::object) return err(status::invalid_arg);
  check_request r;
  const std::string* dest = doc->str("destination");
  if (!dest || dest->empty()) return err(status::invalid_arg);
  r.destination = normalize_root(*dest);
  if (!parse_scope(*doc, r.scope)) return err(status::invalid_arg);
  if (const json::value* v = doc->find("source_folder")) {
    if (v->k != json::kind::string) return err(status::invalid_arg);
    if (!v->s.empty()) r.source_folder = normalize_root(v->s);
  }
  if (const json::value* v = doc->find("source_sizes")) {
    if (v->k != json::kind::array) return err(status::invalid_arg);
    r.source_sizes.reserve(v->a.size());
    for (const json::value& n : v->a) {
      std::uint64_t size = 0;
      if (!parse_size(&n, size)) return err(status::invalid_arg);
      r.source_sizes.push_back(size);
    }
  }
  if (const json::value* v = doc->find("history")) {
    if (v->k != json::kind::boolean) return err(status::invalid_arg);
    r.history = v->b;
  }
  if (const json::value* v = doc->find("expected")) {
    if (v->k != json::kind::array) return err(status::invalid_arg);
    for (const json::value& e : v->a) {
      if (e.k != json::kind::object) return err(status::invalid_arg);
      check_request::expected_file f;
      const std::string* rel = e.str("rel");
      if (!rel || rel->empty() || !parse_size(e.find("size"), f.size)) return err(status::invalid_arg);
      f.rel = *rel;
      for (char& c : f.rel) {
        if (c == '\\') c = '/';
      }
      if (const json::value* hv = e.find("hash")) {
        digest d{};
        if (hv->k != json::kind::string || !digest_from_hex(hv->s, d)) return err(status::invalid_arg);
        f.hash = d;
      }
      r.expected.push_back(std::move(f));
    }
  }
  return r;
}

check_result run_library_check(const host& h, library_index& idx, const dest_locator& loc,
                               const check_request& req, const check_hooks& hooks) {
  check_result out;
  out.destination = req.destination;
  std::atomic<bool> local_flag{false};
  const std::atomic<bool>& cancel_flag = hooks.cancel_flag ? *hooks.cancel_flag : local_flag;
  const auto cancelled = [&] {
    if (!local_flag.load(std::memory_order_relaxed) && hooks.cancelled && hooks.cancelled()) {
      local_flag.store(true, std::memory_order_relaxed);
    }
    return local_flag.load(std::memory_order_relaxed) || cancel_flag.load(std::memory_order_relaxed);
  };
  copy_callbacks cb;
  cb.cancelled = cancelled;
  cb.yield = hooks.yield;
  const auto report = [&](check_phase p, std::uint64_t done, std::uint64_t total, std::uint64_t bytes,
                          const std::string& current) {
    if (hooks.progress) hooks.progress(p, done, total, bytes, current);
  };

  const std::string& dest = req.destination;
  if (auto st = h.stat(dest); !st || !st->is_directory) return out;  // offline: says nothing
  out.reachable = true;
  const std::string key = loc.key_of(dest);
  const std::string source_prefix = req.source_folder.empty() ? std::string() : join_native(req.source_folder, "");

  // 1. The library walk: sizes and mtimes, no reads.
  struct entry {
    std::string rel;  // '/'-separated under dest
    std::uint64_t size;
    std::int64_t mtime;
  };
  std::vector<entry> lib;
  auto last = std::chrono::steady_clock::now();
  const auto walked = h.walk(dest, kCheckDepth, [&](const mv_addon_file_entry& e) {
    if (e.size > 0 && !is_temporary(e.name_utf8 ? e.name_utf8 : "")) {
      // A source folder inside the library is the source, not the library.
      const bool in_source = !source_prefix.empty() && std::string_view(e.path_utf8).rfind(source_prefix, 0) == 0;
      if (!in_source) {
        lib.push_back({e.relative_utf8, e.size, e.mtime_unix});
        out.library_bytes += e.size;
      }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::milliseconds(250)) {
      last = now;
      report(check_phase::walking, lib.size(), 0, 0, e.relative_utf8 ? e.relative_utf8 : "");
    }
    return !cancelled();
  });
  out.library_files = lib.size();
  out.walk_complete = walked.has_value() && !cancelled();

  // 2. The source side: a folder walked here, or sizes the host sent.
  struct source_entry {
    std::string path;
    std::uint64_t size;
    std::int64_t mtime;
  };
  std::vector<source_entry> src;
  if (!req.source_folder.empty() && !cancelled()) {
    (void)h.walk(req.source_folder, kCheckDepth, [&](const mv_addon_file_entry& e) {
      if (e.size > 0 && !is_temporary(e.name_utf8 ? e.name_utf8 : "")) {
        src.push_back({e.path_utf8, e.size, e.mtime_unix});
        out.source_bytes += e.size;
      }
      return !cancelled();
    });
    out.source_files = src.size();
  }
  std::unordered_set<std::uint64_t> source_sizes(req.source_sizes.begin(), req.source_sizes.end());
  for (const source_entry& s : src) source_sizes.insert(s.size);

  // 3. Which library files must have a hash: a size a source file has, or a
  // size another library file has (the duplicates list), or an expected
  // file's size (moved, not deleted).
  std::unordered_map<std::uint64_t, std::uint32_t> lib_sizes;
  for (const entry& f : lib) ++lib_sizes[f.size];
  std::unordered_set<std::uint64_t> expected_sizes;
  for (const auto& e : req.expected) expected_sizes.insert(e.size);
  std::unordered_map<std::string, library_row> known;
  {
    std::vector<library_row> rows = idx.by_root(key);
    if (key != dest) {
      std::vector<library_row> old = idx.by_root(dest);
      rows.insert(rows.end(), std::make_move_iterator(old.begin()), std::make_move_iterator(old.end()));
    }
    for (library_row& r : rows) known.emplace(r.rel, std::move(r));
  }
  std::vector<std::size_t> want;
  std::uint64_t to_read = 0;
  for (std::size_t i = 0; i < lib.size(); ++i) {
    const entry& f = lib[i];
    if (!source_sizes.count(f.size) && lib_sizes[f.size] < 2 && !expected_sizes.count(f.size)) continue;
    want.push_back(i);
    const auto it = known.find(f.rel);
    if (it == known.end() || it->second.size != f.size || it->second.mtime != f.mtime) to_read += f.size;
  }

  // 4. Hashes: remembered where size and mtime still match, read (cached,
  // yielding) otherwise. Stored in one transaction at the end, and on cancel
  // what was read so far.
  std::vector<std::optional<digest>> lib_hash(lib.size());
  std::vector<library_row> upserts;
  std::uint64_t done = 0;
  for (const std::size_t i : want) {
    if (cancelled()) break;
    const entry& f = lib[i];
    const auto it = known.find(f.rel);
    if (it != known.end() && it->second.size == f.size && it->second.mtime == f.mtime) {
      lib_hash[i] = it->second.hash;
      ++out.remembered;
      continue;
    }
    report(check_phase::hashing_library, done, to_read, out.hashed_bytes, f.rel);
    auto d = h.hash(join_native(dest, f.rel), false, cb);
    if (!d) {
      if (d.error() == status::cancelled) break;
      out.unreadable.push_back(join_native(dest, f.rel));
      continue;
    }
    lib_hash[i] = *d;
    ++out.hashed;
    out.hashed_bytes += f.size;
    done += f.size;
    upserts.push_back(library_row{key, f.rel, f.size, f.mtime, *d});
  }

  // Rows of files that have gone: dropped only after a complete walk, and only
  // when the file's folder still answers (an emptied mount point, a folder
  // moved away: the row is the only memory of the file).
  std::vector<std::pair<std::string, std::string>> drops;
  if (out.walk_complete) {
    std::unordered_set<std::string> present;
    present.reserve(lib.size());
    for (const entry& f : lib) present.insert(f.rel);
    for (const auto& [rel, row] : known) {
      if (present.count(rel)) continue;
      const std::string full = join_native(dest, rel);
      if (!source_prefix.empty() && full.rfind(source_prefix, 0) == 0) continue;
      if (auto st = h.stat(full); st && !st->is_directory) continue;  // a file the walk skips
      const auto folder = h.stat(parent_native(full));
      if (folder && folder->is_directory) drops.emplace_back(row.root, rel);
    }
    out.dropped = static_cast<std::uint32_t>(drops.size());
  }
  // Only a cache: a failed store rolls back and the next check reads again.
  (void)idx.store(upserts, drops);

  // The library's content, for lookups below.
  std::map<std::pair<std::uint64_t, digest>, std::vector<std::size_t>> by_content;
  for (std::size_t i = 0; i < lib.size(); ++i) {
    if (lib_hash[i]) by_content[{lib[i].size, *lib_hash[i]}].push_back(i);
  }
  const auto where_is = [&](std::uint64_t size, const digest& hash, const std::string& exclude) {
    std::vector<std::string> where;
    if (auto it = by_content.find({size, hash}); it != by_content.end()) {
      for (const std::size_t i : it->second) {
        std::string full = join_native(dest, lib[i].rel);
        if (full != exclude) where.push_back(std::move(full));
      }
    }
    if (req.scope == dup_scope::library) confirm_elsewhere(h, idx, loc, key, dest, size, hash, cancel_flag, where);
    std::sort(where.begin(), where.end());
    return where;
  };

  // 5. A folder source: hashes remembered by path (find duplicates' memory),
  // read only where a size matches the library in scope.
  if (!src.empty() && !cancelled()) {
    std::unordered_map<std::string, seen_row> seen;
    for (seen_row& r : idx.seen_under(source_prefix)) seen.emplace(r.path, std::move(r));
    std::vector<seen_row> remember;
    std::uint64_t src_done = 0;  // bytes of the source hashed so far, of src_to_read
    std::uint64_t src_to_read = 0;
    for (const source_entry& s : src) {
      if (lib_sizes.count(s.size) > 0) src_to_read += s.size;
    }
    for (const source_entry& s : src) {
      if (cancelled()) break;
      const bool size_here = lib_sizes.count(s.size) > 0;
      const bool size_elsewhere =
          req.scope == dup_scope::library && !idx.by_size(s.size, nullptr).empty();
      if (!size_here && !size_elsewhere) {
        out.not_found.push_back(s.path);
        out.not_found_bytes += s.size;
        continue;
      }
      digest d{};
      if (auto it = seen.find(s.path); it != seen.end() && it->second.size == s.size && it->second.mtime == s.mtime) {
        d = it->second.hash;
      } else {
        report(check_phase::hashing_source, src_done, src_to_read, src_done, s.path);
        auto r = h.hash(s.path, false, cb);
        if (!r) {
          if (r.error() == status::cancelled) break;
          out.unreadable.push_back(s.path);
          continue;
        }
        d = *r;
        remember.push_back({s.path, s.size, s.mtime, d});
        src_done += s.size;
      }
      std::vector<std::string> where = where_is(s.size, d, s.path);
      if (where.empty()) {
        out.not_found.push_back(s.path);
        out.not_found_bytes += s.size;
      } else {
        out.found.push_back({s.path, s.size, std::move(where)});
      }
    }
    (void)idx.seen_store(remember, nullptr);
  }

  // 6. A host source: which of its sizes the library in scope has at all.
  if (!req.source_sizes.empty()) {
    std::set<std::uint64_t> matched;
    for (const std::uint64_t size : source_sizes) {
      if (lib_sizes.count(size) || !rows_in_scope(idx, size, req.scope, key, dest).empty()) matched.insert(size);
    }
    out.matched_sizes.assign(matched.begin(), matched.end());
  }

  // 7. Duplicates inside the library folder.
  for (const auto& [content, members] : by_content) {
    if (members.size() < 2) continue;
    check_group g;
    g.size = content.first;
    for (const std::size_t i : members) g.paths.push_back(join_native(dest, lib[i].rel));
    std::sort(g.paths.begin(), g.paths.end());
    out.duplicates.push_back(std::move(g));
  }
  std::sort(out.duplicates.begin(), out.duplicates.end(), [](const check_group& a, const check_group& b) {
    const std::uint64_t wa = a.size * (a.paths.size() - 1);
    const std::uint64_t wb = b.size * (b.paths.size() - 1);
    if (wa != wb) return wa > wb;
    return a.paths.front() < b.paths.front();
  });

  // 8. Expected files that are not where the record says: moved (the same
  // bytes elsewhere in scope) or deleted. Only after a complete walk: a walk
  // cut short, or a destination that went away, says nothing.
  if (out.walk_complete && !req.expected.empty()) {
    std::unordered_set<std::string> present;
    present.reserve(lib.size());
    for (const entry& f : lib) present.insert(f.rel);
    for (const auto& e : req.expected) {
      if (present.count(e.rel)) continue;
      // The walk skips what it cannot list (a cloud-only file, a subfolder a
      // share would not answer for): only a file that is really not there,
      // in a destination that answers, is gone.
      // Only "not found" is gone: a stat refused (permissions, I/O, a share
      // timing out on one folder) says nothing about the file.
      if (auto st = h.stat(join_native(dest, e.rel)); st || st.error() != status::not_found) continue;
      check_gone g;
      g.rel = e.rel;
      if (e.hash) {
        std::vector<std::string> where = where_is(e.size, *e.hash, {});
        if (!where.empty()) g.moved_to = where.front();
      }
      out.gone.push_back(std::move(g));
    }
  }
  out.cancelled = cancelled();
  return out;
}

std::string check_json(const check_result& r, std::uint64_t job_id, std::int64_t elapsed_ms, bool full) {
  std::uint64_t dup_files = 0;
  std::uint64_t wasted = 0;
  for (const check_group& g : r.duplicates) {
    dup_files += g.paths.size() - 1;
    wasted += g.size * (g.paths.size() - 1);
  }
  std::uint64_t deleted = 0;
  std::uint64_t moved = 0;
  for (const check_gone& g : r.gone) (g.moved_to.empty() ? deleted : moved) += 1;
  json::writer w;
  w.begin_object();
  w.key("job").integer(static_cast<std::int64_t>(job_id));
  w.key("kind").string("check");
  w.key("destination").string(r.destination);
  w.key("reachable").boolean(r.reachable);
  w.key("walk_complete").boolean(r.walk_complete);
  w.key("cancelled").boolean(r.cancelled);
  w.key("library_files").integer(static_cast<std::int64_t>(r.library_files));
  w.key("library_bytes").integer(static_cast<std::int64_t>(r.library_bytes));
  w.key("hashed").integer(r.hashed);
  w.key("hashed_bytes").integer(static_cast<std::int64_t>(r.hashed_bytes));
  w.key("remembered").integer(r.remembered);
  w.key("dropped").integer(r.dropped);
  w.key("source_files").integer(static_cast<std::int64_t>(r.source_files));
  w.key("source_bytes").integer(static_cast<std::int64_t>(r.source_bytes));
  w.key("found_count").integer(static_cast<std::int64_t>(r.found.size()));
  w.key("not_found_count").integer(static_cast<std::int64_t>(r.not_found.size()));
  w.key("not_found_bytes").integer(static_cast<std::int64_t>(r.not_found_bytes));
  w.key("duplicate_groups").integer(static_cast<std::int64_t>(r.duplicates.size()));
  w.key("duplicate_files").integer(static_cast<std::int64_t>(dup_files));
  w.key("wasted_bytes").integer(static_cast<std::int64_t>(wasted));
  w.key("deleted_count").integer(static_cast<std::int64_t>(deleted));
  w.key("moved_count").integer(static_cast<std::int64_t>(moved));
  w.key("unreadable_count").integer(static_cast<std::int64_t>(r.unreadable.size()));
  if (full) {
    w.key("found").begin_array();
    for (const check_found& f : r.found) {
      w.begin_object();
      w.key("path").string(f.path);
      w.key("size").integer(static_cast<std::int64_t>(f.size));
      w.key("where").begin_array();
      for (const std::string& p : f.where) w.string(p);
      w.end_array();
      w.end_object();
    }
    w.end_array();
    w.key("not_found").begin_array();
    for (const std::string& p : r.not_found) w.string(p);
    w.end_array();
    w.key("matched_sizes").begin_array();
    for (const std::uint64_t s : r.matched_sizes) w.integer(static_cast<std::int64_t>(s));
    w.end_array();
    w.key("duplicates").begin_array();
    for (const check_group& g : r.duplicates) {
      w.begin_object();
      w.key("size").integer(static_cast<std::int64_t>(g.size));
      w.key("paths").begin_array();
      for (const std::string& p : g.paths) w.string(p);
      w.end_array();
      w.end_object();
    }
    w.end_array();
    w.key("gone").begin_array();
    for (const check_gone& g : r.gone) {
      w.begin_object();
      w.key("rel").string(g.rel);
      w.key("moved_to").string(g.moved_to);
      w.end_object();
    }
    w.end_array();
    w.key("unreadable").begin_array();
    for (const std::string& p : r.unreadable) w.string(p);
    w.end_array();
  }
  w.key("elapsed_ms").integer(elapsed_ms);
  w.end_object();
  return w.take();
}

std::string check_report_text(const check_result& r) {
  std::string t = "Check library: " + r.destination + "\n";
  if (!r.reachable) return t + "The folder could not be reached; nothing was checked.\n";
  if (r.cancelled) t += "Cancelled; the counts below are for what was checked.\n";
  t += std::to_string(r.library_files) + " files in the library folder; " + std::to_string(r.hashed) +
       " read now, " + std::to_string(r.remembered) + " already known.\n";
  if (r.source_files) {
    t += "\nAlready in the library (" + std::to_string(r.found.size()) + "):\n";
    for (const check_found& f : r.found) {
      t += "  " + f.path + "\n";
      for (const std::string& w : f.where) t += "    = " + w + "\n";
    }
    t += "\nNot in the library (" + std::to_string(r.not_found.size()) + "):\n";
    for (const std::string& p : r.not_found) t += "  " + p + "\n";
  }
  if (!r.gone.empty()) {
    t += "\nNo longer where they were backed up (" + std::to_string(r.gone.size()) + "):\n";
    for (const check_gone& g : r.gone) {
      t += "  " + g.rel + (g.moved_to.empty() ? "  (deleted)\n" : "  (moved to " + g.moved_to + ")\n");
    }
  }
  t += "\nDuplicates already in the library (" + std::to_string(r.duplicates.size()) + " sets):\n";
  for (const check_group& g : r.duplicates) {
    t += "  " + std::to_string(g.paths.size()) + " identical files, " + std::to_string(g.size) + " bytes each:\n";
    for (const std::string& p : g.paths) t += "    " + p + "\n";
  }
  for (const std::string& p : r.unreadable) t += "unreadable  " + p + "\n";
  return t;
}

result<find_request> parse_find_request(std::string_view json_text) {
  const auto doc = json::parse(json_text);
  if (!doc || doc->k != json::kind::object) return err(status::invalid_arg);
  find_request r;
  const std::string* dest = doc->str("destination");
  if (!dest || dest->empty()) return err(status::invalid_arg);
  r.destination = normalize_root(*dest);
  if (!parse_scope(*doc, r.scope) || !parse_size(doc->find("size"), r.size)) return err(status::invalid_arg);
  if (const json::value* v = doc->find("hash")) {
    digest d{};
    if (v->k != json::kind::string || !digest_from_hex(v->s, d)) return err(status::invalid_arg);
    r.hash = d;
  }
  if (const json::value* v = doc->find("exclude")) {
    if (v->k != json::kind::string) return err(status::invalid_arg);
    r.exclude = v->s;
  }
  return r;
}

find_answer library_find(const host& h, library_index& idx, const dest_locator& loc,
                         const find_request& req, const std::atomic<bool>& cancel) {
  find_answer a;
  const std::string key = loc.key_of(req.destination);
  std::vector<library_row> rows = rows_in_scope(idx, req.size, req.scope, key, req.destination);
  a.candidates = static_cast<std::uint32_t>(rows.size());
  if (!req.hash) return a;
  for (const library_row& c : rows) {
    if (c.hash != *req.hash) continue;
    const auto live = loc.live_root(c.root);
    if (!live) continue;  // offline: nothing can be confirmed, and the row stays
    auto now = destination_hash(h, idx, *live, c.root, c.rel, cancel);
    if (!now || *now != *req.hash) continue;
    std::string full = join_native(*live, c.rel);
    if (full == req.exclude) continue;
    if (std::find(a.paths.begin(), a.paths.end(), full) == a.paths.end()) a.paths.push_back(std::move(full));
  }
  std::sort(a.paths.begin(), a.paths.end());
  return a;
}

std::string find_json(const find_answer& a) {
  json::writer w;
  w.begin_object();
  w.key("candidates").integer(a.candidates);
  w.key("paths").begin_array();
  for (const std::string& p : a.paths) w.string(p);
  w.end_array();
  w.end_object();
  return w.take();
}

expected library_record(const host& h, library_index& idx, const dest_locator& loc, std::string_view json_text) {
  const auto doc = json::parse(json_text);
  if (!doc || doc->k != json::kind::object) return err(status::invalid_arg);
  const std::string* dest = doc->str("destination");
  const std::string* path = doc->str("path");
  const std::string* hex = doc->str("hash");
  std::uint64_t size = 0;
  const auto mtime = doc->integer("mtime");
  digest d{};
  if (!dest || !path || !hex || !mtime || !parse_size(doc->find("size"), size) || !digest_from_hex(*hex, d)) {
    return err(status::invalid_arg);
  }
  const std::string root = normalize_root(*dest);
  const std::string prefix = join_native(root, "");
  if (path->size() <= prefix.size() || path->compare(0, prefix.size(), prefix) != 0) return err(status::invalid_arg);
  // The index trusts a row while size and mtime match: the file must be the
  // one described, now.
  auto st = h.stat(*path);
  if (!st || st->is_directory || st->size != size || st->mtime != *mtime) return err(status::invalid_arg);
  std::string rel = path->substr(prefix.size());
  for (char& c : rel) {
    if (c == '\\') c = '/';
  }
  idx.upsert(library_row{loc.key_of(root), rel, size, *mtime, d});
  return {};
}

}  // namespace mv::import
