// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "addons/enhance/lock.h"

namespace mv::enhance {

file_lock::~file_lock() { ::close(static_cast<int>(handle_)); }

std::unique_ptr<file_lock> try_lock(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return nullptr;
  // flock belongs to the open file: a second open in this process is refused too.
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<file_lock>(fd);
}

}  // namespace mv::enhance
