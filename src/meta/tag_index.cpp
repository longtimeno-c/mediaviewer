// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/tag_index.h"

#include "io/file_port.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mv::meta {

struct tag_index::state {
  cache_fn cache_of;
  reader_fn reader;

  mutable std::mutex m;
  std::string dir;
  std::vector<std::string> order;  // the listing, for match() and each_location()
  struct stamp {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
  };
  std::unordered_map<std::string, stamp> listed;
  struct known_file {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    search_facts facts;
    std::string folded;
  };
  std::unordered_map<std::string, known_file> files;
  std::uint32_t known = 0;  // listed files with facts for their current stamp
  bool reading = false;
  std::vector<std::string> forget;  // invalidated paths whose cache rows still stand
  std::string selected_dir;  // the folder whose cache rows are in `files` (one SELECT per folder)

  std::atomic<std::uint64_t> scan{0};
  std::atomic<bool> changed{false};
  std::atomic<std::uint64_t> reads{0};

  // Under m: a listed file's facts are current.
  [[nodiscard]] bool fresh(const std::string& path) const {
    const auto l = listed.find(path);
    if (l == listed.end()) return false;
    const auto f = files.find(path);
    return f != files.end() && f->second.size == l->second.size && f->second.mtime == l->second.mtime;
  }
  // Under m: keeps `row`. A row for a file not listed now (or listed with
  // another stamp) is kept too, so a later relist of the folder finds it
  // without another SELECT; only a listed, current row counts as known.
  void keep(cached_facts&& row) {
    const bool was = fresh(row.path);
    const auto f = files.find(row.path);
    if (was && f->second.size == row.size && f->second.mtime == row.mtime) return;
    std::string path = row.path;
    files[path] = known_file{row.size, row.mtime, std::move(row.facts), std::move(row.folded)};
    if (!was && fresh(path)) ++known;
  }
};

