// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "geo/open.h"

#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace mv::geo {

result<std::unique_ptr<mapped_database>> open_database(std::string_view utf8_path) {
  auto file = io::mapped_file::open(utf8_path);
  if (!file) return err(file.error());
  std::unique_ptr<mapped_database> out(new (std::nothrow) mapped_database());
  if (!out) return err(status::out_of_memory);
  out->file_ = std::move(file.value());
  auto db = database::from_bytes(out->file_.bytes());
  if (!db) return err(db.error());
  out->db_ = std::move(db.value());
  return out;
}

namespace {

struct shared_state {
  std::mutex mu;
  std::string path;
  bool tried = false;
  // Every database ever handed out stays alive for the process: a worker may
  // still hold a pointer after the path changes.
  std::vector<std::unique_ptr<mapped_database>> keep;
  std::atomic<const database*> current{nullptr};
};

shared_state& state() {
  // Leaked deliberately: a function-local static would be destroyed at exit
  // while a worker may still be inside shared() or holding a database.
  static shared_state* s = new shared_state;
  return *s;
}

}  // namespace

void set_data_path(std::string_view utf8_path) {
  shared_state& s = state();
  std::lock_guard lock(s.mu);
  if (s.path == utf8_path) return;
  s.path.assign(utf8_path);
  s.tried = false;
  s.current.store(nullptr, std::memory_order_release);
}

const database* shared() {
  shared_state& s = state();
  if (const database* d = s.current.load(std::memory_order_acquire)) return d;
  // First use: map on this worker. The lock is held across the open (~10 ms,
  // once per process) so concurrent first callers wait rather than map twice.
  std::lock_guard lock(s.mu);
  if (const database* d = s.current.load(std::memory_order_acquire)) return d;
  if (s.tried || s.path.empty()) return nullptr;
  s.tried = true;
  auto opened = open_database(s.path);
  if (!opened) return nullptr;
  try {
    s.keep.push_back(std::move(opened.value()));
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
  const database* d = &s.keep.back()->db();
  s.current.store(d, std::memory_order_release);
  return d;
}

}  // namespace mv::geo
