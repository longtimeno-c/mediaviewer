// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX half of the file port (io/file_port.h). macOS in the product; the
// Linux core test build (cmake/portable.cmake) uses it too, with the closest
// Linux equivalents where macOS has its own call.
#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1  // renameat2 / RENAME_NOREPLACE
#endif

#include "io/file_port.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace mv::io {
namespace {

std::int64_t mtime_of(const struct stat& st) noexcept {
#if defined(__APPLE__)
  return static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
#else
  return static_cast<std::int64_t>(st.st_mtim.tv_sec);
#endif
}

bool durable_sync(int fd) noexcept {
#if defined(__APPLE__)
  // fsync(2) on macOS reaches only the drive's cache; F_FULLFSYNC asks the
  // drive to flush it. SMB and some card filesystems refuse it: fsync there.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}

bool iequals_tail(std::string_view name, std::string_view ext) noexcept {
  if (name.size() <= ext.size()) return false;
  const std::string_view tail = name.substr(name.size() - ext.size());
  for (std::size_t i = 0; i < ext.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != ext[i]) return false;
  }
  return true;
}

// The rename itself to stable storage: the directory entry, not only the
// file's bytes. F8 across volumes deletes the source right after this, so a
// power cut must not be able to lose the new name. Best effort: some card and
// network filesystems refuse to sync a directory.
void sync_parent(const std::string& path) noexcept {
  std::string dir(parent_of(path));
  if (dir.empty()) dir = ".";
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  (void)durable_sync(fd);
  ::close(fd);
}

bool is_package(std::string_view name) noexcept {
  for (const char* ext : {".app", ".photoslibrary", ".bundle", ".framework", ".lrdata"}) {
    if (iequals_tail(name, ext)) return true;
  }
  return false;
}

}  // namespace

result<file_stat> stat_path(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return err(status::io);
  file_stat out;
  out.is_directory = S_ISDIR(st.st_mode);
  out.size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  out.mtime_unix = mtime_of(st);
  return out;
}

expected make_directories(std::string_view utf8_dir) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  std::string path(utf8_dir);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode) ? expected{} : err(status::io);
  const std::string_view parent = parent_of(path);
  if (!parent.empty() && parent != path) MV_TRY_VOID(make_directories(parent));
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return err(status::io);
  return {};
}

expected remove_file(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  if (::unlink(path.c_str()) == 0 || errno == ENOENT) return {};
  return err(status::io);
}

expected remove_tree(std::string_view utf8_dir) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  const std::string dir(utf8_dir);
  struct stat st{};
  if (::lstat(dir.c_str(), &st) != 0) return errno == ENOENT ? expected{} : err(status::io);
  if (!S_ISDIR(st.st_mode)) return ::unlink(dir.c_str()) == 0 ? expected{} : err(status::io);
  DIR* d = ::opendir(dir.c_str());
  if (!d) return err(status::io);
  std::vector<std::string> children;
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    if (name == "." || name == "..") continue;
    children.emplace_back(join_path(dir, name));
  }
  ::closedir(d);
  bool ok = true;
  for (const std::string& child : children) {
    struct stat cst{};
    if (::lstat(child.c_str(), &cst) != 0) continue;
    if (S_ISDIR(cst.st_mode)) {
      ok = remove_tree(child).has_value() && ok;
    } else {
      ok = ::unlink(child.c_str()) == 0 && ok;  // a symlink goes, its target stays
    }
  }
  return ok && ::rmdir(dir.c_str()) == 0 ? expected{} : err(status::io);
}

result<rename_outcome> rename_no_replace(std::string_view from_utf8, std::string_view to_utf8) {
  if (from_utf8.empty() || to_utf8.empty()) return err(status::invalid_arg);
  const std::string from(from_utf8);
  const std::string to(to_utf8);
#if defined(__APPLE__)
  if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
    sync_parent(to);
    return rename_outcome::renamed;
  }
#else
  if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
    sync_parent(to);
    return rename_outcome::renamed;
  }
