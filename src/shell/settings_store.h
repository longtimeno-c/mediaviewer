// SPDX-License-Identifier: GPL-2.0-or-later
// settings.ini as an in-memory document, persisted off the UI thread.
//
// Rule 1: nothing that can block touches the UI or render thread. A settings
// write is small, but it is a file write (plan/12, "Settings writes on the UI
// thread", closed in PR 8). So:
//
//   * The whole file is read once, at startup, into a settings_doc.
//   * Callers on any thread read and mutate that in-memory document. Every
//     mutation publishes a new immutable snapshot (read-copy-update through
//     atomics; there is no mutex for a caller to wait on).
//   * The newest snapshot is handed to one persist worker (mv::job_system, one
//     thread). Bursts coalesce: a snapshot the worker has not taken yet is
//     replaced, so ten toggles in a row are one write of the last state.
//   * The worker writes the whole file to a temp file, flushes it, and renames
//     it over settings.ini. A crash mid-write leaves the old file, never half
//     of a new one.
//   * The exit path (after WM_CLOSE's island teardown) calls flush() with a
//     bounded wait. Exit may wait briefly; the render loop never does.
//
// Adding a persisted key is one get and one set on app_settings():
//
//   auto& s = mv::shell::app_settings();
//   bool on = s.get_int("update", "auto_check", 1) != 0;   // no disk
//   s.set_int("update", "auto_check", on ? 0 : 1);          // queued write
//
// Do not call GetPrivateProfile*/WritePrivateProfile* on settings.ini from
// anywhere else: the store rewrites the whole file from its document, so a
// write that bypasses it is lost at the next persist.
//
// Windows host only (D9): the store and its file live in shell/. The document
// model and the INI text codec are plain C++ and have no Win32 in the header.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"

namespace mv::shell {

#if defined(__APPLE__)
// Apple's libc++ has no std::atomic<std::shared_ptr<T>> (P0718). Same subset of
// the API the store uses, over the shared_ptr atomic free functions.
template <class T>
class atomic_shared_ptr {
 public:
  std::shared_ptr<T> load() const { return std::atomic_load(&p_); }
  void store(std::shared_ptr<T> v) { std::atomic_store(&p_, std::move(v)); }
  std::shared_ptr<T> exchange(std::shared_ptr<T> v) {
    return std::atomic_exchange(&p_, std::move(v));
  }
  bool compare_exchange_strong(std::shared_ptr<T>& expected, std::shared_ptr<T> desired) {
    return std::atomic_compare_exchange_strong(&p_, &expected, std::move(desired));
  }
  bool compare_exchange_weak(std::shared_ptr<T>& expected, std::shared_ptr<T> desired) {
    return std::atomic_compare_exchange_weak(&p_, &expected, std::move(desired));
  }

 private:
  std::shared_ptr<T> p_;
};
template <class T>
using atomic_sp = atomic_shared_ptr<T>;
#else
template <class T>
using atomic_sp = std::atomic<std::shared_ptr<T>>;
#endif

// Ordered sections of ordered key = value lines. Section and key names match
// ASCII-case-insensitively, as the Win32 profile API always did. Values are
// UTF-8. Unknown sections and keys survive a load/persist round trip.
struct settings_doc {
  struct entry {
    std::string key;
    std::string value;
    bool operator==(const entry&) const = default;
  };
  struct section {
    std::string name;
    std::vector<entry> entries;
    bool operator==(const section&) const = default;
  };
  std::vector<section> sections;
  bool operator==(const settings_doc&) const = default;

  [[nodiscard]] const std::string* find(std::string_view section, std::string_view key) const noexcept;
  void set(std::string_view section, std::string_view key, std::string_view value);
  bool erase(std::string_view section, std::string_view key) noexcept;
  // Removes every key in `section` whose name starts with `prefix` (all keys
  // for an empty prefix). The section header goes when it is empty.
  void erase_prefix(std::string_view section, std::string_view prefix) noexcept;

