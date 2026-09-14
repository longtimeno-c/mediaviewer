// SPDX-License-Identifier: GPL-2.0-or-later
// Copy / move / Recycle Bin for marks (plan/16 "Marks, copy, move").
// Portable header; Windows impl is file_ops_win.cpp (D9).
//
// I/O pool only — never the UI or render thread (plan/16 speed rule 2). None
// of these ever overwrites: a name that is taken becomes `name (2).ext`
// (collision_name.h), and a race that takes the name between the check and the
// write moves on to the next number rather than replacing the file. Nothing
// here logs a path (rule 6).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::io {

enum class transfer_kind : std::uint8_t {
  copy,
  // Same volume: a rename. Across volumes: copy, verify the size, then delete
  // the source. A copy that fails or comes up short is removed and the source
  // is left where it was. Moving a file into the folder it is already in is a
  // no-op that succeeds (unlike a copy, which lands beside it as `name (2)`).
  move,
};

// Copies or moves the file at `src_utf8` into the directory `dest_dir_utf8`,
// keeping its name unless taken. Returns the path it was written to. A copy
// never modifies the original (rule 5).
[[nodiscard]] result<std::string> transfer_file(std::string_view src_utf8,
                                                std::string_view dest_dir_utf8,
                                                transfer_kind kind) noexcept;

enum class recycle_outcome : std::uint8_t {
  recycled,
  // The location has no Recycle Bin (network share, some removable drives,
  // bin turned off). Nothing was deleted: the shell would have deleted it
  // permanently, and the user only agreed to the Recycle Bin.
  refused_no_recycle_bin,
};

// Sends the file to the Recycle Bin, never a permanent delete. The caller has
// already asked the user; this shows no UI of its own.
[[nodiscard]] result<recycle_outcome> recycle_file(std::string_view utf8_path) noexcept;

namespace detail {
// The rule recycle_file enforces, on the shell's per-item transfer flags (the
// Windows TSF_* word): a delete may go ahead only if it will go to the bin.
[[nodiscard]] bool pre_delete_allowed(std::uint32_t transfer_flags) noexcept;

// Test seam: runs the real progress sink's PreDeleteItem with `transfer_flags`
// and returns its HRESULT (0 = go ahead, E_ABORT = refused); `refused` is the
// sink's own record of it. No shell operation is performed.
[[nodiscard]] std::int32_t probe_recycle_sink(std::uint32_t transfer_flags, bool& refused) noexcept;

// What recycle_file reports once the operation has run. A refusal wins over
// everything: nothing was deleted, and the user is told why.
[[nodiscard]] inline result<recycle_outcome> recycle_outcome_from(bool sink_refused,
                                                                  bool performed_ok, bool aborted,
                                                                  bool still_exists) noexcept {
  if (sink_refused) return recycle_outcome::refused_no_recycle_bin;
  if (performed_ok && !aborted && !still_exists) return recycle_outcome::recycled;
  return err(status::io);
}
}  // namespace detail

}  // namespace mv::io
