// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's weights (#297, docs/plans/enhance.md E1): the model chosen for each
// kind, fetched through the shared downloader (src/addons/ai/fetch.h) when the
// person clicks Download, and nothing before.
//
// Where things are, under the add-on's data folder (which Remove deletes):
//   cache/models/<id>/<file>   the weights: the manifest's remote path
//                              "models/<id>/<file>" under cache/. The fetcher
//                              and prune both take cache/models as their root
//                              and the path without its "models/", so prune
//                              never reaches anything outside cache/models
//                              (<file>.part and <file>.ok sit beside each file)
//   cache/download.lock        one process at a time (lock.h), outside that root
//   enhance.json               {"schema":1,"models":{"still":"<id>",...}}: the
//                              choices, written whole to a temp file and renamed
//
// Requests. None in the constructor (it reads enhance.json and nothing else),
// none after a restart until the person clicks Resume: a chosen model that is
// not all here shows `stopped`. download() and resume() queue a kind for the
// one worker thread (started on the first of them, at background priority),
// which runs the kinds in the order they were queued. Before each run, and so
// before any GET: the 5 GB cap (`refused`), the model fitting this machine
// (choose.h offer(): `unfit`, or `no_space` with need / have), the lock
// (`other_window`) and a transport (`unsupported`). A finished run deletes
// what no kind's chosen model needs (here, or named by enhance.json on disk
// for another window). Within a run the fetcher waits out an outage by itself
// (`waiting`); a 404 is `gone`, three wrong hashes `failed` (the file deleted).
//
// Threads. Every call is safe from any thread and never waits on the network;
// status() and ready() stat the chosen models' files. `changed` is called on
// the caller's thread or the worker, never under the store's lock, on every
// change of state and at most about four times a second while bytes move; it
// must not destroy the store. The destructor stops the run (the fetcher sees
// the stop between chunks) and joins. Nothing here logs: no path, no file name.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/enhance/catalog.h"
#include "addons/enhance/choose.h"
#include "addons/enhance/machine.h"
#include "core/result.h"

namespace mv::enhance {

enum class download_state : std::uint8_t {
  none,          // no model chosen for the kind
  ready,         // every file here and verified
  queued,        // clicked; another kind's run goes first
  downloading,
  waiting,       // Hugging Face did not answer: trying again by itself (retry_in_s)
  stopped,       // cancelled, cut off by a restart or a disk error: Resume
  gone,          // a pinned file is no longer on Hugging Face (404 / 410)
  failed,        // a file missed its SHA-256 three times; it was deleted
  refused,       // a file over the 5 GB cap: never requested
  no_space,      // need / have: bytes still to fetch plus cache_bytes, and free
  other_window,  // another MediaViewer window holds the download lock
  unsupported,   // no transport on this platform
  unfit,         // the model does not fit this machine (reason, need / have)
};

// "none", "ready", ... (tests, diagnostics).
[[nodiscard]] const char* state_id(download_state s) noexcept;

struct kind_status {
  kind k = kind::still;
  std::string model;  // the chosen model's id; empty: none
  download_state state = download_state::none;
  std::uint64_t done_bytes = 0;   // here (verified, partial or dropped in)
  std::uint64_t total_bytes = 0;  // the chosen model's download_bytes()
  double retry_in_s = 0;          // waiting
  std::uint64_t need = 0;         // no_space, unfit (memory): bytes
  std::uint64_t have = 0;
  why reason = why::fits;  // unfit, refused, no_space
};

struct store_options {
  // The fetcher's backoff and wait (tests: a wait that returns at once).
  // max_file_bytes is always kMaxWeightsFile.
  ai::fetch_options fetch;
  bool background = true;  // enter_background() at the top of the worker
};

class model_store {
 public:
  using transport_factory = std::function<std::unique_ptr<ai::http_transport>()>;

