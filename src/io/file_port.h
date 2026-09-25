// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The file port under verified copies and Import (plan/18-import.md "Ports").
//
// Portable header; io/file_port_win.cpp (CreateFileW, FlushFileBuffers,
// FILE_FLAG_NO_BUFFERING, MoveFileExW without REPLACE_EXISTING) and
// io/file_port_mac.cpp (open O_EXCL, F_FULLFSYNC, F_NOCACHE, renamex_np
// RENAME_EXCL; the Linux core test build uses the same file) implement it. No
// HANDLE or fd in the interface (D9).
//
// Worker threads only, never the UI or render thread (rule 1). Nothing here
// logs a path (rule 6). Nothing here overwrites: create is exclusive and the
// rename refuses a taken name (rule 5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

// Uncached reads need sector-aligned buffers and lengths on Windows. Every
// buffer the copy pipeline uses is a multiple of this and aligned to it.
inline constexpr std::size_t kIoAlign = 4096;

class aligned_buffer {
 public:
  aligned_buffer() = default;
  // `bytes` is rounded up to a multiple of kIoAlign. Empty on allocation failure.
  explicit aligned_buffer(std::size_t bytes) noexcept;
  ~aligned_buffer();
  aligned_buffer(aligned_buffer&& other) noexcept;
  aligned_buffer& operator=(aligned_buffer&& other) noexcept;
  aligned_buffer(const aligned_buffer&) = delete;
  aligned_buffer& operator=(const aligned_buffer&) = delete;

  [[nodiscard]] std::uint8_t* data() noexcept { return data_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return data_ == nullptr; }

 private:
  std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

struct file_stat {
  std::uint64_t size = 0;
  std::int64_t mtime_unix = 0;
  bool is_directory = false;
};

// status::io when nothing is there or it cannot be read.
[[nodiscard]] result<file_stat> stat_path(std::string_view utf8_path);

// Creates `utf8_dir` and any missing parents. Succeeds if it already exists.
[[nodiscard]] expected make_directories(std::string_view utf8_dir);

// Deletes one file (never a directory). Succeeds if it is already gone.
[[nodiscard]] expected remove_file(std::string_view utf8_path);

// Deletes `utf8_dir` and everything in it, never following a link out of it.
// Only for the app's own folders (an add-on version, a download staging
// folder); never pointed at anything of the user's.
[[nodiscard]] expected remove_tree(std::string_view utf8_dir);

enum class rename_outcome : std::uint8_t { renamed, name_taken };

// Renames within one volume and never replaces: a taken `to` is
// rename_outcome::name_taken, not an error and not an overwrite.
[[nodiscard]] result<rename_outcome> rename_no_replace(std::string_view from_utf8,
                                                       std::string_view to_utf8);

enum class read_mode : std::uint8_t {
  sequential,  // the source read: the OS may read ahead
  uncached,    // the verify read-back: from the device, never the page cache
};

class file_reader {
 public:
  file_reader();
  ~file_reader();
  file_reader(file_reader&&) noexcept;
  file_reader& operator=(file_reader&&) noexcept;
  file_reader(const file_reader&) = delete;
  file_reader& operator=(const file_reader&) = delete;

  [[nodiscard]] expected open(std::string_view utf8_path, read_mode mode);
  // Fills up to `into.size()` bytes; 0 at end of file. For read_mode::uncached
  // `into` must be an aligned_buffer's memory (aligned, a kIoAlign multiple).
  [[nodiscard]] result<std::size_t> read(std::span<std::uint8_t> into);
  [[nodiscard]] std::uint64_t size() const noexcept;
  void close() noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

class file_writer {
 public:
  file_writer();
  ~file_writer();  // closes; does not delete (the caller owns cleanup)
  file_writer(file_writer&&) noexcept;
  file_writer& operator=(file_writer&&) noexcept;
  file_writer(const file_writer&) = delete;
  file_writer& operator=(const file_writer&) = delete;

  // Exclusive create: rename_outcome::name_taken if anything is at the path.
  [[nodiscard]] result<rename_outcome> create_new(std::string_view utf8_path);
  [[nodiscard]] expected write(std::span<const std::uint8_t> bytes);
  // To stable storage: FlushFileBuffers / F_FULLFSYNC (fsync where refused).
  [[nodiscard]] expected flush_durable();
  // Stamps the source's modification time so a later scan sees the same file.
  [[nodiscard]] expected set_mtime(std::int64_t mtime_unix);
  [[nodiscard]] expected close();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

struct tree_entry {
  std::string path_utf8;      // absolute
  std::string relative_utf8;  // from the walk root, '/'-separated on every OS
  std::string name_utf8;
  std::uint64_t size = 0;
  std::int64_t mtime_unix = 0;
};

// Every regular file under `utf8_root`, recursively, hidden and system entries
// (and macOS packages) skipped, sorted by relative path (byte order) so two
// walks of the same card list in the same order. `max_depth` bounds recursion
// (a card is shallow; a network share might not be). `visit` returning false
// stops the walk and the call returns status::cancelled.
[[nodiscard]] expected walk_files(std::string_view utf8_root, int max_depth,
                                  const std::function<bool(const tree_entry&)>& visit);

enum class entry_kind : std::uint8_t {
  file,       // a regular file
  directory,  // a real directory, walked into
  other,      // a link, junction, device, or a directory past max_depth: never followed
};

// Every entry under `utf8_root`, recursively, with nothing skipped: hidden and
// system entries included, links and junctions reported as entry_kind::other
// rather than followed or dropped. For checking that a folder holds exactly
// what a manifest lists (src/addon); walk_files is the one for a user's card.
// Unsorted. `visit` returning false stops the walk (status::cancelled).
[[nodiscard]] expected walk_all_entries(
    std::string_view utf8_root, int max_depth,
    const std::function<bool(std::string_view relative_slash, entry_kind kind)>& visit);

// Names of the immediate subdirectories of `utf8_dir` (hidden ones and links
// skipped), sorted byte-wise.
[[nodiscard]] result<std::vector<std::string>> child_directories(std::string_view utf8_dir);

// Parent directory and file name of a UTF-8 path, either separator.
[[nodiscard]] std::string_view parent_of(std::string_view utf8_path) noexcept;
[[nodiscard]] std::string_view file_name_of(std::string_view utf8_path) noexcept;
// `dir` + native separator + `name`, not doubling a trailing separator.
[[nodiscard]] std::string join_path(std::string_view dir, std::string_view name);
// '/'-separated relative path to native separators.
[[nodiscard]] std::string native_relative(std::string_view relative_slash);

}  // namespace mv::io
