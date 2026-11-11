// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue a phone reads (docs/plans/mobile.md §5, §6): one generation
// built beside the index from a read snapshot, then delivered to a NAS folder
// so a reader sees the previous generation or this one, never a mix.
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

#include "core/result.h"

namespace mv::ai::catalog {

inline constexpr int kFormat = 1;
inline constexpr std::size_t kPublicKeyBytes = 32;
inline constexpr std::size_t kSecretKeyBytes = 64;
inline constexpr std::size_t kSignatureBytes = 64;
// One per asset, inside the phone's copy. A 64 px JPEG is 1.5 to 2 KB.
inline constexpr std::uint32_t kTinyEdge = 64;
inline constexpr std::uint32_t kTinyQuality = 72;
// What a retained generation no longer names stays this long (§5 rule 4).
inline constexpr std::int64_t kUnusedSeconds = 30 * 24 * 3600;
// A pack under this is folded into the next one, so a superseded small pack is
// routine, not a loss: it goes as soon as no retained generation needs it.
inline constexpr int kMaxThumbFailures = 3;
inline constexpr std::uint64_t kFoldBytes = 16u << 20;
inline constexpr std::int64_t kRollingUnusedSeconds = 2 * 3600;

// A new Ed25519 key pair: {public, secret}.
[[nodiscard]] std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> make_keypair();
// The public half of a secret key; empty when `secret` is the wrong size.
[[nodiscard]] std::vector<std::uint8_t> public_of(std::span<const std::uint8_t> secret);
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
// What the person compares on the desktop and the phone: the start of the key's hash.
[[nodiscard]] std::string fingerprint(std::span<const std::uint8_t> public_key);

struct current_info {
  std::string library_id;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::string manifest_sha256;
};
// A current.json, believed only if `public_key` signed exactly these fields.
// status::permission_denied when it did not; status::corrupt when unreadable.
[[nodiscard]] result<current_info> verify_current(std::string_view text, std::span<const std::uint8_t> public_key);

// The whole current.json for `c`, signed with `secret_key`; "" when it cannot sign.
[[nodiscard]] std::string signed_current_json(const current_info& c, std::span<const std::uint8_t> secret_key);

// One indexed folder as a phone knows it: no path of this machine.
struct root_map {
  std::int64_t id = 0;   // in the index
  std::string path;      // here, on this machine; never written out
  std::string uuid;      // stable across generations
  std::string share;     // the share's name, no server and no user
  std::string sub;       // under the share, '/'-separated, "" at its top
};

struct control {
  std::function<bool()> cancelled;
  std::function<void(double)> report;  // 0..1 of this step
  std::function<void()> yield;         // between tables, while the viewer needs the machine
};

// The files a reader takes from the share beside the generation, content
// addressed and kept across generations.
struct shared_file {
  std::string source;  // here; deliver() copies it when the share lacks it
  std::string rel;     // under .mediaviewer/: "thumbs/t-….pack", "models/<spec>/…"
  std::uint64_t size = 0;
  std::string sha256;
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

struct deliver_options {
  std::string gen_dir;      // a finished generation from build()
  std::string destination;  // the chosen folder; its .mediaviewer/ is ours
  std::string library_id;
  std::uint64_t generation = 0;
  std::int64_t now_unix = 0;
  std::uint64_t epoch = 1;
  std::vector<std::uint8_t> public_key;
  std::vector<std::uint8_t> secret_key;
  std::int64_t keep_seconds = 2 * 3600;  // an older generation, once its successor has been out this long
  std::vector<shared_file> shared;  // copied first; those the share already has are left
  std::string retention_file;       // here: when each unused shared file was last named
  std::string arrivals_file;        // here: when each generation became current, by this clock
  // Verified copy to a new file in an existing folder.
  std::function<expected(const std::string& from, const std::string& to)> copy;
  std::function<expected(const std::string& dir)> make_directories;
};

// status::permission_denied: that folder holds another library's catalogue,
// or this library under another key. status::not_found: a shared file is on
// neither side (the share lost it and it was not kept here).
[[nodiscard]] expected deliver(const deliver_options& o, const control& c);

// What the folder has now: the newest generation, 0 when it has none.
// status::permission_denied as above; status::io when it cannot be read.
[[nodiscard]] result<std::uint64_t> published_generation(const std::string& destination,
                                                         const std::string& library_id,
                                                         std::span<const std::uint8_t> public_key);

// Shared files no retained generation names and unused for 30 days go (§5
// rule 4). `state_file` (here) remembers since when each has been unused; an
// unreadable manifest deletes nothing. Failing to tidy never fails a publish.
void retain_shared(const std::string& base, const std::string& state_file, std::int64_t now_unix,
                   std::int64_t unused_seconds = kUnusedSeconds);

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

// The reader's side, in the format build() writes (little-endian, sorted by
// asset id, so a phone can binary-search a mapping). A bad file is an error.
struct thumb_location {
  std::string pack;
  std::uint64_t offset = 0;
  std::uint32_t length = 0;
};
[[nodiscard]] bool valid_pack_name(std::string_view name);
// Whether a tile lies inside a pack of this size: check before reading it.
[[nodiscard]] bool within_pack(const thumb_location& at, std::uint64_t pack_size);
[[nodiscard]] result<std::optional<thumb_location>> find_thumb(std::span<const std::uint8_t> thumbs_idx,
                                                               std::int64_t asset);
[[nodiscard]] result<std::optional<std::span<const std::uint8_t>>> find_tiny(std::span<const std::uint8_t> tiny_pack,
                                                                             std::int64_t asset);

}  // namespace mv::ai::catalog
