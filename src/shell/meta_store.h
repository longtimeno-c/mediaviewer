// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host-side cache in front of meta::read (PR 9, docs/design/06 + docs/design/16).
//
// Two jobs, both off the UI and render threads (rule 1):
//   * one full `metadata` per opened item, read once on a worker and kept in a
//     small LRU. Everything that shows metadata — the pane, the info overlay,
//     the AF quads — reads that record, so toggling any of them is a lookup,
//     never a file read (PR 9 verify: "toggling AF points and the info overlay
//     does not re-read the file"). `reads()` counts the reads so a test can
//     hold the store to that.
// The date-taken sort keys moved to meta::tag_index (#291): the folder pass
// that reads every file's tags gives them too, from meta_cache.db.
//
// Thread model: `get` / `peek` are UI-thread calls that only take a short
// mutex; loads run on the job system. `on_ready` fires on
// the pool thread — the host marshals it (no dispatcher here, docs/design/14).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "io/dir.h"
#include "meta/meta.h"

namespace mv::shell {

class meta_store {
 public:
  using loader_fn = std::function<result<meta::metadata>(std::string_view utf8_path)>;
  // Runs on the pool thread. `path` is the item the record is for.
  using ready_fn = std::function<void(std::string path)>;

  // Defaults to meta::read.
  meta_store();
  explicit meta_store(loader_fn loader);
  ~meta_store();

  meta_store(const meta_store&) = delete;
  meta_store& operator=(const meta_store&) = delete;

  // The record for `e`, or null while it is still loading (a miss submits one
  // read; a second miss for the same key while it is in flight submits none).
  // A file that could not be read caches an *empty* record, so a broken file is
  // not retried on every toggle. Stale entries (mtime/size moved) are re-read.
  [[nodiscard]] std::shared_ptr<const meta::metadata> get(const io::dir_entry& e, job_system& jobs,
                                                          ready_fn on_ready);
  // Cache only — never submits.
  [[nodiscard]] std::shared_ptr<const meta::metadata> peek(const io::dir_entry& e) const;

  // PR 12: drops every cached record for `utf8_path` (whatever its mtime and
  // size), so the next `get` reads it again. A write that landed in a sidecar
  // leaves the file's own mtime and size alone, so the key cannot tell.
  void invalidate(std::string_view utf8_path);

  // How many full metadata reads have been performed. For tests and the F3
  // overlay: the number must not move when an overlay is toggled.
  [[nodiscard]] std::uint64_t reads() const noexcept;
  void clear();

  static constexpr std::size_t kCapacity = 48;

 private:
  struct state;
  std::shared_ptr<state> state_;
};

}  // namespace mv::shell
