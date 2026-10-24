// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/fetch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "addons/ai/sha256.h"

namespace mv::ai {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::uint64_t size_of(const std::string& path) {
  std::error_code ec;
  const auto n = fs::file_size(fs_path(path), ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

long long mtime_of(const std::string& path) {
  std::error_code ec;
  const auto t = fs::last_write_time(fs_path(path), ec);
  return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

void remove_file(const std::string& path) {
  std::error_code ec;
  fs::remove(fs_path(path), ec);
}

bool default_wait(double seconds, const std::atomic<bool>& stop) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < until) {
    if (stop.load()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !stop.load();
}

// "<sha256> <size> <mtime>": what was verified, so a later start can stat.
std::string ok_record(const remote_file& f, long long mtime) {
  return f.sha256 + " " + std::to_string(f.size) + " " + std::to_string(mtime) + "\n";
}

// Records `path` as verified against `f` (its .ok, as of its mtime now).
bool write_ok(const remote_file& f, const std::string& path) {
  std::ofstream ok(fs_path(path + ".ok"), std::ios::binary | std::ios::trunc);
  ok << ok_record(f, mtime_of(path));
  ok.close();
  return !ok.fail();
}

// A regular file (not a link or folder) of exactly `size` bytes at `path`.
bool sits_at(const std::string& path, std::uint64_t size) {
  std::error_code ec;
  return fs::is_regular_file(fs::symlink_status(fs_path(path), ec)) && size_of(path) == size;
}

// NTFS and APFS are case-insensitive unless made otherwise, so there a file
// spelt like a kept path but for case IS that file (the pinned spelling
// opens it). On a volume made case-sensitive such a file is kept too: a
// leftover, never a wrong delete.
#if defined(_WIN32) || defined(__APPLE__)
constexpr bool kFoldCase = true;
#else
constexpr bool kFoldCase = false;
#endif

// fetch_path_key, of a path already in hand.
std::string key_of(const fs::path& path, bool fold) {
  const std::u8string g = path.lexically_normal().generic_u8string();
  std::string key(g.begin(), g.end());
  if (fold) {
    // ASCII only: every byte of a multi-byte UTF-8 sequence is over 0x7F.
    for (char& c : key) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return key;
}

}  // namespace

fetcher::fetcher(std::string root, http_transport& net, fetch_options options)
    : root_(std::move(root)), net_(net), opt_(std::move(options)) {
  if (!opt_.wait) opt_.wait = default_wait;
}

std::string fetcher::local_path(const remote_file& f) const {
  if (root_.empty()) return f.path;
  const char last = root_.back();
  return (last == '/' || last == '\\') ? root_ + f.path : root_ + "/" + f.path;
}

bool fetcher::ready(const remote_file& f) const {
  const std::string path = local_path(f);
  if (size_of(path) != f.size) return false;
  std::ifstream in(fs_path(path + ".ok"), std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line)) return false;
  return line + "\n" == ok_record(f, mtime_of(path));
}

expected fetcher::run(std::span<const remote_file> files, const std::atomic<bool>& stop,
                      const std::function<void(const fetch_progress&)>& progress) {
  // Over the cap: the whole list is refused before anything is asked or made.
  if (opt_.max_file_bytes) {
    for (const remote_file& f : files) {
      if (f.size > opt_.max_file_bytes) return err(status::invalid_arg);
    }
  }
  fetch_progress p;
  p.files_total = static_cast<std::uint32_t>(files.size());
  for (const remote_file& f : files) p.total_bytes += f.size;
  std::uint64_t finished_bytes = 0;  // of files already verified
  auto last_report = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  const auto report = [&](bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_report < std::chrono::milliseconds(250)) return;
    last_report = now;
    if (progress) progress(p);
  };

  for (const remote_file& f : files) {
    p.file = f.path;
    const std::string path = local_path(f);
    if (ready(f)) {
      finished_bytes += f.size;
      p.done_bytes = finished_bytes;
      ++p.files_done;
      continue;
    }
    // Already here with no .ok that agrees (dropped in by hand, or copied
    // back with a new mtime): hashed once and adopted if it is the pinned one.
    if (sits_at(path, f.size)) {
      if (stop.load()) return err(status::cancelled);
      // The run's totals first: the hash of a large file takes a while.
      p.state = fetch_state::downloading;
      p.retry_in_s = 0;
      p.done_bytes = finished_bytes;
      report(true);
      const auto hash = sha256_file_hex(path, stop);
      if (!hash && stop.load()) return err(status::cancelled);  // the file stays, to hash next run
      if (hash && *hash == f.sha256) {
        if (!write_ok(f, path)) return err(status::io);
        remove_file(path + ".part");
        finished_bytes += f.size;
        p.done_bytes = finished_bytes;
        ++p.files_done;
        report(true);
        continue;
      }
      remove_file(path);
    }
    std::error_code ec;
    fs::create_directories(fs_path(path).parent_path(), ec);
    const std::string part = path + ".part";
    remove_file(path + ".ok");
    double backoff = opt_.first_backoff_s;
    int misses = 0;
    for (;;) {
      if (stop.load()) return err(status::cancelled);
      std::uint64_t have = size_of(part);
      if (have > f.size) {
        remove_file(part);
        have = 0;
      }
      if (have < f.size) {
        p.state = fetch_state::downloading;
        p.retry_in_s = 0;
        p.done_bytes = finished_bytes + have;
        report(true);
        std::ofstream out;
        std::uint64_t got = have;
        bool moved = false;
        const http_reply reply = net_.get(
            f.url, have,
            [&](int code) {
              if (code == 206 && have > 0) {
                out.open(fs_path(part), std::ios::binary | std::ios::app);
              } else if (code == 200) {
                // The server sent the whole file (no Range, or it ignored it).
                got = 0;
                out.open(fs_path(part), std::ios::binary | std::ios::trunc);
              } else {
                return false;
              }
              return static_cast<bool>(out);
            },
            [&](std::span<const std::uint8_t> chunk) {
              if (stop.load() || got + chunk.size() > f.size) return false;
              out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
              if (!out) return false;
              got += chunk.size();
              moved = true;
              p.done_bytes = finished_bytes + got;
              report(false);
              return true;
            });
        out.close();
        if (stop.load()) return err(status::cancelled);
        if (reply.status == 404 || reply.status == 410) {
          p.state = fetch_state::gone;
          report(true);
          return err(status::not_found);
        }
        if (reply.status == 416) {  // the part no longer fits the file: start over
          remove_file(part);
          continue;
        }
        if (moved) backoff = opt_.first_backoff_s;  // it was answering
        if (size_of(part) < f.size) {
          // No answer, a 5xx / 429, or a cut transfer: wait and try again.
          p.state = fetch_state::waiting;
          p.retry_in_s = backoff;
          report(true);
          if (!opt_.wait(backoff, stop)) return err(status::cancelled);
          backoff = std::min(backoff * 2, opt_.max_backoff_s);
          continue;
        }
      }
      // All the bytes: kept only if they are the right ones. Said first, as
      // the adoption above: a run can start here, with a part an earlier one
      // finished but was stopped before it was hashed.
      p.state = fetch_state::downloading;
      p.retry_in_s = 0;
      p.done_bytes = finished_bytes + f.size;
      report(true);
      const auto hash = sha256_file_hex(part, stop);
      if (!hash && stop.load()) return err(status::cancelled);  // the part stays, to hash next run
      if (hash && *hash == f.sha256) {
        remove_file(path);
        fs::rename(fs_path(part), fs_path(path), ec);
        if (ec || !write_ok(f, path)) return err(status::io);
        break;
      }
      remove_file(part);
      if (++misses >= opt_.hash_attempts) {
        p.state = fetch_state::failed;
        report(true);
        return err(status::corrupt);
      }
    }
    finished_bytes += f.size;
    p.done_bytes = finished_bytes;
    ++p.files_done;
  }
  p.state = fetch_state::done;
  p.file.clear();
  p.retry_in_s = 0;
  report(true);
  return {};
}

void fetcher::prune(std::span<const remote_file> keep) const { prune_fetched(root_, keep); }

namespace {
struct null_transport final : http_transport {
  http_reply get(const std::string&, std::uint64_t, const std::function<bool(int)>&,
                 const std::function<bool(std::span<const std::uint8_t>)>&) override {
    return {};
  }
};
}  // namespace

bool fetch_ready(const std::string& root, const remote_file& f) {
  null_transport none;
  return fetcher(root, none).ready(f);
}

std::string fetch_path_key(const std::string& path, bool fold) { return key_of(fs_path(path), fold); }

void prune_fetched(const std::string& root_, std::span<const remote_file> keep) {
  null_transport none;
  const fetcher at(root_, none);
  std::set<std::string> wanted;
  for (const remote_file& f : keep) {
    const std::string p = at.local_path(f);
    wanted.insert(key_of(fs_path(p), kFoldCase));
    wanted.insert(key_of(fs_path(p + ".ok"), kFoldCase));
    wanted.insert(key_of(fs_path(p + ".part"), kFoldCase));
  }
  std::error_code ec;
  // With no trailing separator (lexically_normal leaves "a/b/"): through
  // one, the check below would see a link's target rather than the link.
  fs::path root = fs_path(root_).lexically_normal();
  while (!root.has_filename() && root.has_relative_path()) root = root.parent_path();
  // Only a folder of its own: a root that is a link or (on Windows) a
  // junction is someone else's folder, and nothing in it is the fetcher's.
  if (fs::symlink_status(root, ec).type() != fs::file_type::directory) return;
  std::vector<fs::path> drop;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::file_type type = it->symlink_status(ec).type();
    if (type == fs::file_type::directory) continue;  // walked into
    if (type != fs::file_type::regular) {
      // A link, a junction (MSVC's file_type::junction) or anything else:
      // never followed, never deleted.
      it.disable_recursion_pending();
      continue;
    }
    if (!wanted.count(key_of(it->path(), kFoldCase))) drop.push_back(it->path());
  }
  for (const fs::path& p : drop) fs::remove(p, ec);
}

}  // namespace mv::ai
