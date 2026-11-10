// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over a folder of files: the tests, and `driftwire serve`, which
// has no keyring to use (it says so; it is not a keychain). Names are
// hex-encoded into file names. On POSIX each value is this user's alone (see
// secrets.h); on Windows the per-user profile's ACL is what guards the folder.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "addons/driftwire/secrets.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <string_view>
#endif

namespace mv::driftwire::secrets {

namespace {

class folder_store final : public store {
 public:
  explicit folder_store(std::string folder) : folder_(std::move(folder)) {}
#if defined(_WIN32)
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    std::ofstream f(path(name), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(f);
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    std::ifstream f(path(name), std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<std::uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (v.size() > kMaxValue) return std::nullopt;
    return v;
  }
#else
  // Whole or not at all, and 0600 from its first byte: another process reading
  // while this writes sees the old value, a crash leaves the old value.
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    static std::atomic<unsigned> counter{0};
    const std::string final_path = path(name);
    const std::string temp = final_path + ".tmp" + std::to_string(::getpid()) + "." + std::to_string(counter.fetch_add(1));
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, value);
    ok = ::fsync(fd) == 0 && ok;
    ok = ::close(fd) == 0 && ok;
    if (ok) ok = ::rename(temp.c_str(), final_path.c_str()) == 0;
    if (!ok) {
      (void)::unlink(temp.c_str());
      return false;
    }
    // Into the folder's own record too, so a power cut after this keeps it.
    if (const int dir = ::open(folder_.c_str(), O_RDONLY | O_CLOEXEC); dir >= 0) {
      (void)::fsync(dir);
      (void)::close(dir);
    }
    return true;
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    const int fd = ::open(path(name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    // Someone else's, or readable by anyone else, is refused.
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
        st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > kMaxValue) {
      (void)::close(fd);
      return std::nullopt;
    }
    std::vector<std::uint8_t> v(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < v.size()) {
      const ssize_t n = ::read(fd, v.data() + got, v.size() - got);
      if (n > 0) {
        got += static_cast<std::size_t>(n);
      } else if (n == 0 || errno != EINTR) {
        break;
      }
    }
    (void)::close(fd);
    if (got != v.size()) return std::nullopt;
    return v;
  }
#endif
  bool erase(const std::string& name) override { return std::remove(path(name).c_str()) == 0; }
  bool exists(const std::string& name) override {
    std::error_code ec;
    return std::filesystem::symlink_status(
               std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path(name).data()), path(name).size())), ec)
               .type() != std::filesystem::file_type::not_found &&
           !ec;
  }

 private:
#if !defined(_WIN32)
  static bool write_all(int fd, std::span<const std::uint8_t> value) {
    std::size_t put = 0;
    while (put < value.size()) {
      const ssize_t n = ::write(fd, value.data() + put, value.size() - put);
      if (n > 0) {
        put += static_cast<std::size_t>(n);
      } else if (errno != EINTR) {
        return false;
      }
    }
    return true;
  }
#endif
  std::string path(const std::string& name) const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string p = folder_ + "/secret-";
    for (const unsigned char c : name) {
      p.push_back(kHex[c >> 4]);
      p.push_back(kHex[c & 15]);
    }
    return p;
  }
  std::string folder_;
};

}  // namespace

std::unique_ptr<store> file_store(const std::string& folder) { return std::make_unique<folder_store>(folder); }

bool owner_only_folder(const std::string& folder) {
#if defined(_WIN32)
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(folder.data()), folder.size())), ec);
  return !ec;
#else
  if (folder.empty()) return false;
  if (::mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) {
    // A parent that is missing: make the parents with the default mode, then this one.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(folder).parent_path(), ec);
    if (ec || (::mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST)) return false;
  }
  struct stat st{};
  return ::lstat(folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() &&
         (st.st_mode & 077) == 0;
#endif
}

}  // namespace mv::driftwire::secrets
