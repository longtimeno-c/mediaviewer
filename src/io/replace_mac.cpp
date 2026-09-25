// SPDX-License-Identifier: GPL-2.0-or-later
// POSIX half of the io replace port (io/replace.h). macOS in the product; the
// Linux core test build uses it too.
#include "io/replace.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace mv::io {
namespace {

bool write_fd(int fd, std::span<const std::uint8_t> bytes) noexcept {
  std::size_t filled = 0;
  while (filled < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + filled, bytes.size() - filled);
    if (n <= 0) return false;
    filled += static_cast<std::size_t>(n);
  }
#if defined(__APPLE__)
  // fsync(2) on macOS only reaches the drive's cache; F_FULLFSYNC asks the
  // drive to flush it (plan/10 PR 10: "after F_FULLFSYNC"). Some filesystems
  // (SMB, FAT on a card) refuse it; fall back to fsync there.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}

}  // namespace

expected write_new(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) return err(status::io);
  const bool ok = write_fd(fd, bytes);
  if (::close(fd) != 0 || !ok) {
    ::unlink(path.c_str());
    return err(status::io);
  }
  return {};
}

expected write_new_atomic(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  std::string temp;
  int fd = -1;
  for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
    temp = path + ".mvtmp" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
    fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  }
  if (fd < 0) return err(status::io);
  const bool ok = write_fd(fd, bytes);
  // link(2) fails with EEXIST rather than replacing, which is what "new" means;
  // the temporary is unlinked either way.
  const bool linked = ::close(fd) == 0 && ok && ::link(temp.c_str(), path.c_str()) == 0;
  ::unlink(temp.c_str());
  return linked ? expected{} : expected{err(status::io)};
}

expected replace_atomic(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return err(status::io);

  // Same directory, so the rename never crosses a volume.
  std::string temp;
  int fd = -1;
  for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
    temp = path + ".mvtmp" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
    fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
  }
  if (fd < 0) return err(status::io);
  const bool ok = write_fd(fd, bytes) && ::fchmod(fd, st.st_mode & 07777) == 0;
  if (::close(fd) != 0 || !ok || ::rename(temp.c_str(), path.c_str()) != 0) {
    ::unlink(temp.c_str());
    return err(status::io);
  }
  return {};
}

}  // namespace mv::io