  // `data_dir`: the add-on's data folder (UTF-8). `transport`: made for each
  // run, after the gates (ai::make_http_transport). `probe`: this machine, for
  // the fit and the disk gate (probe(data_dir) in the add-on).
  model_store(std::string data_dir_utf8, catalog models, transport_factory transport,
              std::function<machine()> probe, std::function<void()> changed, store_options options = {});
  ~model_store();
  model_store(const model_store&) = delete;
  model_store& operator=(const model_store&) = delete;

  // The click: `model_id` becomes the kind's choice (saved) and the kind is
  // queued; a run of the kind's previous choice stops. status::invalid_arg for
  // a model the catalogue does not list for the kind; status::io when the
  // choice could not be saved (the download still runs).
  expected download(kind k, const std::string& model_id);
  void cancel(kind k);
  // Queues the chosen model again (after stopped, gone, failed, a restart...).
  void resume(kind k);
  // Forgets the kind's choice, stops its run and deletes its files (on the
  // worker, once nothing is running).
  void remove(kind k);

  // One per kind, in kKinds order.
  [[nodiscard]] std::vector<kind_status> status() const;
  // The chosen model's every file is here and verified.
  [[nodiscard]] bool ready(kind k) const;
  [[nodiscard]] std::string chosen(kind k) const;
  // The model's bytes not here yet: what the disk gate counts.
  [[nodiscard]] std::uint64_t still_to_fetch(const model& m) const;
  [[nodiscard]] const catalog& models() const noexcept { return catalog_; }

  // Until nothing is queued, running or waiting to be pruned (tests; a
  // host that wants to finish first). False on timeout.
  bool wait_idle(std::chrono::milliseconds timeout);

  [[nodiscard]] std::string weights_root() const { return data_ + "/cache/models"; }
  [[nodiscard]] std::string lock_path() const { return data_ + "/cache/download.lock"; }
  [[nodiscard]] std::string choices_path() const { return data_ + "/enhance.json"; }
  // A model's files as the fetcher takes them: under weights_root(), the
  // remote path without its leading "models/".
  [[nodiscard]] static std::vector<ai::remote_file> remote_files(const model& m);

 private:
  struct slot {
    std::string chosen;
    bool queued = false;
    bool running = false;
    bool cancelled = false;  // cancel() or remove() during the run
    std::string running_model;
    download_state live = download_state::queued;  // while running
    download_state outcome = download_state::none;  // how the last run here ended
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    double retry_in_s = 0;
    std::uint64_t need = 0;
    std::uint64_t have = 0;
    why reason = why::fits;
  };
  struct run_end {
    download_state state = download_state::stopped;
    std::uint64_t need = 0;
    std::uint64_t have = 0;
    why reason = why::fits;
  };

  void work();
  run_end run_one(kind k, const std::string& id);
  void on_progress(kind k, const ai::fetch_progress& p);
  void prune_unchosen(bool lock_held);
  void start_worker_locked();
  [[nodiscard]] std::string choices_json_locked() const;
  bool save(const std::string& text, std::uint64_t gen);
  [[nodiscard]] std::uint64_t here_bytes(const model& m) const;
  [[nodiscard]] bool all_ready(const model& m) const;
  [[nodiscard]] bool idle_locked() const noexcept;
  void tell() const;

  std::string data_;
  catalog catalog_;
  transport_factory transport_;
  std::function<machine()> probe_;
  std::function<void()> changed_;
  store_options opt_;

  mutable std::mutex m_;
  std::condition_variable work_cv_;
  std::condition_variable idle_cv_;
  std::array<slot, 3> slots_;
  std::deque<kind> queue_;
  bool prune_pending_ = false;
  bool pruning_ = false;
  bool quit_ = false;
  std::uint64_t save_gen_ = 0;  // under m_: the newest choices snapshot
  std::chrono::steady_clock::time_point last_tell_{};
  std::atomic<bool> run_stop_{false};
  std::thread worker_;

  std::mutex save_m_;
  std::uint64_t saved_gen_ = 0;  // under save_m_: the snapshot on disk
};

}  // namespace mv::enhance
