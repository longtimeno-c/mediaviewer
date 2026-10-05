// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Verified copy (docs/design/18-import.md "Verify"; the base app's F8 across volumes).
//
// One read of the source feeds every destination (Import's backup copy is
// written from the same read). The source is hashed while it is read; each
// destination is written to a sibling temporary, flushed to stable storage,
// read back uncached, hashed, compared, and only then renamed into place
// without replacing anything. A mismatch deletes the temporary and retries
// once, re-reading the source; a second failure is reported, never counted as
// done. Cancel leaves no temporary behind.
//
// Reads and writes overlap: the calling thread reads into 2-4 large aligned
// buffers while one writer thread per destination drains them. Portable over
// io/file_port.h (D9). Worker threads only (rule 1); no path is logged (rule 6).
//
// A network share is the exception (copy_profile_for): every request there is
// a round trip, so one at a time leaves a 10 GbE link mostly idle. With
// read_depth / write_depth above 1, several reader threads fetch chunks at
// their offsets, the calling thread hashes them in order, and several writer
// threads per destination write them at their offsets; the read-back is
// pipelined the same way. A card is never read that way (docs/design/18: no parallel
// reads of a card), and cards and local disks keep the sequential path.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "io/content_hash.h"
#include "io/file_port.h"

namespace mv::io {

enum class copy_target_outcome : std::uint8_t {
  verified = 0,          // read back from the device and matched
  written_unverified,    // copy_options::read_back off (network targets): hash-on-read only
  name_taken,            // the final name exists; nothing was written there
  write_failed,          // create / write / flush failed (twice)
  verify_failed,         // read-back hash differed (twice)
  cancelled,
};

[[nodiscard]] constexpr bool copy_succeeded(copy_target_outcome o) noexcept {
  return o == copy_target_outcome::verified || o == copy_target_outcome::written_unverified;
}

// Fault injection for tests and the PR 16 verify line ("fault injection in
// the writer is detected, retried, and reported"): flips one bit of the byte
// at `offset` in what is written to `target`, `times` times across attempts.
struct copy_fault {
  int target = -1;
  std::uint64_t offset = 0;
  int times = 0;
};

struct copy_options {
  bool read_back = true;                    // full read-back verify
  std::size_t buffer_bytes = 4u << 20;      // per buffer, rounded to kIoAlign
  unsigned buffers_in_flight = 3;           // clamped to 2..4
  bool keep_mtime = true;                   // destination keeps the source's mtime
  int retries = 1;                          // docs/design/18: retry once, then fail the file
  // Requests in flight per file: reads of the source (and of each read-back),
  // and writes to each destination. 1 is the sequential path. Set from
  // copy_profile_for(); clamped to kMaxIoDepth.
  unsigned read_depth = 1;
  unsigned write_depth = 1;
  const std::atomic<bool>* cancel = nullptr;
  // Called on the reading thread with the bytes just read (the ETA's input).
  std::function<void(std::uint64_t delta_bytes)> on_progress;
  // Called between buffers. Background priority blocks in here while the
  // present loop needs the CPU or the disk.
  std::function<void()> yield;
  copy_fault* fault = nullptr;
};

struct copy_target_result {
  std::string path_utf8;
  copy_target_outcome outcome = copy_target_outcome::write_failed;
  bool retried = false;
};

struct copy_outcome {
  content_hash source_hash;
  std::uint64_t bytes = 0;
  std::vector<copy_target_result> targets;
  // The source read differently on the retry: the card itself is suspect.
  bool source_unstable = false;
};

// The I/O shape of one copy, from where its two ends live (docs/design/18
// "Throughput", network profile; docs/design/12 2026-10-01).
inline constexpr unsigned kMaxIoDepth = 16;
inline constexpr unsigned kNetworkIoDepth = 8;
inline constexpr std::size_t kNetworkChunkBytes = 2u << 20;
inline constexpr unsigned kNetworkFilesInFlight = 4;
// Budget: (depth + 2) chunks per file, kNetworkFilesInFlight files: 80 MiB at
// the network profile, inside this cap.
inline constexpr std::size_t kCopyBufferBudget = 128u << 20;

struct copy_profile {
  unsigned read_depth = 1;
  unsigned write_depth = 1;
  std::size_t buffer_bytes = 4u << 20;
  // Files a batch copies at once (io/in_flight.h). Only batch_copy_profile
  // raises it: one file at a time is the right shape for a card.
  unsigned files_in_flight = 1;

  void apply(copy_options& o) const noexcept {
    o.read_depth = read_depth;
    o.write_depth = write_depth;
    o.buffer_bytes = buffer_bytes;
  }
};

// Per file and cheap (io/volume.h is_network_path on each end, no device
// query): deep reads when the source is on a share, deep writes when a
// destination is.
[[nodiscard]] copy_profile copy_profile_for(std::string_view src_utf8,
                                            std::string_view dest_utf8) noexcept;

// For a batch from one folder to another (F8, Transfer): copy_profile_for,
// plus several files at once when either end is a share and the source is
// not a removable card. Asks volume_of(src) once, so call it once per batch.
[[nodiscard]] copy_profile batch_copy_profile(std::string_view src_dir_utf8,
                                              std::string_view dest_dir_utf8);

// `targets` are final paths whose parent directories exist. Returns an error
// only when the source cannot be read at all (status::io) or on cancel
// (status::cancelled); per-destination failures are in the outcome.
[[nodiscard]] result<copy_outcome> verified_copy(std::string_view src_utf8,
                                                 std::span<const std::string> targets,
                                                 const copy_options& options);

// Hashes a whole file. read_mode::uncached for a read-back or verify-a-folder.
[[nodiscard]] result<content_hash> hash_file(std::string_view utf8_path, read_mode mode,
                                             const std::atomic<bool>* cancel = nullptr,
                                             const std::function<void()>& yield = {},
                                             const std::function<void(std::uint64_t)>& on_progress = {},
                                             unsigned read_depth = 1,
                                             std::size_t buffer_bytes = 4u << 20);

// The temporary a destination is written to before it is renamed into place:
// "<final>.mvtmp", then "<final>.mvtmp2" … if one is left over. Exposed so the
// job journal can sweep them after a crash.
[[nodiscard]] std::string temp_name_for(std::string_view final_utf8, int attempt);
inline constexpr int kTempAttempts = 16;

}  // namespace mv::io