#endif
  if (errno == EEXIST) return rename_outcome::name_taken;
  if (errno != ENOTSUP && errno != EINVAL && errno != ENOSYS
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
      && errno != EOPNOTSUPP
#endif
  ) {
    return err(status::io);
  }
  // The filesystem has no exclusive rename (FAT / exFAT on a card, some
  // network shares). A hard link is exclusive where links exist; otherwise
  // check then rename. That last path has a window in which another process
  // could create `to`: nothing in this app writes that name meanwhile, and the
  // destination's writer is serialised per device (plan/18 "one writer per
  // physical destination").
  if (::link(from.c_str(), to.c_str()) == 0) {
    ::unlink(from.c_str());
    sync_parent(to);
    return rename_outcome::renamed;
  }
  if (errno == EEXIST) return rename_outcome::name_taken;
  struct stat st{};
  if (::lstat(to.c_str(), &st) == 0) return rename_outcome::name_taken;
  if (::rename(from.c_str(), to.c_str()) != 0) return err(status::io);
  sync_parent(to);
  return rename_outcome::renamed;
}

// ---------------------------------------------------------------------------
struct file_reader::impl {
  int fd = -1;
  std::uint64_t size = 0;
  ~impl() {
    if (fd >= 0) ::close(fd);
  }
};

file_reader::file_reader() = default;
file_reader::~file_reader() = default;
file_reader::file_reader(file_reader&&) noexcept = default;
file_reader& file_reader::operator=(file_reader&&) noexcept = default;

expected file_reader::open(std::string_view utf8_path, read_mode mode) {
  close();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return err(status::io);
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return err(status::io);
  }
#if defined(__APPLE__)
  if (mode == read_mode::uncached) {
    (void)::fcntl(fd, F_NOCACHE, 1);
  } else {
    (void)::fcntl(fd, F_RDAHEAD, 1);
  }
#else
  // Linux (tests): drop the clean pages the writer's fsync left, so the read
  // comes from the device.
  (void)::posix_fadvise(fd, 0, 0,
                        mode == read_mode::uncached ? POSIX_FADV_DONTNEED : POSIX_FADV_SEQUENTIAL);
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  impl_->size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  return {};
}

result<std::size_t> file_reader::read(std::span<std::uint8_t> into) {
  if (!impl_) return err(status::invalid_arg);
  std::size_t filled = 0;
  while (filled < into.size()) {
    const ssize_t n = ::read(impl_->fd, into.data() + filled, into.size() - filled);
    if (n < 0) {
      if (errno == EINTR) continue;
      return err(status::io);
    }
    if (n == 0) break;
    filled += static_cast<std::size_t>(n);
  }
  return filled;
}

std::uint64_t file_reader::size() const noexcept { return impl_ ? impl_->size : 0; }
void file_reader::close() noexcept { impl_.reset(); }

// ---------------------------------------------------------------------------
struct file_writer::impl {
  int fd = -1;
  ~impl() {
    if (fd >= 0) ::close(fd);
  }
};

file_writer::file_writer() = default;
file_writer::~file_writer() = default;
file_writer::file_writer(file_writer&&) noexcept = default;
file_writer& file_writer::operator=(file_writer&&) noexcept = default;

result<rename_outcome> file_writer::create_new(std::string_view utf8_path) {
  impl_.reset();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (errno == EEXIST) return rename_outcome::name_taken;
    return err(status::io);
  }
#if defined(__APPLE__)
  // Keep the written pages out of the unified buffer cache, so the verify
  // read-back cannot be answered from memory.
  (void)::fcntl(fd, F_NOCACHE, 1);
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  return rename_outcome::renamed;
}

expected file_writer::write(std::span<const std::uint8_t> bytes) {
  if (!impl_) return err(status::invalid_arg);
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(impl_->fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return err(status::io);
    }
    if (n == 0) return err(status::io);
    done += static_cast<std::size_t>(n);
  }
  return {};
}

expected file_writer::flush_durable() {
  if (!impl_) return err(status::invalid_arg);
  return durable_sync(impl_->fd) ? expected{} : err(status::io);
}

expected file_writer::set_mtime(std::int64_t mtime_unix) {
  if (!impl_) return err(status::invalid_arg);
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = static_cast<time_t>(mtime_unix);
  return ::futimens(impl_->fd, times) == 0 ? expected{} : err(status::io);
}

