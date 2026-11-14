// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX half of the file port (io/file_port.h). macOS, and Linux (the
// driftwire tool, the add-on and the portable build), with the closest Linux
// equivalents where macOS has its own call.
#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1  // renameat2 / RENAME_NOREPLACE
#endif

#include "io/file_port.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <span>
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

#if defined(__APPLE__)
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
#endif

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

file_id id_of(const struct stat& st) noexcept {
  return {static_cast<std::uint64_t>(st.st_dev), 0, static_cast<std::uint64_t>(st.st_ino)};
}

// A macOS package (an app, a Photos library): one item in Finder, never
// walked into. Linux has none: a folder named "x.app" there is a folder.
bool is_package(std::string_view name) noexcept {
#if defined(__APPLE__)
  for (const char* ext : {".app", ".photoslibrary", ".bundle", ".framework", ".lrdata"}) {
    if (iequals_tail(name, ext)) return true;
  }
#else
  (void)name;
#endif
  return false;
}

}  // namespace

result<file_stat> stat_path(std::string_view utf8_path) {
  detail::simulated_round_trip();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  struct stat st{};
  // not_found only when nothing is there (Check library holds a photo as
  // deleted on that alone); a refused or failed stat stays io.
  if (::stat(path.c_str(), &st) != 0) return err(errno == ENOENT || errno == ENOTDIR ? status::not_found : status::io);
  file_stat out;
  out.is_directory = S_ISDIR(st.st_mode);
  out.size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  out.mtime_unix = mtime_of(st);
#if defined(__APPLE__)
  out.mtime_ns = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  if (st.st_birthtimespec.tv_sec > 0) out.created_unix = static_cast<std::int64_t>(st.st_birthtimespec.tv_sec);
#if defined(SF_DATALESS)
  out.cloud_placeholder = (st.st_flags & SF_DATALESS) != 0;
#endif
#else
  out.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  return out;
}

result<file_id> identify(std::string_view utf8_path) {
  detail::simulated_round_trip();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return err(status::io);
  if (S_ISLNK(st.st_mode)) return err(status::permission_denied);
  if (!S_ISREG(st.st_mode)) return err(status::io);
  return id_of(st);
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

namespace {

// Every plain file (and, with `with_directories`, folder) of `d`, then closes it.
std::vector<dir_child> children_of(DIR* d, bool with_directories) {
  std::vector<dir_child> out;
  const int dfd = ::dirfd(d);
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    if (name == "." || name == "..") continue;
    struct stat st{};
    // fstatat on the open directory: no path rebuilt per entry, and a link is
    // seen as a link (AT_SYMLINK_NOFOLLOW), never followed.
    if (::fstatat(dfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    const bool is_dir = S_ISDIR(st.st_mode);
    if (!S_ISREG(st.st_mode) && !(is_dir && with_directories)) continue;
    dir_child f;
    f.name.assign(name);
    f.is_directory = is_dir;
    f.size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    f.mtime_unix = mtime_of(st);
    out.push_back(std::move(f));
  }
  ::closedir(d);
  return out;
}

// The folder `parts` names under `root`, each part opened from the one before
// and never through a link. The caller closes it. O_NONBLOCK: a part that is
// a pipe must not hang the open.
result<int> folder_beneath(const std::string& root, std::span<const std::string_view> parts) {
  int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return err(status::io);
  for (const std::string_view part : parts) {
    const std::string name(part);
    const int next = ::openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (next < 0) {
      struct stat st{};
      const bool link = ::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode);
      ::close(fd);
      return err(link ? status::permission_denied : status::io);
    }
    ::close(fd);
    fd = next;
  }
  return fd;
}

file_stat beneath_stat_of(const struct stat& st) noexcept {
  file_stat out;
  out.size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  out.mtime_unix = mtime_of(st);
  out.is_directory = S_ISDIR(st.st_mode);
  return out;
}

}  // namespace

result<std::vector<dir_child>> list_children(std::string_view utf8_dir, bool with_directories) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  const std::string dir(utf8_dir);
  DIR* d = ::opendir(dir.c_str());
  if (!d) return errno == ENOENT ? result<std::vector<dir_child>>(std::vector<dir_child>{}) : err(status::io);
  return children_of(d, with_directories);
}

result<std::vector<dir_child>> list_beneath(std::string_view root, std::string_view relative_slash,
                                            bool with_directories) {
  detail::simulated_round_trip();
  std::vector<std::string_view> parts;
  if (root.empty() || !detail::relative_parts(relative_slash, parts)) return err(status::invalid_arg);
  auto fd = folder_beneath(std::string(root), parts);
  if (!fd) return err(fd.error());
  DIR* d = ::fdopendir(*fd);
  if (!d) {
    ::close(*fd);
    return err(status::io);
  }
  return children_of(d, with_directories);
}