  [[nodiscard]] std::string get(std::string_view section, std::string_view key,
                                std::string_view fallback = {}) const;
  [[nodiscard]] int get_int(std::string_view section, std::string_view key, int fallback) const noexcept;
};

// Pure. Tolerant: blank lines, ';' / '#' comments, whitespace around names and
// values; a key before any section header is dropped.
[[nodiscard]] settings_doc parse_settings_ini(std::string_view utf8_text);
// Pure. "[section]\r\nkey=value\r\n" with a blank line between sections.
[[nodiscard]] std::string serialize_settings_ini(const settings_doc& doc);

class settings_store {
 public:
  // `path` is the UTF-16 file path; empty means "no persistence this run"
  // (the store still works in memory).
  explicit settings_store(std::wstring path = {}) noexcept;
  ~settings_store();

  settings_store(const settings_store&) = delete;
  settings_store& operator=(const settings_store&) = delete;

  // [startup] Reads the file synchronously. Call before the window exists;
  // this is the only disk read the store does.
  void load_from_disk() noexcept;

  // Starts the persist worker. Mutations made before start() are persisted by
  // the first job after it. False if the worker could not start; then flush()
  // writes on the exit path instead.
  [[nodiscard]] bool start() noexcept;

  // [any-thread, no disk, no lock] The current document.
  [[nodiscard]] std::shared_ptr<const settings_doc> snapshot() const noexcept;
  [[nodiscard]] std::string get(std::string_view section, std::string_view key,
                                std::string_view fallback = {}) const;
  [[nodiscard]] int get_int(std::string_view section, std::string_view key, int fallback) const noexcept;

  // [any-thread, no disk] Mutate the document and queue a coalesced persist.
  // A mutation that changes nothing queues nothing.
  void set(std::string_view section, std::string_view key, std::string_view value) noexcept;
  void set_int(std::string_view section, std::string_view key, int value) noexcept;
  void erase(std::string_view section, std::string_view key) noexcept;
  // Several keys as one snapshot (and at most one write).
  void update(const std::function<void(settings_doc&)>& mutate) noexcept;

  // [exit path only] Waits up to `timeout_ms` for the newest snapshot to reach
  // disk. Retries once if the last write failed. Without a running worker it
  // writes on the calling thread. True when disk matches memory.
  bool flush(unsigned timeout_ms) noexcept;

  // Joins the worker. Call flush() first; stop() drops a snapshot the worker
  // has not started.
  void stop() noexcept;

  // Diagnostics and test hooks.
  [[nodiscard]] std::uint64_t writes_ok() const noexcept { return writes_ok_.load(); }
  [[nodiscard]] std::uint64_t writes_failed() const noexcept { return writes_failed_.load(); }
  [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

 private:
  struct version {
    settings_doc doc;
    std::uint64_t seq = 0;
  };

  void publish(std::shared_ptr<const version> v) noexcept;
  void schedule() noexcept;
  void persist_pending() noexcept;  // worker body

  std::wstring path_;
  atomic_sp<const version> current_;
  atomic_sp<const version> pending_;
  std::atomic<bool> job_queued_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> settled_seq_{0};      // newest seq the worker attempted
  std::atomic<std::uint64_t> failed_seq_{0};       // newest seq whose write failed
  std::atomic<std::uint64_t> writes_ok_{0};
  std::atomic<std::uint64_t> writes_failed_{0};
  void* settled_event_ = nullptr;                   // HANDLE, manual-reset
  mv::job_system pool_;
};

// The process-wide store for %LocalAppData%\MediaViewer\settings.ini. Loaded
// from disk on first use (startup, on the main thread, before the window).
[[nodiscard]] settings_store& app_settings() noexcept;

// Atomic whole-file write: temp file, FlushFileBuffers, MoveFileEx over the
// target with write-through. The target is untouched on failure.
[[nodiscard]] bool write_settings_file_atomic(const std::wstring& path, std::string_view utf8_text) noexcept;

// Rule 1 detector. The host registers its UI thread once; any settings file
// write on that thread outside exit_write_scope is counted (and asserts in a
// debug build). Tests read the count.
void register_settings_ui_thread() noexcept;
[[nodiscard]] std::uint64_t settings_ui_thread_writes() noexcept;
void reset_settings_ui_thread_detector() noexcept;  // tests

class settings_exit_write_scope {
 public:
  settings_exit_write_scope() noexcept;
  ~settings_exit_write_scope();
  settings_exit_write_scope(const settings_exit_write_scope&) = delete;
  settings_exit_write_scope& operator=(const settings_exit_write_scope&) = delete;
};

}  // namespace mv::shell
