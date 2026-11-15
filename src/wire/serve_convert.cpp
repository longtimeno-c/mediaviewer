// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/serve_convert.h"

#include <sodium.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mv::driftwire::serve::convert {

namespace fs = std::filesystem;

namespace {

fs::path fs_path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

std::string read_small(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return text.size() > kMaxRequestBytes ? std::string() : text;
}

std::string segment_name(std::uint32_t k) { return "seg-" + std::to_string(k) + ".ts"; }

}  // namespace

std::string cache_key(const source& s) {
  // Each field with its length, so no two sources run together into one input.
  std::string in;
  auto put = [&in](std::string_view v) {
    in += std::to_string(v.size());
    in += ':';
    in += v;
  };
  put(s.root_id);
  put(s.rel);
  put(std::to_string(s.size));
  put(std::to_string(s.mtime));
  put(std::to_string(kFormat));
  unsigned char h[16];
  crypto_generichash(h, sizeof h, reinterpret_cast<const unsigned char*>(in.data()), in.size(), nullptr, 0);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char b : h) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

struct service::state {
  options o;
  fs::path dir;

  struct job {
    std::string key;
    source src;
    bool is_plan = false;
    std::uint32_t k = 0;
    bool asked = false;  // a phone waits on it; else made ahead
    std::vector<plan_cb> plans;
    std::vector<segment_cb> segments;
  };

  mutable std::mutex m;
  std::condition_variable cv;
  std::deque<job> queue;           // under m
  std::unique_ptr<job> current;    // under m: the job running now, which a caller may join
  std::set<std::string> not_clips; // under m: keys whose plan said not a clip
  std::map<std::string, convert_plan> plans;  // under m: plans read or made, by key
  stats st;                        // under m
  bool stopping = false;           // under m
  std::uint64_t generation = 0;    // under m: bumped by purge, so a job made before it is not kept
  std::thread worker;

  fs::path key_dir(const std::string& key) const { return dir / key; }

  // ---- the cache's size (under m) ----

  struct entry {
    fs::path path;
    fs::file_time_type used;
    std::uint64_t bytes = 0;
  };

