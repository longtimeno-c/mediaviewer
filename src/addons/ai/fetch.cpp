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
      // All the bytes: kept only if they are the right ones.
      const auto hash = sha256_file_hex(part);
      if (hash && *hash == f.sha256) {
        remove_file(path);
        fs::rename(fs_path(part), fs_path(path), ec);
        if (ec) return err(status::io);
        std::ofstream ok(fs_path(path + ".ok"), std::ios::binary | std::ios::trunc);
        ok << ok_record(f, mtime_of(path));
        if (!ok) return err(status::io);
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

void prune_fetched(const std::string& root_, std::span<const remote_file> keep) {
  null_transport none;
  const fetcher at(root_, none);
  std::set<fs::path> wanted;
  for (const remote_file& f : keep) {
    const fs::path p = fs_path(at.local_path(f)).lexically_normal();
    wanted.insert(p);
    wanted.insert(fs::path(p.native() + fs::path(".ok").native()));
    wanted.insert(fs::path(p.native() + fs::path(".part").native()));
  }
  std::error_code ec;
  const fs::path root = fs_path(root_);
  if (!fs::is_directory(root, ec)) return;
  std::vector<fs::path> drop;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_symlink(ec)) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && !wanted.count(it->path().lexically_normal())) drop.push_back(it->path());
  }
  for (const fs::path& p : drop) fs::remove(p, ec);
}

}  // namespace mv::ai
