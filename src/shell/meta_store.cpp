// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/meta_store.h"

#include <algorithm>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mv::shell {
namespace {

// (path, mtime, size): a file rewritten in place changes at least one.
std::string key_of(const io::dir_entry& e) {
  std::string k = e.path_utf8;
  k += '\0';
  k += std::to_string(e.mtime_unix);
  k += '\0';
  k += std::to_string(e.size);
  return k;
}

}  // namespace

struct meta_store::state {
  loader_fn loader;

  std::mutex mutex;
  // LRU: front = most recent.
  std::list<std::string> lru;
  struct record {
    std::shared_ptr<const meta::metadata> value;
    std::list<std::string>::iterator lru_it;
  };
  std::unordered_map<std::string, record> cache;
  std::unordered_set<std::string> in_flight;
  // PR 12: `invalidate` bumps `gen` and stamps the path, so a read that began
  // before a write landed cannot cache the pre-write record afterwards.
  std::uint64_t gen = 0;
  std::unordered_map<std::string, std::uint64_t> invalidated;

  std::atomic<std::uint64_t> reads{0};
};

meta_store::meta_store() : meta_store([](std::string_view p) { return meta::read(p); }) {}

meta_store::meta_store(loader_fn loader) : state_(std::make_shared<state>()) {
  state_->loader = std::move(loader);
}

meta_store::~meta_store() = default;

std::shared_ptr<const meta::metadata> meta_store::peek(const io::dir_entry& e) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto it = state_->cache.find(key_of(e));
  if (it == state_->cache.end()) return nullptr;
  state_->lru.splice(state_->lru.begin(), state_->lru, it->second.lru_it);
  return it->second.value;
}

std::shared_ptr<const meta::metadata> meta_store::get(const io::dir_entry& e, job_system& jobs,
                                                      ready_fn on_ready) {
  const std::string key = key_of(e);
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (const auto it = state_->cache.find(key); it != state_->cache.end()) {
      state_->lru.splice(state_->lru.begin(), state_->lru, it->second.lru_it);
      return it->second.value;
    }
    if (!state_->in_flight.insert(key).second) return nullptr;  // already loading
  }

  // The job owns the shared state, not `this`: the store can be destroyed while
  // a read is in flight (job_system's queue outlives its submitters).
  std::shared_ptr<state> st = state_;
  const std::string path = e.path_utf8;
  jobs.submit_at(
      background_generation,
      [st, key, path, ready = std::move(on_ready)](const job_context&) -> status {
        st->reads.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t started = 0;
        {
          std::lock_guard<std::mutex> lock(st->mutex);
          started = st->gen;
        }
        auto record = std::make_shared<meta::metadata>();
        if (auto r = st->loader(path); r) *record = std::move(r).value();
        // else: unreadable -> the empty record stands, cached below.
        {
          std::lock_guard<std::mutex> lock(st->mutex);
          st->in_flight.erase(key);
          const auto inv = st->invalidated.find(path);
          // A write landed while this read ran: what it read is out of date.
          // Do not keep it; the `ready` below makes the UI ask again.
          if (inv == st->invalidated.end() || inv->second <= started) {
            st->lru.push_front(key);
            st->cache[key] = {std::move(record), st->lru.begin()};
            while (st->lru.size() > kCapacity) {
              st->cache.erase(st->lru.back());
              st->lru.pop_back();
            }
          }
        }
        if (ready) ready(path);
        return status::ok;
      });
  return nullptr;
}

std::uint64_t meta_store::reads() const noexcept {
  return state_->reads.load(std::memory_order_relaxed);
}

void meta_store::invalidate(std::string_view utf8_path) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->invalidated.size() > 256) state_->invalidated.clear();
  state_->invalidated[std::string(utf8_path)] = ++state_->gen;
  // Keys are "path\0mtime\0size".
  std::string prefix(utf8_path);
  prefix += '\0';
  for (auto it = state_->cache.begin(); it != state_->cache.end();) {
    if (it->first.compare(0, prefix.size(), prefix) == 0) {
      state_->lru.erase(it->second.lru_it);
      it = state_->cache.erase(it);
    } else {
      ++it;
    }
  }
}

void meta_store::clear() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->cache.clear();
  state_->lru.clear();
}

}  // namespace mv::shell