  std::uint64_t scan(std::vector<entry>* out) const {
    std::error_code ec;
    std::uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const auto bytes = it->file_size(ec);
      if (ec) continue;
      total += bytes;
      if (out && it->path().extension() == ".ts") out->push_back({it->path(), it->last_write_time(ec), bytes});
    }
    return total;
  }

  // Oldest-used segments go until the cache is under its bound (the newest one stays).
  void trim_locked(const fs::path& keep) {
    if (st.cached_bytes <= o.max_bytes) return;
    std::vector<entry> all;
    st.cached_bytes = scan(&all);
    std::sort(all.begin(), all.end(), [](const entry& a, const entry& b) { return a.used < b.used; });
    std::error_code ec;
    for (const auto& e : all) {
      if (st.cached_bytes <= o.max_bytes) break;
      if (e.path == keep) continue;
      if (fs::remove(e.path, ec)) st.cached_bytes -= std::min(st.cached_bytes, e.bytes);
    }
  }

  static void touch(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
  }

  // ---- lookups (under m) ----

  std::optional<convert_plan> cached_plan_locked(const std::string& key) {
    if (auto it = plans.find(key); it != plans.end()) return it->second;
    auto p = parse_convert_plan(read_small(key_dir(key) / "plan.json"));
    if (p) plans[key] = *p;
    return p;
  }

  bool has_segment(const std::string& key, std::uint32_t k) const {
    std::error_code ec;
    return fs::is_regular_file(key_dir(key) / segment_name(k), ec);
  }

  job* find_locked(const std::string& key, bool is_plan, std::uint32_t k) {
    if (current && current->key == key && current->is_plan == is_plan && (is_plan || current->k == k)) return current.get();
    for (auto& j : queue) {
      if (j.key == key && j.is_plan == is_plan && (is_plan || j.k == k)) return &j;
    }
    return nullptr;
  }

  // Asked first (in the order asked), then ahead.
  void order_locked() {
    std::stable_partition(queue.begin(), queue.end(), [](const job& j) { return j.asked; });
  }

  void ahead_locked(const source& src, const std::string& key, std::uint32_t k) {
    // Segment 0 alone is a poster as often as a start: ahead only once a second one is asked.
    if (k == 0) return;
    // A seek, or another clip: what was queued ahead of anything else goes.
    std::erase_if(queue, [&](const job& j) {
      return !j.is_plan && !j.asked && (j.key != key || j.k <= k || j.k > k + static_cast<std::uint32_t>(o.ahead));
    });
    const auto p = cached_plan_locked(key);
    if (!p) return;
    for (int i = 1; i <= o.ahead; ++i) {
      const std::uint32_t n = k + static_cast<std::uint32_t>(i);
      if (n >= p->segments || has_segment(key, n) || find_locked(key, false, n)) continue;
      job j;
      j.key = key;
      j.src = src;
      j.k = n;
      queue.push_back(std::move(j));
    }
  }

  // ---- the worker ----

  void loop() {
    for (;;) {
      job* j = nullptr;
      std::uint64_t gen = 0;
      {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) return;
        current = std::make_unique<job>(std::move(queue.front()));
        queue.pop_front();
        j = current.get();
        st.busy = true;
        gen = generation;
      }
      run_one(*j, gen);
    }
  }

  void run_one(job& j, std::uint64_t gen) {
    std::error_code ec;
    const fs::path d = key_dir(j.key);
    fs::create_directories(d, ec);
    const fs::path final_path = j.is_plan ? d / "plan.json" : d / segment_name(j.k);
    const fs::path part = fs::path(final_path).concat(".part");
    fs::remove(part, ec);
    const std::string part_u8 = [&] {
      const auto u = part.u8string();
      return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }();
    ran r = ran::failed;
    try {
      r = j.is_plan ? (o.run.plan ? o.run.plan(j.src.path, o.segment_ms, part_u8) : ran::failed)
                    : (o.run.segment ? o.run.segment(j.src.path, j.k, o.segment_ms, part_u8) : ran::failed);
    } catch (...) {
      r = ran::failed;
    }
    std::optional<convert_plan> plan;
    if (r == ran::ok && j.is_plan) {
      plan = parse_convert_plan(read_small(part));
      if (!plan || plan->segment_ms != o.segment_ms) r = ran::failed;
    }
    if (r == ran::ok && !j.is_plan && (!fs::is_regular_file(part, ec) || fs::file_size(part, ec) == 0)) r = ran::failed;
    std::vector<plan_cb> plan_cbs;
    std::vector<segment_cb> seg_cbs;
    outcome out = r == ran::ok ? outcome::ok : r == ran::not_a_clip ? outcome::not_a_clip : outcome::failed;
    {
      std::lock_guard lock(m);
      st.busy = false;
      if (gen != generation) out = outcome::off;  // purged while it ran
      if (out == outcome::ok) {
        fs::rename(part, final_path, ec);
        if (ec) out = outcome::failed;
      }
      if (out != outcome::ok) fs::remove(part, ec);
      if (out == outcome::ok) {
        ++st.made;
        if (plan) plans[j.key] = *plan;
        st.cached_bytes += fs::file_size(final_path, ec);
        trim_locked(final_path);
      } else if (out == outcome::not_a_clip) {
        not_clips.insert(j.key);
      } else if (out == outcome::failed) {
        ++st.failed;
      }
      // Callers that joined the job while it ran.
      plan_cbs = std::move(j.plans);
      seg_cbs = std::move(j.segments);
      j.plans.clear();
      j.segments.clear();
    }
    const auto u = final_path.u8string();
    const std::string file(reinterpret_cast<const char*>(u.data()), u.size());
    {
      std::lock_guard lock(m);
      current.reset();
    }
    for (auto& cb : plan_cbs) cb(out, plan.value_or(convert_plan{}));
    for (auto& cb : seg_cbs) cb(out, out == outcome::ok ? file : std::string());
  }
};

