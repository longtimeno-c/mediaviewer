// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/file_transfer.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <system_error>

#include "io/collision_name.h"
#include "io/verified_copy.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <cstdio>
#else
#include <cerrno>
#include <cstdio>
#endif

namespace mv::shell {
namespace {

namespace fs = std::filesystem;

fs::path to_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string join(const std::string& dir, const std::string& name) {
  std::string p = dir;
  if (p.empty() || (p.back() != '/' && p.back() != '\\')) {
    p.push_back(dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos ? '\\' : '/');
  }
  return p + name;
}

class local : public transfer_target {
 public:
  bool exists(const std::string& path) override {
    std::error_code ec;
    return fs::symlink_status(to_path(path), ec).type() != fs::file_type::not_found && !ec;
  }

  mv::status make_directory(const std::string& path) override {
    std::error_code ec;
    return fs::create_directory(to_path(path), ec) && !ec ? mv::status::ok : mv::status::io;
  }

  mv::status put_file(const std::string& src, const std::string& dest, bool verify,
                      const std::atomic<bool>* cancel, std::atomic<std::uint64_t>* done_bytes) override {
    io::copy_options options;
    options.read_back = verify;
    options.cancel = cancel;
    if (done_bytes) options.on_progress = [done_bytes](std::uint64_t d) { done_bytes->fetch_add(d); };
    const std::string targets[] = {dest};
    const auto copied = io::verified_copy(src, std::span<const std::string>(targets), options);
    if (!copied) return copied.error();
    return io::copy_succeeded(copied->targets[0].outcome) ? mv::status::ok
           : copied->targets[0].outcome == io::copy_target_outcome::cancelled ? mv::status::cancelled
                                                                               : mv::status::io;
  }

  rename_outcome rename(const std::string& src, const std::string& dest) override {
#if defined(_WIN32)
    // No MOVEFILE_REPLACE_EXISTING: an existing name fails. No COPY_ALLOWED:
    // another volume fails too, and the runner copies with a check instead.
    if (MoveFileExW(to_path(src).c_str(), to_path(dest).c_str(), 0)) return rename_outcome::done;
    const DWORD e = GetLastError();
    if (e == ERROR_NOT_SAME_DEVICE) return rename_outcome::other_volume;
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return rename_outcome::taken;
    return rename_outcome::failed;
#elif defined(__APPLE__)
    if (renamex_np(src.c_str(), dest.c_str(), RENAME_EXCL) == 0) return rename_outcome::done;
    if (errno == EXDEV) return rename_outcome::other_volume;
    if (errno == EEXIST) return rename_outcome::taken;
    return rename_outcome::failed;
#else
    if (exists(dest)) return rename_outcome::taken;
    if (std::rename(src.c_str(), dest.c_str()) == 0) return rename_outcome::done;
    return errno == EXDEV ? rename_outcome::other_volume : rename_outcome::failed;
#endif
  }

  mv::status remove(const std::string& path) override {
    std::error_code ec;
    fs::remove_all(to_path(path), ec);
    return ec ? mv::status::io : mv::status::ok;
  }
};

// Bytes under `path` (a file's size, a folder's files), for the bar. A walk
// on the worker; what cannot be read counts as nothing.
std::uint64_t size_of(const std::string& path) {
  std::error_code ec;
  const fs::path p = to_path(path);
  const auto st = fs::symlink_status(p, ec);
  if (ec) return 0;
  if (st.type() == fs::file_type::regular) {
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
  }
  if (st.type() != fs::file_type::directory) return 0;
  std::uint64_t total = 0;
  for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2) && !it->is_symlink(e2)) {
      const auto n = it->file_size(e2);
      if (!e2) total += n;
    }
  }
  return total;
}

