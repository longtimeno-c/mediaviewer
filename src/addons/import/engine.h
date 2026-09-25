// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Import engine (plan/18-import.md): scans, plans and jobs, over the host
// function table and import.db. Behind mv_import_api (addon_entry.cpp).
//
// Threads (rule 1: never the UI or render thread): one control thread runs
// scans, plans and card arrivals in order; each job runs on its own thread.
// A per-device lock gives one reader per physical source and one writer per
// physical destination (plan/18 "Throughput"). Every public method returns at
// once unless marked [worker].
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <mediaviewer/mediaviewer_import.h>

#include "addons/import/host.h"
#include "addons/import/library_index.h"
#include "addons/import/model.h"
#include "core/result.h"

namespace mv::import {

class engine {
 public:
  explicit engine(const mv_host_api* api);
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  // Opens import.db in the host's data folder (or `db_path_override`, tests),
  // marks jobs a crash left running as interrupted, starts the volume watch.
  [[nodiscard]] expected start(const std::string& db_path_override = {});
  // Cancels every job, joins every thread. Idempotent.
  void shutdown() noexcept;

  // ---- sources ----
  [[nodiscard]] std::string sources_json();
  [[nodiscard]] expected add_folder_source(const std::string& dir);
  [[nodiscard]] expected remove_folder_source(const std::string& dir);
  [[nodiscard]] result<std::string> arrival_root(std::uint64_t seq);
  void on_volume(std::uint32_t event, const std::string& root);  // watcher thread

  // ---- scan / plan ----
  [[nodiscard]] result<std::uint64_t> scan(const std::string& root);
  [[nodiscard]] result<std::uint64_t> scan_files(const std::vector<std::string>& paths);
  [[nodiscard]] result<std::uint64_t> plan(std::uint64_t scan_id, const std::string* preset_json,
                                           const std::vector<std::string>* marked);
  [[nodiscard]] result<std::string> plan_json(std::uint64_t plan_id);
  [[nodiscard]] expected select(std::uint64_t plan_id, int unit, const std::string* day,
                                bool selected);
  [[nodiscard]] result<std::string> thumbnail(std::uint64_t plan_id, std::uint32_t unit);  // [worker]

  // ---- jobs ----
  [[nodiscard]] result<std::uint64_t> start_job(std::uint64_t plan_id);
  [[nodiscard]] result<std::uint64_t> import_now(const std::vector<std::string>& paths);
  [[nodiscard]] expected pause(std::uint64_t job, bool paused);
  [[nodiscard]] expected cancel(std::uint64_t job);
  [[nodiscard]] expected set_priority(std::uint64_t job, bool fast);
  [[nodiscard]] expected progress(std::uint64_t job, mv_import_progress& out);
  [[nodiscard]] result<std::string> summary_json(std::uint64_t job);
  [[nodiscard]] result<std::uint64_t> retry_failed(std::uint64_t job);
  [[nodiscard]] std::string unfinished_json();
  [[nodiscard]] expected resume(std::uint64_t job);
  [[nodiscard]] result<std::string> report_path(std::uint64_t job);
  [[nodiscard]] expected eject(const std::string& root);

  // ---- presets ----
  [[nodiscard]] std::string presets_json();
  [[nodiscard]] expected save_preset(const std::string& json);
  [[nodiscard]] expected delete_preset(const std::string& name);
  [[nodiscard]] expected bind_card(const std::string& volume_id, const std::string& preset,
                                   bool auto_import);
  [[nodiscard]] result<std::string> preview_names_json(const std::string& preset_json);

  // ---- library tools ----
  [[nodiscard]] std::string history_json();
  [[nodiscard]] result<std::uint64_t> verify_folder(const std::string& dir);

  // Tests: wait until the control queue and every job are idle.
  void wait_idle();

 private:
  struct scan_slot;
  struct plan_slot;
  struct job;

  void control_loop();
  void post_task(std::function<void()> fn);
  // In memory only: safe on the UI thread. persist() writes its row.
  std::shared_ptr<job> make_job(std::uint32_t kind, const preset& settings);
  void persist(const std::shared_ptr<job>& j);
  void fill_job_from_plan(const std::shared_ptr<job>& j, const scan_result& scan,
                          const plan_result& plan);
  void launch(const std::shared_ptr<job>& j);
  void run_job(const std::shared_ptr<job>& j);
  void run_verify(const std::shared_ptr<job>& j, const std::string& dir);
  void finish_job(const std::shared_ptr<job>& j, std::uint32_t state);
  void resume_on_control(std::uint64_t id);
  // Scan + plan + start, on the control thread, into an already-made job.
  void pipeline_into(const std::shared_ptr<job>& j, const std::vector<std::string>& paths,
                     const std::string& root, const preset& settings);
  [[nodiscard]] std::string make_summary(job& j, std::uint32_t state);
  [[nodiscard]] preset last_preset();
  [[nodiscard]] std::string default_library() const;
  std::mutex& device_lock(const std::string& key);
  std::shared_ptr<job> find_job(std::uint64_t id);
  void progress_tick(job& j, std::uint64_t delta);

  host host_;
  std::unique_ptr<library_index> idx_;
  std::string data_dir_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool busy_ = false;
  bool stopping_ = false;
  std::thread control_;

  std::uint64_t next_scan_ = 1;
  std::uint64_t next_plan_ = 1;
  std::uint64_t next_job_ = 1;
  std::uint64_t next_arrival_ = 1;
  std::map<std::uint64_t, std::shared_ptr<scan_slot>> scans_;
  std::map<std::uint64_t, std::shared_ptr<plan_slot>> plans_;
  std::map<std::uint64_t, std::shared_ptr<job>> jobs_;
  std::map<std::uint64_t, std::string> arrivals_;
  std::map<std::string, std::int64_t> new_counts_;  // root -> units not imported before
  std::map<std::string, double> rates_;             // device key -> measured bytes/s
  std::map<std::string, std::unique_ptr<std::mutex>> device_locks_;
  std::atomic<bool> cancel_control_{false};
  bool watching_ = false;
};

}  // namespace mv::import
