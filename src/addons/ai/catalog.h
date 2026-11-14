// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue a phone reads (docs/plans/mobile.md §5, §6), Local search's
// half: one generation built beside the index from a read snapshot. Keys,
// current.json, delivery to a NAS folder (a reader sees the previous
// generation or this one, never a mix), retention and the pack readers are
// the generic half, catalogue/catalog.h, which the app's phone feature and
// the phone itself use too (docs/plans/phone-in-the-app.md).
//
// A generation is trusted through its Ed25519 signature over the library id,
// the epoch, the generation and the manifest's hash (verify_current), and the
// manifest names the hash of every file a reader will use, shared packs and
// model files included. Moving the key to another desktop is catalog_chain.h.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catalogue/catalog.h"
#include "core/result.h"

namespace mv::ai::catalog {

// One per asset, inside the phone's copy. A 64 px JPEG is 1.5 to 2 KB.
inline constexpr std::uint32_t kTinyEdge = 64;
inline constexpr std::uint32_t kTinyQuality = 72;
inline constexpr int kMaxThumbFailures = 3;

// One indexed folder as a phone knows it: no path of this machine.
struct root_map {
  std::int64_t id = 0;   // in the index
  std::string path;      // here, on this machine; never written out
  std::string uuid;      // stable across generations
  std::string share;     // the share's name, no server and no user
  std::string sub;       // under the share, '/'-separated, "" at its top
};

class thumb_store;

struct build_options {
  std::string index_db;      // the live index, read through its own connection
  std::string out_dir;       // created; removed again on failure
  std::string library_id;
  std::uint64_t epoch = 1;
  std::uint64_t generation = 0;
  std::int64_t created_unix = 0;
  std::string active_spec;
  std::string labels_file;   // copied when it exists
  std::string settings_json; // what a reader needs of this machine's settings
  std::vector<root_map> roots;

  // Everything below is optional: what is left unset is simply not in the
  // generation.
  thumb_store* thumbs = nullptr;  // sealed; thumbs.idx, tiny.pack and its packs
  std::string faces_db;           // the live faces.db: names and asset links only
  // A label per GPS point, "" for none; not_found: nothing here can name one.
  std::function<result<std::vector<std::string>>(std::span<const std::pair<double, double>>)> place_labels;
  std::string model_spec;              // active_spec's text tower ...
  std::vector<std::string> model_files;  // ... its files, here
  std::vector<std::string> preview_packs;  // finished previews/p-*.pack, here
  std::function<void()> after_thumb_snapshot;  // tests: the index moves on mid-build
};

struct build_counts {
  std::uint64_t assets = 0;
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  int schema = 0;
  std::uint64_t thumbs = 0;  // assets with a 64 px tile
  std::uint64_t people = 0;
  std::uint64_t places = 0;  // assets with a label
  std::vector<shared_file> shared;
};

[[nodiscard]] result<build_counts> build(const build_options& o, const control& c);

// ---- thumbnails ---------------------------------------------------------------------
// The publisher's own copy of what it has made, in a local ledger, so hours of
// generation survive a cancel: each tile is committed when made, and a pack is
// only sealed (named by its hash, never changed again) when a build needs it.

struct thumb_want {
  std::string path;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  int kind = 0;  // assets.kind
  std::int64_t duration_ms = 0;
  bool backfill = false;  // its tile exists; only the 64 px one is wanted
};

struct thumb_fill_io {
  std::function<result<std::vector<std::uint8_t>>(const thumb_want&)> jpeg512;
  std::function<result<std::vector<std::uint8_t>>(std::span<const std::uint8_t>)> tiny;
  std::function<void()> yield;  // waits while the viewer needs the machine
  std::function<bool()> share_present;  // asked after a failure; unset: taken as yes
};

struct thumb_fill_result {
  std::uint64_t made = 0;
  std::uint64_t failed = 0;  // the file has no picture we can make; not asked again
  bool complete = false;     // nothing left to try this round
};

class thumb_store {
 public:
  ~thumb_store();
  thumb_store(const thumb_store&) = delete;
  thumb_store& operator=(const thumb_store&) = delete;
  [[nodiscard]] static result<std::unique_ptr<thumb_store>> open(const std::string& dir);

  // What the index has under `roots` and the ledger lacks, newest first; rows
  // of files gone from the index are forgotten here. Cloud-only files are left
  // out: making their tile would download them.
  [[nodiscard]] result<std::vector<thumb_want>> missing(const std::string& index_db,
                                                        std::span<const std::int64_t> roots, bool want_tiny = true);
  // Makes tiles until done, cancelled (still status ok: `complete` false), the
  // share goes, or `budget_seconds` have gone (<= 0: no limit). A file that
  // fails without saying it has no picture is passed over and asked again next
  // run; after kMaxThumbFailures runs at one size and date it counts as having none.
  [[nodiscard]] result<thumb_fill_result> fill(std::span<const thumb_want> wants, const thumb_fill_io& io,
                                               const control& c, double budget_seconds);
  // Names the tiles not yet in a pack. Packs under `fold_bytes` are folded
  // into the next, so hourly publishes do not leave a pack per hour.
  [[nodiscard]] expected seal(std::uint64_t fold_bytes = kFoldBytes);
  // After a delivery: the local copy of a delivered pack goes, except one
  // small enough to be folded into the next.
  [[nodiscard]] expected delivered(std::uint64_t fold_bytes = kFoldBytes);
  // Forgets packs on neither side, so their tiles are made again.
  // `after_listing` (tests): the share changes between the listing and the stats.
  [[nodiscard]] expected reconcile(const std::string& destination, const std::function<void()>& after_listing = {});
  [[nodiscard]] result<std::string> hash_of(const std::string& file);  // cached by size and mtime
  [[nodiscard]] const std::string& ledger_path() const noexcept { return ledger_; }
  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

 private:
  thumb_store() = default;
  struct sqlite3_ref;
  std::unique_ptr<sqlite3_ref> db_;
  std::string dir_;
  std::string ledger_;
};

}  // namespace mv::ai::catalog