namespace {

using entries_ptr = std::shared_ptr<const std::vector<io::dir_entry>>;

void submit_chunk(const std::shared_ptr<tag_index::state>& st, std::uint64_t scan, entries_ptr entries,
                  std::size_t from, job_system& jobs, tag_index::notify_fn notify, tag_index::busy_fn busy);

status run_chunk(const std::shared_ptr<tag_index::state>& st, std::uint64_t scan, const entries_ptr& entries,
                 std::size_t from, job_system& jobs, const tag_index::notify_fn& notify,
                 const tag_index::busy_fn& busy) {
  const auto current = [&] { return st->scan.load(std::memory_order_acquire) == scan; };
  if (!current()) return status::cancelled;
  meta_cache* cache = st->cache_of ? st->cache_of() : nullptr;
  if (cache) {
    std::vector<std::string> forget;
    {
      std::lock_guard lock(st->m);
      forget.swap(st->forget);
    }
    for (const std::string& p : forget) cache->forget(p);
  }
  if (from == 0 && cache) {
    // A folder seen before: one SELECT, no file reads. Once per folder: a
    // relist of the same folder (a watcher event, a re-sort) already holds
    // every row in memory, and this process wrote the rows it read since.
    std::string dir;
    {
      std::lock_guard lock(st->m);
      if (st->selected_dir != st->dir) dir = st->dir;
    }
    if (!dir.empty()) {
      std::vector<cached_facts> rows = cache->folder(dir);
      bool any = false;
      std::vector<std::string> unlisted;
      {
        std::lock_guard lock(st->m);
        if (!current()) return status::cancelled;
        const std::uint32_t before = st->known;
        for (cached_facts& r : rows) {
          if (!st->listed.count(r.path)) unlisted.push_back(r.path);
          st->keep(std::move(r));
        }
        any = st->known != before;
        st->selected_dir = dir;
      }
      if (any) {
        st->changed.store(true, std::memory_order_release);
        if (notify) notify();
      }
      // Rows of files that are gone (deleted, renamed) leave the cache, so a
      // folder visited again does not grow it. A file only filtered out of
      // the listing (Settings hides a kind) still exists and keeps its row.
      std::vector<std::string> gone;
      for (const std::string& p : unlisted) {
        if (!io::stat_path(p)) gone.push_back(p);
      }
      if (!gone.empty()) {
        cache->forget_many(gone);
        std::lock_guard lock(st->m);
        for (const std::string& p : gone) st->files.erase(p);
      }
    }
  }
  const std::size_t to = std::min(entries->size(), from + tag_index::kChunk);
  std::vector<cached_facts> got;
  got.reserve(to - from);
  std::size_t next = to;  // where the next chunk starts
  for (std::size_t i = from; i < to; ++i) {
    const io::dir_entry& e = (*entries)[i];
    {
      std::lock_guard lock(st->m);
      if (st->fresh(e.path_utf8)) continue;
    }
    // The view goes first (CLAUDE.md "The view comes first"). A view job
    // waiting for a worker (the photo's decode, the +-2 prefetch, a page)
    // gets this one: the chunk re-queues itself from here, behind it. One
    // already running, or the present loop panning or playing, is waited out
    // here; sleeping costs no core and no view job is waiting for this one.
    bool yielded = false;
    for (;;) {
      if (!current()) return status::cancelled;
      if (jobs.foreground_queued() != 0) {
        yielded = true;
        break;
      }
      if (jobs.foreground_running() == 0 && !(busy && busy())) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (yielded) {
      next = i;
      break;
    }
    st->reads.fetch_add(1, std::memory_order_relaxed);
    cached_facts row;
    row.path = e.path_utf8;
    row.size = e.size;
    row.mtime = e.mtime_unix;
    if (auto f = st->reader(e.path_utf8)) row.facts = std::move(f).value();
    // else: unreadable. An empty row stands, so it is not read again.
    row.folded = folded_text(row.facts.fields);
    got.push_back(std::move(row));
  }
  if (cache && !got.empty()) (void)cache->store(got);
  const bool last = next >= entries->size();
  {
    std::lock_guard lock(st->m);
    if (!current()) return status::cancelled;
    for (cached_facts& r : got) st->keep(std::move(r));
    if (last) st->reading = false;
  }
  if (!got.empty() || last) {
    st->changed.store(true, std::memory_order_release);
    if (notify) notify();
  }
  if (!last) submit_chunk(st, scan, entries, next, jobs, notify, busy);
  return status::ok;
}

void submit_chunk(const std::shared_ptr<tag_index::state>& st, std::uint64_t scan, entries_ptr entries,
                  std::size_t from, job_system& jobs, tag_index::notify_fn notify, tag_index::busy_fn busy) {
  // Behind every foreground job and every background job already queued.
  const job_id id = jobs.submit_at(
      background_generation,
      [st, scan, entries, from, &jobs, notify, busy](const job_context&) -> status {
        return run_chunk(st, scan, entries, from, jobs, notify, busy);
      });
  if (id == invalid_job) {
    std::lock_guard lock(st->m);
    if (st->scan.load() == scan) st->reading = false;  // the pool is shutting down
  }
}

}  // namespace

tag_index::tag_index(meta_cache* cache, reader_fn reader)
    : tag_index(cache ? cache_fn([cache] { return cache; }) : cache_fn(), std::move(reader)) {}

tag_index::tag_index(cache_fn cache, reader_fn reader) : state_(std::make_shared<state>()) {
  state_->cache_of = std::move(cache);
  state_->reader = reader ? std::move(reader) : reader_fn([](std::string_view p) { return read_search_facts(p); });
}

tag_index::~tag_index() { state_->scan.fetch_add(1, std::memory_order_acq_rel); }

void tag_index::scan(std::string dir, std::vector<io::dir_entry> entries, job_system& jobs, notify_fn on_progress,
                     busy_fn busy) {
  const std::shared_ptr<state> st = state_;
  bool anything = false;
  std::uint64_t scan = 0;
  {
    std::lock_guard lock(st->m);
    scan = st->scan.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (dir != st->dir) {
      st->files.clear();
      st->selected_dir.clear();
    }
    st->dir = std::move(dir);
    st->order.clear();
    st->order.reserve(entries.size());
    st->listed.clear();
    st->listed.reserve(entries.size());
    for (const io::dir_entry& e : entries) {
      st->order.push_back(e.path_utf8);
      st->listed[e.path_utf8] = {e.size, e.mtime_unix};
    }
    st->known = 0;
    for (const std::string& p : st->order) {
      if (st->fresh(p)) ++st->known;
    }
    anything = st->known < st->order.size();
    st->reading = anything;
  }
  if (!anything) return;
  submit_chunk(st, scan, std::make_shared<const std::vector<io::dir_entry>>(std::move(entries)), 0, jobs,
               std::move(on_progress), std::move(busy));
}

void tag_index::stop() {
  std::lock_guard lock(state_->m);
  state_->scan.fetch_add(1, std::memory_order_acq_rel);
  state_->dir.clear();
  state_->order.clear();
  state_->listed.clear();
  state_->files.clear();
  state_->selected_dir.clear();
  state_->known = 0;
  state_->reading = false;
}

tag_index::progress_info tag_index::progress() const {
  std::lock_guard lock(state_->m);
  return {state_->known, static_cast<std::uint32_t>(state_->order.size()), state_->reading};
}

std::string tag_index::folder() const {
  std::lock_guard lock(state_->m);
  return state_->dir;
}

std::vector<tag_index::hit> tag_index::match(std::string_view typed) const {
  std::vector<hit> out;
  const std::vector<std::string> words = search_words(typed);
  std::size_t letters = 0;
  for (const std::string& w : words) letters += w.size();
  if (letters < 2) return out;
  std::lock_guard lock(state_->m);
  for (const std::string& p : state_->order) {
    const auto f = state_->files.find(p);
    if (f == state_->files.end() || f->second.folded.empty() || !state_->fresh(p)) continue;
    const int field = match_fields(f->second.folded, words);
    if (field < 0) continue;
    out.push_back({p, why_of(f->second.facts.fields, field)});
  }
  return out;
}

std::optional<std::int64_t> tag_index::date_key(const io::dir_entry& e) const {
  std::lock_guard lock(state_->m);
  const auto f = state_->files.find(e.path_utf8);
  if (f == state_->files.end() || f->second.size != e.size || f->second.mtime != e.mtime_unix) return std::nullopt;
  if (f->second.facts.date_taken_key == 0) return std::nullopt;
  return f->second.facts.date_taken_key;
}

bool tag_index::consume_changed() noexcept { return state_->changed.exchange(false, std::memory_order_acq_rel); }

std::optional<search_facts> tag_index::facts(std::string_view path) const {
  std::lock_guard lock(state_->m);
  const std::string p(path);
  if (!state_->fresh(p)) return std::nullopt;
  return state_->files.at(p).facts;
}

void tag_index::each_location(
    const std::function<bool(const std::string& path, const geo_point& where)>& visit) const {
  std::vector<std::pair<std::string, geo_point>> copy;
  {
    std::lock_guard lock(state_->m);
    for (const std::string& p : state_->order) {
      if (!state_->fresh(p)) continue;
      const auto& f = state_->files.at(p).facts;
      if (f.where) copy.emplace_back(p, *f.where);
    }
  }
  // Called outside the lock: a visitor may take its time.
  for (const auto& [p, w] : copy) {
    if (!visit(p, w)) return;
  }
}

void tag_index::invalidate(std::string_view path) {
  {
    std::lock_guard lock(state_->m);
    const std::string p(path);
    if (state_->fresh(p) && state_->known > 0) --state_->known;
    state_->files.erase(p);
    state_->forget.push_back(p);
  }
}

std::uint64_t tag_index::reads() const noexcept { return state_->reads.load(std::memory_order_relaxed); }

}  // namespace mv::meta
