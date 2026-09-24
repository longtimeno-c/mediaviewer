// SPDX-License-Identifier: GPL-2.0-or-later
// import.db (plan/18 "The engine"): the library index, the per-card memory,
// {seq} counters, presets, card bindings, added folder sources, and the job
// journal that makes an import resumable.
//
// SQLite, one connection behind a mutex; every method is safe from any of the
// add-on's threads and none may be called from the UI thread. Hashes, paths
// and names stay in this file on this machine (rule 6).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "addons/import/model.h"
#include "core/result.h"

struct sqlite3;

namespace mv::import {

struct library_row {
  std::string root;  // destination root, native, no trailing separator
  std::string rel;   // '/'-separated
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  digest hash{};
};

struct card_row {
  digest hash{};
  std::int64_t imported_at = 0;  // 0: hashed but never imported
};

enum class member_state : std::uint8_t {
  pending = 0,
  done = 1,
  failed = 2,
  skipped = 3,
  cancelled = 4,
};

struct journal_row {
  std::uint64_t job = 0;
  std::uint32_t unit = 0;
  std::uint32_t member = 0;
  std::string src;
  std::string rel;               // source-relative, for the card memory
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  std::vector<std::string> targets;  // final paths, one per destination
  std::vector<std::string> target_roots;
  member_state state = member_state::pending;
  digest hash{};
  std::string reason;            // failure reason, or what a duplicate matched
  std::string display;           // the unit's display name (report / summary)
};

struct job_row {
  std::uint64_t id = 0;
  std::int64_t created = 0;
  std::int64_t finished = 0;
  std::uint32_t state = 0;  // mv_import_job_state
  std::string source_root;
  std::string volume_id;
  std::string label;
  std::string device_key;
  std::string preset_json;
  std::string summary_json;
  std::uint32_t kind = 0;  // 0 import, 1 verify-a-folder
};

class library_index {
 public:
  ~library_index();
  library_index(const library_index&) = delete;
  library_index& operator=(const library_index&) = delete;

  [[nodiscard]] static result<std::unique_ptr<library_index>> open(const std::string& db_path);

  // ---- library rows ----
  // Rows of `size`; `root` limits them to one destination (nullptr: library-wide).
  [[nodiscard]] std::vector<library_row> by_size(std::uint64_t size, const std::string* root);
  [[nodiscard]] std::optional<library_row> by_path(const std::string& root, const std::string& rel);
  void upsert(const library_row& row);
  void drop(const std::string& root, const std::string& rel);
  [[nodiscard]] std::vector<library_row> all_rows();

  // ---- the card memory ----
  [[nodiscard]] std::optional<card_row> card_lookup(const std::string& volume_id,
                                                    const std::string& rel, std::uint64_t size,
                                                    std::int64_t mtime);
  void card_record(const std::string& volume_id, const std::string& rel, std::uint64_t size,
                   std::int64_t mtime, const digest& hash, std::int64_t imported_at);

  // ---- {seq} ----
  [[nodiscard]] std::uint32_t last_seq(const std::string& root, const std::string& day);
  void commit_seq(const std::string& root, const std::string& day, std::uint32_t seq);

  // ---- presets, bindings, folder sources, settings ----
  [[nodiscard]] std::vector<std::string> preset_jsons();
  void save_preset(const std::string& name, const std::string& json);
  void delete_preset(const std::string& name);
  [[nodiscard]] std::optional<std::string> preset_json(const std::string& name);
  struct binding {
    std::string preset;
    bool auto_import = false;
  };
  [[nodiscard]] std::optional<binding> card_binding(const std::string& volume_id);
  void bind_card(const std::string& volume_id, const std::string& preset, bool auto_import);
  [[nodiscard]] std::vector<std::string> folder_sources();
  void add_folder_source(const std::string& path);
  void remove_folder_source(const std::string& path);
  [[nodiscard]] std::string setting(const std::string& key);
  void set_setting(const std::string& key, const std::string& value);

  // ---- jobs and the journal ----
  // `job.id` is the caller's (ids are handed out in memory, so the UI thread
  // never waits on this write); 0 on failure.
  [[nodiscard]] std::uint64_t create_job(const job_row& job);
  [[nodiscard]] std::uint64_t max_job_id();
  void set_job_state(std::uint64_t id, std::uint32_t state, std::int64_t finished,
                     const std::string& summary_json);
  [[nodiscard]] std::optional<job_row> job(std::uint64_t id);
  [[nodiscard]] std::vector<job_row> jobs(std::size_t limit);
  [[nodiscard]] std::vector<job_row> unfinished_jobs();
  void journal_add(const std::vector<journal_row>& rows);
  void journal_set(std::uint64_t job, std::uint32_t unit, std::uint32_t member, member_state state,
                   const digest* hash, const std::string& reason);
  [[nodiscard]] std::vector<journal_row> journal(std::uint64_t job);

 private:
  library_index() = default;
  [[nodiscard]] bool exec(const char* sql);

  std::mutex mutex_;
  sqlite3* db_ = nullptr;
};

}  // namespace mv::import