result<file_stat> stat_beneath(std::string_view root, std::string_view relative_slash) {
  detail::simulated_round_trip();
  std::vector<std::string_view> parts;
  if (root.empty() || !detail::relative_parts(relative_slash, parts)) return err(status::invalid_arg);
  const bool whole = parts.empty();
  auto fd = folder_beneath(std::string(root), whole ? std::span<const std::string_view>{}
                                                    : std::span<const std::string_view>(parts).first(parts.size() - 1));
  if (!fd) return err(fd.error());
  struct stat st{};
  int rc = 0;
  if (whole) {
    rc = ::fstat(*fd, &st);
  } else {
    const std::string name(parts.back());
    rc = ::fstatat(*fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW);
  }
  ::close(*fd);
  if (rc != 0) return err(status::io);
  if (S_ISLNK(st.st_mode)) return err(status::permission_denied);
  if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) return err(status::io);
  return beneath_stat_of(st);
}

tree_usage tree_size(std::string_view utf8_path) {
  tree_usage total;
  if (utf8_path.empty()) return total;
  const std::string root(utf8_path);
  struct stat st{};
  if (::lstat(root.c_str(), &st) != 0) return total;
  if (S_ISREG(st.st_mode)) {
    total.bytes = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    total.files = 1;
    return total;
  }
  if (!S_ISDIR(st.st_mode)) return total;  // a link, a socket: nothing of ours
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    const std::string dir = std::move(pending.back());
    pending.pop_back();
    DIR* d = ::opendir(dir.c_str());
    if (!d) continue;
    const int dfd = ::dirfd(d);
    while (dirent* ent = ::readdir(d)) {
      const std::string_view name(ent->d_name);
      if (name == "." || name == "..") continue;
      struct stat cst{};
      if (::fstatat(dfd, ent->d_name, &cst, AT_SYMLINK_NOFOLLOW) != 0) continue;
      if (S_ISDIR(cst.st_mode)) {
        pending.push_back(join_path(dir, name));
      } else if (S_ISREG(cst.st_mode)) {
        total.bytes += cst.st_size > 0 ? static_cast<std::uint64_t>(cst.st_size) : 0;
        ++total.files;
      }
    }
    ::closedir(d);
  }
  return total;
}

result<std::string> make_directories_beneath(std::string_view base_utf8, std::string_view relative_slash) {
  if (base_utf8.empty()) return err(status::invalid_arg);
  std::string path(base_utf8);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  struct stat st{};
  // The base is the person's own choice (a link there is theirs): followed.
  if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return err(status::io);
  std::string_view rest = relative_slash;
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    if (part.empty() || part == "." || part == "..") return err(status::invalid_arg);
    path = join_path(path, part);
    for (int attempt = 0;; ++attempt) {
      if (::lstat(path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) return err(status::permission_denied);
        if (!S_ISDIR(st.st_mode)) return err(status::io);
        break;
      }
      if (errno != ENOENT || attempt > 0) return err(status::io);
      // Made here, or by someone else in between: the lstat above says which.
      if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return err(status::io);
    }
  }
  return path;
}

expected mark_from_elsewhere(std::string_view utf8_path, std::string_view agent) {
  if (utf8_path.empty()) return err(status::invalid_arg);
#if defined(__APPLE__)
  // The format LaunchServices writes for a download: flags (0x0081: from the
  // network, not yet approved), the time in hex, the agent, no event id.
  // Gatekeeper checks any file carrying it, and Archive Utility hands it on
  // to what it extracts.
  char head[32];
  std::snprintf(head, sizeof head, "0081;%08llx;", static_cast<unsigned long long>(::time(nullptr)));
  std::string value(head);
  for (char c : agent) value.push_back(c == ';' ? ' ' : c);
  value.push_back(';');
  const std::string path(utf8_path);
  if (::setxattr(path.c_str(), "com.apple.quarantine", value.data(), value.size(), 0, XATTR_NOFOLLOW) != 0) {
    return err(status::io);
  }
  return {};
#else
  (void)agent;
  return err(status::unsupported_format);  // the Linux core test build has no such mark
#endif
}