expected file_writer::close() {
  if (!impl_) return {};
  const int fd = impl_->fd;
  impl_->fd = -1;
  impl_.reset();
  return ::close(fd) == 0 ? expected{} : err(status::io);
}

// ---------------------------------------------------------------------------
namespace {

bool walk_dir(const std::string& dir, const std::string& rel, int depth, int max_depth,
              std::vector<tree_entry>& out) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return depth > 0;  // an unreadable subfolder is skipped; the root is an error
  std::vector<std::string> subdirs;
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    // ".", "..", AppleDouble "._x", ".Spotlight-V100", ".Trashes": all hidden.
    if (name.empty() || name.front() == '.') continue;
    const std::string path = join_path(dir, name);
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) continue;
    const std::string child_rel = rel.empty() ? std::string(name) : rel + "/" + std::string(name);
    if (S_ISDIR(st.st_mode)) {
      if (!is_package(name) && depth < max_depth) subdirs.push_back(std::string(name));
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;  // symlinks, devices: never followed
    tree_entry e;
    e.path_utf8 = path;
    e.relative_utf8 = child_rel;
    e.name_utf8 = std::string(name);
    e.size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    e.mtime_unix = mtime_of(st);
    out.push_back(std::move(e));
  }
  ::closedir(d);
  for (const std::string& sub : subdirs) {
    const std::string child_rel = rel.empty() ? sub : rel + "/" + sub;
    walk_dir(join_path(dir, sub), child_rel, depth + 1, max_depth, out);
  }
  return true;
}

bool walk_all_dir(const std::string& dir, const std::string& rel, int depth, int max_depth,
                  const std::function<bool(std::string_view, entry_kind)>& visit, bool& stopped) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return false;
  std::vector<std::string> subdirs;
  bool ok = true;
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    if (name == "." || name == "..") continue;
    const std::string child_rel = rel.empty() ? std::string(name) : rel + "/" + std::string(name);
    struct stat st{};
    entry_kind kind = entry_kind::other;
    if (::lstat(join_path(dir, name).c_str(), &st) == 0) {
      if (S_ISREG(st.st_mode)) kind = entry_kind::file;
      if (S_ISDIR(st.st_mode) && depth < max_depth) kind = entry_kind::directory;
    }
    if (!visit(child_rel, kind)) {
      stopped = true;
      break;
    }
    if (kind == entry_kind::directory) subdirs.emplace_back(name);
  }
  ::closedir(d);
  for (const std::string& sub : subdirs) {
    if (stopped) break;
    const std::string child_rel = rel.empty() ? sub : rel + "/" + sub;
    ok = walk_all_dir(join_path(dir, sub), child_rel, depth + 1, max_depth, visit, stopped) && ok;
  }
  return ok;
}

}  // namespace

expected walk_all_entries(std::string_view utf8_root, int max_depth,
                          const std::function<bool(std::string_view, entry_kind)>& visit) {
  if (utf8_root.empty()) return err(status::invalid_arg);
  bool stopped = false;
  const bool ok = walk_all_dir(std::string(utf8_root), std::string(), 0, max_depth, visit, stopped);
  if (stopped) return err(status::cancelled);
  return ok ? expected{} : err(status::io);
}

result<std::vector<std::string>> child_directories(std::string_view utf8_dir) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  const std::string dir(utf8_dir);
  DIR* d = ::opendir(dir.c_str());
  if (!d) return err(status::io);
  std::vector<std::string> out;
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    if (name.empty() || name.front() == '.') continue;
    struct stat st{};
    if (::lstat(join_path(dir, name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out.emplace_back(name);
    }
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

expected walk_files(std::string_view utf8_root, int max_depth,
                    const std::function<bool(const tree_entry&)>& visit) {
  if (utf8_root.empty()) return err(status::invalid_arg);
  std::vector<tree_entry> entries;
  if (!walk_dir(std::string(utf8_root), std::string(), 0, max_depth, entries)) {
    return err(status::io);
  }
  std::sort(entries.begin(), entries.end(), [](const tree_entry& a, const tree_entry& b) {
    return a.relative_utf8 < b.relative_utf8;
  });
  for (const tree_entry& e : entries) {
    if (!visit(e)) return err(status::cancelled);
  }
  return {};
}

}  // namespace mv::io
