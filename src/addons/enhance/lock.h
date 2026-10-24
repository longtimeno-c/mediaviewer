// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One process at a time (docs/plans/enhance.md, E1): every MediaViewer window is
// its own process and loads its slot add-ons, so the weights folder is
// downloaded into and pruned by the one that holds this lock. As Driftwire's
// instance lock (src/addons/driftwire/net.h): flock(LOCK_EX | LOCK_NB) on
// POSIX, an unshared CreateFileW on Windows. The OS lets go of it if the
// process dies. One TU per OS: lock_posix.cpp, lock_win.cpp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mv::enhance {

class file_lock {
 public:
  explicit file_lock(std::intptr_t handle) noexcept : handle_(handle) {}
  ~file_lock();  // closing lets go of the lock
  file_lock(const file_lock&) = delete;
  file_lock& operator=(const file_lock&) = delete;

 private:
  std::intptr_t handle_;  // a descriptor (POSIX) or a HANDLE (Windows)
};

// Non-blocking and exclusive across processes (and across two tries in one
// process). nullptr when someone else holds it or the file cannot be opened.
// The file is created; its folder must exist.
[[nodiscard]] std::unique_ptr<file_lock> try_lock(const std::string& path_utf8);

// Whether someone holds it now: a probe that never blocks and never makes
// the file (false when there is none). A holder in this process counts too,
// so a caller never probes a lock it holds itself.
[[nodiscard]] bool is_held(const std::string& path_utf8);

}  // namespace mv::enhance
