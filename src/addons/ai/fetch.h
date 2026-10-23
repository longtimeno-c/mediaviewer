// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model downloader (#482 T3, docs/plans/model-tiers.md). The add-on's
// signed manifest lists the upstream files it does not carry (`remote`: URL,
// path, size, SHA-256); this fetches the ones a tier needs into the add-on's
// data folder, from Hugging Face at a pinned revision.
//
// - A plain GET of a fixed URL: no token, identifier, query or cookies
//   (the transports, fetch_mac.mm / fetch_win.cpp, send nothing else).
// - Resumable: bytes land in <path>.part, and the next try asks for the rest
//   with a Range request, including after a restart.
// - Patient: no answer, a 5xx or a 429 waits and retries by itself, backing
//   off to max_backoff_s; the state says so (fetch_state::waiting). Only a
//   404 / 410 on the pinned revision is final (fetch_state::gone): that file
//   needs a new pin in an app update.
// - Verified: a finished file is hashed and kept only if it matches; one that
//   does not is deleted and fetched again, never loaded. A verified file gets a
//   <path>.ok record (sha256, size, mtime), so later starts stat it rather
//   than hash gigabytes.
// - Capped: with fetch_options::max_file_bytes set, a list naming any file
//   over it is refused whole (status::invalid_arg) before a request is made
//   or a folder created (Enhance: 5 GB a weights file).
// - Adopting: a file already at its path with the pinned size but no .ok that
//   agrees (dropped there by hand, or copied back with a new mtime) is hashed
//   once, kept with a .ok when it matches and deleted and fetched when not.
//
// Transport-free and blocking: the engine runs it on a thread of its own;
// tests drive it with a fake transport. Nothing here knows about tiers or
// models: the AI pack and Enhance (#297, docs/plans/enhance.md) both link it
// as mv_model_fetch (cmake/ai.cmake), so Enhance does not link the AI engine.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::ai {

struct remote_file {
  std::string url;
  std::string path;    // relative to the fetcher's root, '/'-separated
  std::string sha256;  // lowercase hex
  std::uint64_t size = 0;
};

struct http_reply {
  int status = 0;     // the final response's HTTP status; 0 = no response at all
  std::string error;  // the transport's own words when there was none
};

class http_transport {
 public:
  virtual ~http_transport() = default;
  // GET `url`, from byte `from` (a Range request when > 0). `head` gets the
  // final status before any body; false stops there. `body` gets each chunk
  // of a 200 / 206; false stops the transfer. Blocks until done or stopped.
  virtual http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int status)>& head,
                         const std::function<bool(std::span<const std::uint8_t>)>& body) = 0;
};

// The platform's transport (NSURLSession on the Mac, WinHTTP on Windows); null
// where there is none.
[[nodiscard]] std::unique_ptr<http_transport> make_http_transport();

enum class fetch_state : std::uint32_t {
  idle = 0,
  downloading = 1,
  waiting = 2,  // Hugging Face did not answer: retrying by itself
  gone = 3,     // a pinned file is gone (404 / 410)
  failed = 4,   // a file never matched its SHA-256
  done = 5,
};

struct fetch_progress {
  fetch_state state = fetch_state::idle;
  std::uint64_t done_bytes = 0;   // of every file in the run, verified or partial
  std::uint64_t total_bytes = 0;
  std::uint32_t files_done = 0;
  std::uint32_t files_total = 0;
  std::string file;          // the file being fetched (its manifest path)
  double retry_in_s = 0;     // waiting: the next try
};

struct fetch_options {
  double first_backoff_s = 2;
  double max_backoff_s = 300;
  int hash_attempts = 3;  // fetches of one file that may miss its hash before it fails
  std::uint64_t max_file_bytes = 0;  // 0 = none (Enhance: 5 GB a weights file)
  // Waits up to `seconds` or until `stop`; true if it waited the whole time.
  // Tests replace it.
  std::function<bool(double seconds, const std::atomic<bool>& stop)> wait;
};

class fetcher {
 public:
  fetcher(std::string root_utf8, http_transport& net, fetch_options options = {});

  [[nodiscard]] std::string local_path(const remote_file& f) const;
  // Here and verified: the right size, and a .ok record that agrees.
  [[nodiscard]] bool ready(const remote_file& f) const;
  // Every file, in order. ok when all are ready; status::invalid_arg, with
  // nothing requested, made or reported, when one is over max_file_bytes;
  // status::cancelled when `stop` was set (partial files are kept for the
  // next run); status::not_found when one is gone; status::corrupt when one
  // never matched its hash. `progress` is called on each change of state and
  // about every quarter second while bytes arrive.
  [[nodiscard]] expected run(std::span<const remote_file> files, const std::atomic<bool>& stop,
                             const std::function<void(const fetch_progress&)>& progress);
  // Deletes what is under the root and not one of `keep` (the models of a
  // tier left behind, once the new one answers).
  void prune(std::span<const remote_file> keep) const;

 private:
  std::string root_;
  http_transport& net_;
  fetch_options opt_;
};

// `f` is under `root`, complete and verified (fetcher::ready, without a fetcher).
[[nodiscard]] bool fetch_ready(const std::string& root_utf8, const remote_file& f);
// Deletes what is under `root` and not one of `keep` (fetcher::prune).
void prune_fetched(const std::string& root_utf8, std::span<const remote_file> keep);

}  // namespace mv::ai
