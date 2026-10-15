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
//   * issue #207: the rating and colour label of every file in a folder, for
//     View > Show's cull filters (Picks, Not rejected, a colour), filled the
//     same way and dropped by `invalidate` (a sidecar write leaves the key).
//   * #345: the provenance of the item on screen, for the canvas's AI badge,
//     pane open or not. A held full record answers it; otherwise one bounded
//     probe (meta/probe.h, a few kilobytes) runs on a worker, and its answer is
//     kept in its own, larger LRU of a few strings per file.
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
#include "meta/generators.h"
#include "io/pairing.h"
#include "meta/meta.h"

namespace mv::shell {

class meta_store {
 public:
  using loader_fn = std::function<result<meta::metadata>(std::string_view utf8_path)>;
  using cull_reader_fn = std::function<io::cull_marks(std::string_view utf8_path)>;
  using probe_fn = std::function<meta::provenance(std::string_view utf8_path)>;
  // Runs on the pool thread. `path` is the item the record is for.
  using ready_fn = std::function<void(std::string path)>;

  // Defaults read through meta::read / meta::read_cull_marks (also when
  // `culls` is empty) / meta::probe_provenance (also when `probe` is empty).
  meta_store();
  explicit meta_store(loader_fn loader, cull_reader_fn culls = {}, probe_fn probe = {});
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

  // Issue #207: rating + label, read per folder in the background. `cull_marks_of` answers a
  // whole listing under one lock (nullopt = not read yet); it is a cache read,
  // never a file read, so the UI thread's relist can call it. `resolve_cull_marks`
  // reads the missing ones on one background job (a newer call supersedes it)
  // and `consume_cull_changed` is true once after any arrived.
  [[nodiscard]] std::vector<std::optional<io::cull_marks>> cull_marks_of(
      std::span<const io::dir_entry> entries) const;
  void resolve_cull_marks(std::vector<io::dir_entry> entries, job_system& jobs,
                          std::function<void()> on_done);
  [[nodiscard]] bool consume_cull_changed() noexcept;
  // Cull reads performed (tests: a second relist must not read again).
  [[nodiscard]] std::uint64_t cull_reads() const noexcept;

  // #345: what the file says about how it was made, for the AI badge. A full
  // record already held for `e` answers at once (no probe); so does an earlier
  // probe. Otherwise one probe is submitted (none while one for the key is in
  // flight), `on_ready` fires on the pool thread when it lands, and this returns
  // nullopt. The probe is meta::probe_provenance: bounded reads of the header
  // bytes, never the full read.
  [[nodiscard]] std::optional<meta::provenance> provenance(const io::dir_entry& e, job_system& jobs,
                                                           ready_fn on_ready);
  // Cache only (a held record, or a probe that landed) -- never submits.
  [[nodiscard]] std::optional<meta::provenance> peek_provenance(const io::dir_entry& e) const;
  // How many probes have run: a test holds held-arrow and cache hits to zero.
  [[nodiscard]] std::uint64_t probes() const noexcept;

  // PR 12: drops every cached record for `utf8_path` (whatever its mtime and
  // size), and its rating + label (issue #207), so the next `get` or cull
  // relist reads it again. A write that landed in a sidecar
  // leaves the file's own mtime and size alone, so the key cannot tell.
  void invalidate(std::string_view utf8_path);

  // How many full metadata reads have been performed. For tests and the F3
  // overlay: the number must not move when an overlay is toggled.
  [[nodiscard]] std::uint64_t reads() const noexcept;
  void clear();

  static constexpr std::size_t kCapacity = 48;
  // Provenance answers are a few short strings each; a whole folder's worth fits.
  static constexpr std::size_t kProvenanceCapacity = 1024;

 private:
  struct state;
  std::shared_ptr<state> state_;
};

}  // namespace mv::shell