// `src` (a local file or folder) to `dest`, which is free. A link inside is
// not followed or copied: the item fails, so a move never drops it.
mv::status copy_tree(const std::string& src, const std::string& dest, bool verify, transfer_target& target,
                     transfer_progress& progress, const std::atomic<bool>* cancel) {
  if (cancel && cancel->load()) return mv::status::cancelled;
  // Another computer's file: the target fetches it (a folder there never comes this way).
  if (is_remote_path(src)) {
    if (src.back() == '/') return mv::status::unsupported_format;
    return target.put_file(src, dest, verify, cancel, &progress.done_bytes);
  }
  std::error_code ec;
  const fs::path p = to_path(src);
  const auto st = fs::symlink_status(p, ec);
  if (ec || st.type() == fs::file_type::not_found) return mv::status::not_found;
  if (st.type() == fs::file_type::regular) {
    return target.put_file(src, dest, verify, cancel, &progress.done_bytes);
  }
  if (st.type() != fs::file_type::directory) return mv::status::unsupported_format;
  if (const mv::status s = target.make_directory(dest); s != mv::status::ok) return s;
  std::vector<fs::path> children;
  for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
  if (ec) return mv::status::io;
  std::sort(children.begin(), children.end());
  for (const fs::path& child : children) {
    const mv::status s = copy_tree(to_utf8(child), join(dest, to_utf8(child.filename())), verify, target,
                                   progress, cancel);
    if (s != mv::status::ok) return s;
  }
  return mv::status::ok;
}

bool is_directory(const std::string& path) {
  std::error_code ec;
  return fs::symlink_status(to_path(path), ec).type() == fs::file_type::directory && !ec;
}

std::string quoted_leaf(const std::string& path) { return std::string(browse_path::leaf(path)); }

std::string remote_parent(const std::string& key) {
  std::string_view k = key;
  if (!k.empty() && k.back() == '/') k.remove_suffix(1);
  const std::size_t cut = k.rfind('/');
  return cut == std::string_view::npos ? std::string() : std::string(k.substr(0, cut + 1));
}

class mixed : public transfer_target {
 public:
  explicit mixed(remote_ops ops) : ops_(std::move(ops)) {}

  bool exists(const std::string& path) override {
    if (!is_remote_path(path)) return here_.exists(path);
    if (!ops_.exists) return true;  // unknown: never a name that might be taken
    auto r = ops_.exists(path);
    return !r || r.value();
  }

  mv::status make_directory(const std::string& path) override {
    if (!is_remote_path(path)) return here_.make_directory(path);
    if (!ops_.make_folder) return mv::status::permission_denied;
    auto r = ops_.make_folder(remote_parent(path), browse_path::leaf(path));
    return r ? mv::status::ok : r.error();
  }

  mv::status put_file(const std::string& src, const std::string& dest, bool verify, const std::atomic<bool>* cancel,
                      std::atomic<std::uint64_t>* done_bytes) override {
    std::string local = src;
    if (is_remote_path(src)) {
      if (!ops_.fetch) return mv::status::permission_denied;
      auto got = ops_.fetch(src, cancel, nullptr);
      if (!got) return got.error();
      local = std::move(got).value();
    }
    if (!is_remote_path(dest)) return here_.put_file(local, dest, verify || is_remote_path(src), cancel, done_bytes);
    if (!ops_.add) return mv::status::permission_denied;
    // The service checks the whole file's hash as it lands: what reached its disk is what was sent.
    auto r = ops_.add(remote_parent(dest), local, browse_path::leaf(dest), cancel, done_bytes);
    return r ? mv::status::ok : r.error();
  }

  rename_outcome rename(const std::string& src, const std::string& dest) override {
    const bool from = is_remote_path(src), to = is_remote_path(dest);
    if (!from && !to) return here_.rename(src, dest);
    if (from != to || drop_detail::server_of(src) != drop_detail::server_of(dest)) return rename_outcome::other_volume;
    if (!ops_.move) return rename_outcome::failed;
    bool taken = false;
    auto r = ops_.move(src, remote_parent(dest), browse_path::leaf(dest), taken);
    if (r) return rename_outcome::done;
    return taken ? rename_outcome::taken : rename_outcome::failed;
  }

  mv::status remove(const std::string& path) override {
    if (!is_remote_path(path)) return here_.remove(path);
    if (!ops_.remove) return mv::status::permission_denied;
    auto r = ops_.remove(path);
    return r ? mv::status::ok : r.error();
  }