expected set_hidden(std::string_view utf8_path, bool hidden) {
  if (utf8_path.empty()) return err(status::invalid_arg);
#if defined(__APPLE__)
  const std::string path(utf8_path);
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return err(status::io);
  const auto flags = hidden ? (st.st_flags | UF_HIDDEN) : (st.st_flags & ~static_cast<decltype(st.st_flags)>(UF_HIDDEN));
  if (flags == st.st_flags) return {};
  return ::lchflags(path.c_str(), flags) == 0 ? expected{} : err(status::io);
#else
  (void)hidden;
  return err(status::unsupported_format);  // no hidden flag: dot names only
#endif
}

result<rename_outcome> rename_no_replace(std::string_view from_utf8, std::string_view to_utf8) {
  detail::simulated_round_trip();
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
  // destination's writer is serialised per device (docs/design/18 "one writer per
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

expected file_reader::open(std::string_view utf8_path, read_mode mode, bool /*concurrent*/) {
  detail::simulated_round_trip();
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
  // Linux: drop the clean pages the writer's fsync left (flush_durable drops
  // them too), so the read comes from the device.
  (void)::posix_fadvise(fd, 0, 0,
                        mode == read_mode::uncached ? POSIX_FADV_DONTNEED : POSIX_FADV_SEQUENTIAL);
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  impl_->size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  return {};
}

expected file_reader::open_same(std::string_view utf8_path, const file_id& expect) {
  detail::simulated_round_trip();
  close();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  // O_NOFOLLOW: a link at the last part fails (ELOOP) rather than being read
  // through; one in a folder above gives another file, which fstat shows.
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return err(errno == ELOOP ? status::permission_denied : status::io);
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return err(status::io);
  }
  if (expect.known() && id_of(st) != expect) {
    ::close(fd);
    return err(status::corrupt);
  }
#if defined(__APPLE__)
  (void)::fcntl(fd, F_RDAHEAD, 1);
#else
  (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  impl_->size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  return {};
}

result<std::size_t> file_reader::read(std::span<std::uint8_t> into) {
  detail::simulated_round_trip();
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

expected file_reader::open_beneath(std::string_view root, std::string_view relative_slash) {
  detail::simulated_round_trip();
  close();
  std::vector<std::string_view> parts;
  if (root.empty() || !detail::relative_parts(relative_slash, parts) || parts.empty()) return err(status::invalid_arg);
  auto parent = folder_beneath(std::string(root), std::span<const std::string_view>(parts).first(parts.size() - 1));
  if (!parent) return err(parent.error());
  const std::string name(parts.back());
  const int fd = ::openat(*parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    struct stat link_st{};
    const bool link = ::fstatat(*parent, name.c_str(), &link_st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(link_st.st_mode);
    ::close(*parent);
    return err(link ? status::permission_denied : status::io);
  }
  ::close(*parent);
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return err(status::io);
  }
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#if defined(__APPLE__)
  (void)::fcntl(fd, F_RDAHEAD, 1);
#else
  (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  impl_->size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  return {};
}

result<std::size_t> file_reader::read_at(std::uint64_t offset, std::span<std::uint8_t> into) {
  detail::simulated_round_trip();
  if (!impl_) return err(status::invalid_arg);
  std::size_t filled = 0;
  while (filled < into.size()) {
    const ssize_t n = ::pread(impl_->fd, into.data() + filled, into.size() - filled,
                              static_cast<off_t>(offset + filled));
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

result<rename_outcome> file_writer::create_new(std::string_view utf8_path, bool /*concurrent*/) {
  detail::simulated_round_trip();
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

expected file_writer::open_resume(std::string_view utf8_path, std::uint64_t offset) {
  detail::simulated_round_trip();
  impl_.reset();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return err(errno == ENOENT ? status::not_found : errno == ELOOP ? status::corrupt : status::io);
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) < offset) {
    ::close(fd);
    return err(status::corrupt);
  }
  if (::ftruncate(fd, static_cast<off_t>(offset)) != 0 || ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
    ::close(fd);
    return err(status::io);
  }
#if defined(__APPLE__)
  (void)::fcntl(fd, F_NOCACHE, 1);  // as create_new: the read-back comes from the device
#endif
  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  return {};
}

expected file_writer::write(std::span<const std::uint8_t> bytes) {
  detail::simulated_round_trip();
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

expected file_writer::write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
  detail::simulated_round_trip();
  if (!impl_) return err(status::invalid_arg);
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::pwrite(impl_->fd, bytes.data() + done, bytes.size() - done,
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return err(status::io);
    }
    if (n == 0) return err(status::io);
    done += static_cast<std::size_t>(n);
  }
  return {};
}

expected file_writer::set_size(std::uint64_t bytes) {
  detail::simulated_round_trip();
  if (!impl_) return err(status::invalid_arg);
  return ::ftruncate(impl_->fd, static_cast<off_t>(bytes)) == 0 ? expected{} : err(status::io);
}

expected file_writer::flush_durable() {
  detail::simulated_round_trip();
  if (!impl_) return err(status::invalid_arg);
  if (!durable_sync(impl_->fd)) return err(status::io);
#if !defined(__APPLE__)
  // Linux has no per-file no-cache for buffered writes (the Mac's F_NOCACHE):
  // the pages just made durable are clean now, so drop them, and the verify
  // read-back (read_mode::uncached) comes from the device, not from memory.
  (void)::posix_fadvise(impl_->fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
  return {};
}

expected file_writer::set_mtime(std::int64_t mtime_unix) {
  detail::simulated_round_trip();
  if (!impl_) return err(status::invalid_arg);
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = static_cast<time_t>(mtime_unix);
  return ::futimens(impl_->fd, times) == 0 ? expected{} : err(status::io);
}

expected file_writer::close() {
  detail::simulated_round_trip();
  if (!impl_) return {};
  const int fd = impl_->fd;
  impl_->fd = -1;
  impl_.reset();
  return ::close(fd) == 0 ? expected{} : err(status::io);
}

// ---------------------------------------------------------------------------
namespace {

// How far a walk's listing can be trusted.
struct walk_trust {
  bool incomplete = false;  // something failed that says nothing about the files
  bool vanished = false;    // a name it had just listed was gone: check the root after
};

void note_walk_failure(walk_trust& t, int error) {
  switch (error) {
    case EACCES:
    case EPERM:
    case ELOOP:
    case ENAMETOOLONG:
      return;  // this folder, for good: the walk is as complete as it can be
    case ENOENT:
    case ENOTDIR:
      t.vanished = true;
      return;
    default:
      t.incomplete = true;  // EIO, ENOTCONN, ETIMEDOUT, ESTALE, EHOSTDOWN, ...
  }
}

bool walk_dir(const std::string& dir, const std::string& rel, int depth, int max_depth,
              std::vector<tree_entry>& out, walk_trust& trust) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) {
    if (depth == 0) return false;  // the root is an error
    note_walk_failure(trust, errno);
    return true;  // a subfolder is skipped
  }
  std::vector<std::string> subdirs;
  for (;;) {
    errno = 0;
    dirent* ent = ::readdir(d);
    if (!ent) {
      // End of the folder leaves errno alone; a failed read looks the same
      // otherwise, and the rest of the folder would be taken for absent.
      if (errno != 0) trust.incomplete = true;
      break;
    }
    const std::string_view name(ent->d_name);
    // ".", "..", AppleDouble "._x", ".Spotlight-V100", ".Trashes": all hidden.
    if (name.empty() || name.front() == '.') continue;
    const std::string path = join_path(dir, name);
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
      note_walk_failure(trust, errno);
      continue;
    }
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
    e.id = id_of(st);
#ifdef SF_DATALESS
    // Evicted by iCloud Drive (or another File Provider): reading it would
    // download it (2026-10-05).
    e.cloud_only = (st.st_flags & SF_DATALESS) != 0;
#endif
    out.push_back(std::move(e));
  }
  ::closedir(d);
  for (const std::string& sub : subdirs) {
    const std::string child_rel = rel.empty() ? sub : rel + "/" + sub;
    walk_dir(join_path(dir, sub), child_rel, depth + 1, max_depth, out, trust);
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
                    const std::function<bool(const tree_entry&)>& visit, bool* incomplete) {
  if (utf8_root.empty()) return err(status::invalid_arg);
  const std::string root(utf8_root);
  struct stat before{};
  const bool had_root = ::stat(root.c_str(), &before) == 0;
  std::vector<tree_entry> entries;
  walk_trust trust;
  if (!walk_dir(root, std::string(), 0, max_depth, entries, trust)) {
    return err(status::io);
  }
  if (trust.vanished) {
    // A folder deleted under the walk, or the whole share unmounted under it,
    // which also reads as "no such file": the root is on another device now.
    struct stat after{};
    if (!had_root || ::stat(root.c_str(), &after) != 0 || after.st_dev != before.st_dev) trust.incomplete = true;
  }
  if (incomplete) *incomplete = trust.incomplete;
  std::sort(entries.begin(), entries.end(), [](const tree_entry& a, const tree_entry& b) {
    return a.relative_utf8 < b.relative_utf8;
  });
  for (const tree_entry& e : entries) {
    if (!visit(e)) return err(status::cancelled);
  }
  return {};
}

}  // namespace mv::io