std::unique_ptr<service> service::start(options o) {
  std::unique_ptr<service> s(new service());
  s->st_ = std::make_shared<state>();
  s->st_->o = std::move(o);
  s->st_->dir = fs_path(s->st_->o.cache_dir);
  std::error_code ec;
  fs::create_directories(s->st_->dir, ec);
  fs::permissions(s->st_->dir, fs::perms::owner_all, fs::perm_options::replace, ec);
  // Unfinished files of a run that ended mid-job.
  for (auto it = fs::recursive_directory_iterator(s->st_->dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->path().extension() == ".part") fs::remove(it->path(), ec);
  }
  s->st_->st.cached_bytes = s->st_->scan(nullptr);
  s->st_->worker = std::thread([st = s->st_.get()] { st->loop(); });
  return s;
}

service::~service() {
  std::deque<state::job> left;
  {
    std::lock_guard lock(st_->m);
    st_->stopping = true;
    left.swap(st_->queue);
  }
  st_->cv.notify_all();
  if (st_->worker.joinable()) st_->worker.join();
  for (auto& j : left) {
    for (auto& cb : j.plans) cb(outcome::off, {});
    for (auto& cb : j.segments) cb(outcome::off, {});
  }
}

void service::plan(const source& s, plan_cb done) {
  const std::string key = cache_key(s);
  std::optional<convert_plan> hit;
  bool not_clip = false;
  {
    std::lock_guard lock(st_->m);
    if (st_->stopping) {
      not_clip = false;
    } else if (st_->not_clips.count(key)) {
      not_clip = true;
    } else if ((hit = st_->cached_plan_locked(key))) {
      ++st_->st.hits;
    } else {
      auto* j = st_->find_locked(key, true, 0);
      if (!j) {
        state::job n;
        n.key = key;
        n.src = s;
        n.is_plan = true;
        st_->queue.push_back(std::move(n));
        j = &st_->queue.back();
      }
      j->asked = true;
      j->plans.push_back(std::move(done));
      st_->order_locked();
      st_->cv.notify_all();
      return;
    }
  }
  if (hit) return done(outcome::ok, *hit);
  done(not_clip ? outcome::not_a_clip : outcome::off, {});
}

void service::segment(const source& s, std::uint32_t k, segment_cb done) {
  const std::string key = cache_key(s);
  std::string file;
  {
    std::lock_guard lock(st_->m);
    if (st_->stopping) {
      // falls through to off
    } else if (st_->has_segment(key, k)) {
      ++st_->st.hits;
      const fs::path p = st_->key_dir(key) / segment_name(k);
      state::touch(p);
      const auto u = p.u8string();
      file.assign(reinterpret_cast<const char*>(u.data()), u.size());
      st_->ahead_locked(s, key, k);
      st_->cv.notify_all();
    } else {
      auto* j = st_->find_locked(key, false, k);
      if (!j) {
        state::job n;
        n.key = key;
        n.src = s;
        n.k = k;
        st_->queue.push_back(std::move(n));
        j = &st_->queue.back();
      }
      j->asked = true;
      j->segments.push_back(std::move(done));
      st_->ahead_locked(s, key, k);
      st_->order_locked();
      st_->cv.notify_all();
      return;
    }
  }
  done(file.empty() ? outcome::off : outcome::ok, file);
}

void service::purge() {
  std::deque<state::job> left;
  {
    std::lock_guard lock(st_->m);
    left.swap(st_->queue);
    ++st_->generation;
    st_->plans.clear();
    st_->not_clips.clear();
    std::error_code ec;
    for (auto it = fs::directory_iterator(st_->dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      std::error_code rm;
      fs::remove_all(it->path(), rm);
    }
    st_->st.cached_bytes = st_->scan(nullptr);
  }
  for (auto& j : left) {
    for (auto& cb : j.plans) cb(outcome::off, {});
    for (auto& cb : j.segments) cb(outcome::off, {});
  }
}

stats service::now() const {
  std::lock_guard lock(st_->m);
  return st_->st;
}

}  // namespace mv::driftwire::serve::convert
