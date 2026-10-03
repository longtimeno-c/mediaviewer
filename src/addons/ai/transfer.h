// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharing an index (docs/design/17 "Sharing an index", 2026-09-28): one file that
// carries what an index knows about some roots, so another machine (or this
// one, pointed at the same NAS through another path) need not embed the
// library again.
//
// The file is SQLite, `info.format` = kFormat, version kVersion:
//
//   info(key, value)          format, version, created, from, picture_spec,
//                             face_spec, faces, thumbs
//   roots(id, name, path, recursive, media)       path: where it was, for the offer
//   assets(id, root_id, rel, mtime, size, kind, duration_ms)   rel: '/'-separated
//   progress(asset_id, spec, state, resume_ms, indexed_at)     done / partial only
//   frames(asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)
//   speech(asset_id, spec, start_ms, end_ms, text)
//   -- with People (the user ticked it):
//   people(id, name, created_at)
//   faces(id, asset_id, pts_ms, x, y, w, h, score, person_id, emb, pinned, quality, tta)
//   rejected(face_id, person_id)   no_merge(a, b)   face_scanned(asset_id, spec)
//   -- with thumbnails:
//   thumbs(asset_id, pts_ms, jpeg)                 pts_ms -1: the file's own
//
// Nothing here logs a path, a name or a vector (rule 6). Every call is a
// control-thread call.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

struct sqlite3;

namespace mv::ai::transfer {

inline constexpr char kFormat[] = "mediaviewer.index";
inline constexpr int kVersion = 1;

// Progress in [0, 1] for the chrome; cancel polled between batches.
struct control {
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(double)> progress;
  [[nodiscard]] bool cancelled() const noexcept { return cancel && cancel->load(); }
  void report(double f) const {
    if (progress) progress(f);
  }
};

// ---- export ---------------------------------------------------------------------

// One thumbnail an export carries: the file's own (pts_ms -1; a clip's is its
// poster) or a moment's.
struct thumb_want {
  std::string path;
  std::int64_t pts_ms = -1;
  bool video = false;
  std::int64_t duration_ms = 0;
};

struct export_options {
  std::string index_db;               // read on its own connection
  std::string faces_db;               // "" : no People in the file
  std::vector<std::int64_t> roots;    // empty: every root
  bool faces = false;
  bool thumbs = false;
  std::string picture_spec;           // the spec that answers here (info)
  std::string face_spec;              // the People model's (info)
  std::string from;                   // "macOS arm64", "Windows x64" (info)
  // The JPEG-512 of a file (pts -1) or a moment: the viewer's cached one, or
  // made now when it never was (the caller decodes; this file never does).
  std::function<result<std::vector<std::uint8_t>>(const thumb_want&)> thumb;
};

struct export_counts {
  std::uint64_t roots = 0;
  std::uint64_t assets = 0;
  std::uint64_t frames = 0;
  std::uint64_t speech = 0;
  std::uint64_t faces = 0;
  std::uint64_t people = 0;
  std::uint64_t thumbs = 0;
  std::uint64_t thumbs_missing = 0;  // could not be made (an unreadable or moved file)
  std::uint64_t bytes = 0;
};

// Writes `dest` (through `dest.part`, renamed at the end; removed on failure
// or cancel).
[[nodiscard]] result<export_counts> write(const std::string& dest, const export_options& o,
                                          const control& c);

// ---- reading a file ---------------------------------------------------------------

struct file_root {
  std::int64_t id = 0;
  std::string name;
  std::string path;
  bool recursive = true;
  std::uint32_t media = 0;
  std::uint64_t assets = 0;
};

struct file_info {
  int version = 0;
  std::int64_t created = 0;
  std::string from;
  std::string picture_spec;
  std::vector<std::string> specs;  // every spec with progress rows
  std::string face_spec;
  bool faces = false;
  std::uint64_t face_count = 0;
  std::uint64_t people = 0;
  bool thumbs = false;
  std::uint64_t thumb_count = 0;
  std::vector<file_root> roots;
};

// status::unsupported_format: not an index file, or a newer version.
[[nodiscard]] result<file_info> inspect(const std::string& file);

// ---- import -------------------------------------------------------------------------

// A root of the file and where its files are on this machine.
struct root_target {
  std::int64_t file_root = 0;
  std::string dir;
  std::int64_t local_root = 0;  // the local root that covers `dir` (engine::index_folder)
};

struct imported_asset {
  std::int64_t file_id = 0;
  std::int64_t local_id = 0;
  std::string path;          // this machine's
  std::int64_t mtime = 0;    // the file's row
  std::uint64_t size = 0;
  bool added = false;        // a new row here (not one this machine had)
};

struct import_counts {
  std::uint64_t assets = 0;       // rows of the chosen roots
  std::uint64_t added = 0;        // new here
  std::uint64_t replaced = 0;     // rows here that were not done, now the file's
  std::uint64_t kept = 0;         // rows here kept (done, or a different file)
  std::uint64_t frames = 0;
  std::uint64_t speech = 0;
  std::uint64_t skipped_rows = 0; // progress rows of a spec not used here
  std::uint64_t faces = 0;
  std::uint64_t people_new = 0;
  std::uint64_t people_joined = 0;
  std::uint64_t thumbs = 0;
  std::uint64_t thumbs_skipped = 0;  // the file here differs, or is not reachable
};

// This machine's path for a file-relative one under `dir`.
[[nodiscard]] std::string local_path(const std::string& dir, const std::string& rel);

// Step 1, on index.db's connection (index_db::with_connection): the rows of
// the targets' roots, for `specs` only, in one transaction.
[[nodiscard]] expected import_index(sqlite3* db, const std::string& file,
                                    std::span<const root_target> targets,
                                    const std::set<std::string>& specs,
                                    std::vector<imported_asset>& out, import_counts& counts,
                                    const control& c);

// Step 2, on faces.db's connection: the People of the imported assets this
// machine has not scanned for `face_spec`. Named people join the local person
// of the same name.
[[nodiscard]] expected import_faces(sqlite3* fdb, const std::string& file, const std::string& face_spec,
                                    std::span<const imported_asset> assets, import_counts& counts,
                                    const control& c);

// Step 3: the file's thumbnails for assets whose file here has the file's
// (mtime, size), through `store`.
struct thumb_io {
  std::function<bool(const std::string& path, std::int64_t& mtime, std::uint64_t& size)> stat;
  std::function<expected(const std::string& path, std::int64_t pts_ms, std::span<const std::uint8_t> jpeg)> store;
  std::function<void()> yield;  // waits while the viewer is busy
};
[[nodiscard]] expected import_thumbs(const std::string& file, std::span<const imported_asset> assets,
                                     const thumb_io& io, import_counts& counts, const control& c);

}  // namespace mv::ai::transfer
