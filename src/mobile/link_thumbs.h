// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// JPEG-512s from thumbs/*.pack (docs/plans/mobile.md §10, M6): one worker
// merges neighbouring requests into one read_range, and a bounded on-disk LRU
// (Caches, purgeable) keeps what came, keyed by what the pack names.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "addons/ai/catalog.h"
#include "core/result.h"
#include "mobile/link.h"

namespace mv::mobile {

class thumb_cache {
 public:
  using done_fn = std::function<void(result<std::vector<std::uint8_t>>)>;

  thumb_cache(std::string dir, std::uint64_t capacity_bytes, remote_file& remote);
  ~thumb_cache();

  // The catalogue root every pack is under (service root id); empty pauses reads.
  void set_catalogue_root(std::string root);

  // `lane` 0: on screen, 1: prefetch. `done` runs on a worker. Cancel the
  // returned flag to drop a want nobody needs now: a want shared with a live
  // one stays alive for that one.
  std::shared_ptr<cancel_flag> request(const ai::catalog::thumb_location& where, int lane, done_fn done);

  [[nodiscard]] std::uint64_t bytes_used() const;
  void clear();

 private:
  struct want;
  void worker();
  void admit(const std::string& name, std::uint64_t size);
  void trim_locked();
  void scan();
  [[nodiscard]] std::string path_of(const std::string& name) const;

  std::string dir_;
  std::uint64_t capacity_;
  remote_file& remote_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::string root_;
  std::vector<std::shared_ptr<want>> queue_;
  std::map<std::string, std::shared_ptr<want>> by_key_;  // queued and in flight, for dedupe
  std::thread worker_a_, worker_b_;

  bool scanned_ = false;
  std::list<std::pair<std::string, std::uint64_t>> lru_;  // newest first
  std::unordered_map<std::string, std::list<std::pair<std::string, std::uint64_t>>::iterator> index_;
  std::uint64_t used_ = 0;
};

}  // namespace mv::mobile
