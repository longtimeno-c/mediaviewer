// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #530: what a receive that stopped (the link lost, either machine asleep or
// off) leaves to resume from when the same sender sends again: the files
// that arrived whole and where, and the one file that was arriving, kept as
// its hidden temporary ("IMG_1.JPG.mvtmp") with how many of its bytes are on
// stable storage. A cancel or a decline keeps nothing, as before.
//
// One record per sender (its identity key, #501), at most kMaxResumeRecords,
// in the add-on's data folder (never beside the files, never off the
// machine: rule 6). A record unused for kResumeKeepS goes, and so do its
// temporaries. Written whole and crash-safe as prefs.h's file. Portable (D9):
// the file port only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "addons/driftwire/transfer.h"

namespace mv::driftwire {

inline constexpr const char* kResumeFile = "driftwire_resume.json";
inline constexpr std::int64_t kResumeKeepS = 7 * 24 * 60 * 60;
inline constexpr std::size_t kMaxResumeRecords = 8;

struct resume_file {
  // The file as the sender offered it: what a later offer must match.
  std::string rel;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  // Here: where it landed (whole), or will (partial).
  std::string final_path;
  std::int64_t final_mtime = 0;  // whole: the time it was stamped with
  std::string temp_path;         // partial: the kept temporary
  std::uint64_t have = 0;        // partial: its bytes on stable storage
  bool whole = false;
};

struct resume_record {
  std::string sender;  // the sender's identity, 64 hex digits
  std::string peer;    // its name then, for the window's list
  std::string dest;    // the folder it was received into
  std::string landed;  // a folder offer: the folder's name here ("Trip (2)")
  std::int64_t saved = 0;  // Unix seconds, when it was kept
  std::uint64_t count = 0, bytes = 0;  // the offer's
  std::vector<resume_file> files;      // the ones with something here
  [[nodiscard]] std::uint64_t bytes_here() const noexcept;
};

// Records at `path` (none when there is no file or it does not parse).
[[nodiscard]] std::vector<resume_record> load_resume(const std::string& path);
// Writes them (an empty list removes the file); false on any I/O error.
[[nodiscard]] bool save_resume(const std::string& path, const std::vector<resume_record>& records);
// Removes a record's kept temporaries (Discard, expiry, a later transfer
// from that sender that did not take them up).
void discard_partials(const resume_record& r);
// What of `offer`, received into `dest`, `r` already holds: files whole and
// still in place at their size and time, and a temporary still at least its
// kept bytes long. Nothing when the offer cannot say what its files are
// (a sender before #530: no "resume").
[[nodiscard]] resume_plan plan_resume(const resume_record& r, const incoming_offer& offer, const std::string& dest);
// The record a receive of `offer` holding `k` leaves (hooks::keep), with the
// partial `plan` was to take up if the receive has not reached it. Files
// whose time the sender did not say are left out (nothing could match them).
[[nodiscard]] resume_record record_of(const kept_state& k, const incoming_offer& offer, const resume_plan& plan,
                                      std::string sender, std::string peer, std::string dest, std::int64_t now);

}  // namespace mv::driftwire