 private:
  remote_ops ops_;
  local here_;
};

}  // namespace

std::unique_ptr<transfer_target> local_target() { return std::make_unique<local>(); }
std::unique_ptr<transfer_target> mixed_target(remote_ops ops) { return std::make_unique<mixed>(std::move(ops)); }
bool is_remote_path(std::string_view path) noexcept { return path.substr(0, 7) == "remote:"; }

transfer_result run_transfer(drop_verb verb, const std::vector<std::string>& sources, const std::string& target_dir,
                             transfer_target& target, transfer_progress& progress,
                             const std::atomic<bool>* cancel) {
  transfer_result out;
  out.verb = verb;
  if (verb == drop_verb::none) return out;
  const std::vector<transfer_step> plan =
      plan_transfer(sources, target_dir, verb, [&](std::string_view p) { return target.exists(std::string(p)); });
  std::uint64_t total = 0;
  for (const auto& step : plan) total += size_of(step.source);
  progress.total_bytes.store(total);
  progress.total_items.store(static_cast<std::uint32_t>(plan.size()));

  for (const transfer_step& step : plan) {
    transfer_item item{step.source, step.dest, mv::status::ok};
    if (cancel && cancel->load()) {
      out.cancelled = true;
      break;
    }
    const std::uint64_t before = progress.done_bytes.load();
    const std::uint64_t bytes = size_of(step.source);
    bool renamed = false;
    if (verb == drop_verb::move) {
      auto outcome = target.rename(step.source, step.dest);
      if (outcome == transfer_target::rename_outcome::taken) {
        // Taken since the plan (another app, the server's other user): keep both, as Finder does.
        const std::string parent = item.dest.substr(0, item.dest.size() - browse_path::leaf(item.dest).size());
        const std::string again = io::unique_name(browse_path::leaf(step.source), [&](std::string_view candidate) {
          return target.exists(parent + std::string(candidate));
        });
        if (!again.empty()) {
          item.dest = parent + again;
          outcome = target.rename(step.source, item.dest);
        }
      }
      switch (outcome) {
        case transfer_target::rename_outcome::done: renamed = true; break;
        case transfer_target::rename_outcome::other_volume: break;
        case transfer_target::rename_outcome::taken:
        case transfer_target::rename_outcome::failed: item.status = mv::status::io; break;
      }
    }
    if (!renamed && item.status == mv::status::ok) {
      // Across volumes a move's copy is read back before the original goes.
      item.status = copy_tree(step.source, step.dest, verb == drop_verb::move, target, progress, cancel);
      if (item.status != mv::status::ok) {
        // What we wrote under a name that was free; a remote one never got a name at all
        // (the service keeps an unfinished upload aside, to resume).
        if (!is_remote_path(step.dest)) (void)target.remove(step.dest);
        if (item.status == mv::status::cancelled) out.cancelled = true;
      } else if (verb == drop_verb::move && is_remote_path(step.source)) {
        // Down from the server, read back right here: only now it goes to the server's trash.
        if (target.remove(step.source) != mv::status::ok) {
          (void)target.remove(step.dest);  // never two
          item.status = mv::status::io;
        }
      } else if (verb == drop_verb::move) {
        const bool folder = is_directory(step.source);
        std::error_code ec;
        if (folder) {
          fs::remove_all(to_path(step.source), ec);
        } else {
          fs::remove(to_path(step.source), ec);
        }
        if (ec) {
          // A file that will not go: take the copy back, so a move is never two.
          // A folder may have half gone; then the copy is all that is whole, so
          // it stays and the item reports the failure.
          if (!folder) (void)target.remove(step.dest);
          item.status = mv::status::io;
        }
      }
    }
    progress.done_bytes.store(before + bytes);
    progress.done_items.fetch_add(1);
    out.items.push_back(std::move(item));
    if (out.cancelled) break;
  }
  return out;
}

std::string make_new_folder(const std::string& parent, const std::string& name, transfer_target& target) {
  const std::string chosen =
      io::unique_name(name, [&](std::string_view candidate) { return target.exists(join(parent, std::string(candidate))); });
  if (chosen.empty()) return {};
  const std::string path = join(parent, chosen);
  return target.make_directory(path) == mv::status::ok ? path : std::string();
}

std::string transfer_summary(const transfer_result& r, const std::string& target_dir) {
  const std::size_t ok = r.succeeded();
  const std::size_t all = r.items.size();
  const std::string where = quoted_leaf(target_dir);
  const bool move = r.verb == drop_verb::move;
  const auto count = [](std::size_t n) { return n == 1 ? std::string("1 item") : std::to_string(n) + " items"; };
  if (all == 0) return r.cancelled ? "Cancelled · nothing changed" : "Nothing to " + std::string(move ? "move" : "copy");
  std::string what = ok == 1 && all == 1 ? quoted_leaf(r.items[0].source) : count(ok);
  if (r.cancelled) return "Cancelled · " + std::to_string(ok) + " of " + std::to_string(all) + (move ? " moved" : " copied");
  if (ok == all) return std::string(move ? "Moved " : "Copied ") + what + " to " + where;
  if (ok == 0) return std::string(move ? "Could not move " : "Could not copy ") + (all == 1 ? quoted_leaf(r.items[0].source) : count(all)) + " to " + where;
  return std::string(move ? "Moved " : "Copied ") + std::to_string(ok) + " of " + std::to_string(all) + " to " + where +
         " · " + std::to_string(all - ok) + " failed";
}

std::shared_ptr<transfer_board::job> transfer_board::add(std::string title, std::string target, std::int64_t now_ms) {
  auto j = std::make_shared<job>();
  j->title = std::move(title);
  j->target = std::move(target);
  j->started_ms = now_ms;
  {
    std::lock_guard lock(m_);
    j->id = kTransferJobBit | next_++;
    jobs_.push_back(j);
  }
  generation_.fetch_add(1);
  return j;
}

void transfer_board::finish(const std::shared_ptr<job>& j, const transfer_result& r, std::int64_t now_ms) {
  j->failed.store(static_cast<std::int32_t>(r.failed()));
  j->ended_ms.store(now_ms);
  j->state.store(r.cancelled ? 5 : r.failed() > 0 && r.succeeded() == 0 ? 4 : 3);
  generation_.fetch_add(1);
}

std::vector<std::uint64_t> transfer_board::ids() const {
  std::lock_guard lock(m_);
  std::vector<std::uint64_t> out;
  out.reserve(jobs_.size());
  for (const auto& j : jobs_) out.push_back(j->id);
  return out;
}

bool transfer_board::view(std::uint64_t id, std::int64_t now_ms, transfer_job_view& out) const {
  std::lock_guard lock(m_);
  for (const auto& j : jobs_) {
    if (j->id != id) continue;
    out.id = j->id;
    out.state = j->state.load();
    const std::uint64_t total = j->progress.total_bytes.load();
    const std::uint32_t items = j->progress.total_items.load();
    out.done_items = static_cast<std::int32_t>(j->progress.done_items.load());
    out.fraction = out.state == 3 ? 1.0
                   : total > 0    ? std::min(1.0, double(j->progress.done_bytes.load()) / double(total))
                   : items > 0    ? double(out.done_items) / double(items)
                                  : 0.0;
    const std::int64_t end = j->ended_ms.load();
    out.elapsed_ms = (end > 0 ? end : now_ms) - j->started_ms;
    out.failed = j->failed.load();
    out.title = j->title;
    out.target = j->target;
    return true;
  }
  return false;
}

void transfer_board::cancel(std::uint64_t id) {
  std::lock_guard lock(m_);
  for (const auto& j : jobs_) {
    if (j->id == id) j->cancel.store(true);
  }
}

void transfer_board::clear_finished() {
  {
    std::lock_guard lock(m_);
    std::erase_if(jobs_, [](const std::shared_ptr<job>& j) { return j->state.load() >= 3; });
  }
  generation_.fetch_add(1);
}

bool transfer_board::any_running() const {
  std::lock_guard lock(m_);
  return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return j->state.load() <= 2; });
}

}  // namespace mv::shell
