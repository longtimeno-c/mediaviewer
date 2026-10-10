// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/file_jobs.h"

#include <memory>
#include <new>

#include "io/duplicate.h"
#include "io/file_ops.h"
#include "io/file_port.h"
#include "io/in_flight.h"

namespace mv::shell {

file_jobs::~file_jobs() { stop(); }

bool file_jobs::start() noexcept {
  if (started_) return true;
  // One worker: operations on one destination run in the order asked, and a
  // card dump copy does not compete with itself for the same disk.
  started_ = pool_.start(1) == mv::status::ok;
  return started_;
}

void file_jobs::stop() noexcept {
  if (!started_) return;
  pool_.shutdown();
  started_ = false;
}

bool file_jobs::submit(HWND notify, file_job_kind kind, std::vector<std::string> paths,
                       std::string dest_dir, std::uint64_t token) noexcept {
  if (!started_ || !notify || paths.empty()) return false;
  if ((kind == file_job_kind::copy || kind == file_job_kind::move) && dest_dir.empty()) return false;
  if (kind == file_job_kind::duplicate && paths.size() > 2) return false;
  try {
    // job_fn is a std::function, so what it captures must be copyable.
    auto work = std::make_shared<file_job_result>();
    work->kind = kind;
    work->token = token;
    work->items.reserve(paths.size());
    // A duplicate reports once, for its stop; the pair half rides along.
    std::string secondary;
    if (kind == file_job_kind::duplicate && paths.size() == 2) secondary = std::move(paths[1]);
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (kind == file_job_kind::duplicate && i > 0) break;
      file_job_item item;
      item.path = std::move(paths[i]);
      work->items.push_back(std::move(item));
    }
    auto pair_half = std::make_shared<std::string>(std::move(secondary));
    auto dest = std::make_shared<std::string>(std::move(dest_dir));

    const mv::job_id id = pool_.submit_at(
        mv::background_generation,
        [work, dest, pair_half, notify](const mv::job_context&) -> mv::status {
          if (work->kind == file_job_kind::duplicate) {
            auto& item = work->items.front();
            try {
              const auto group = io::duplicate_group(item.path, *pair_half);
              auto r = io::duplicate_files(group, io::native_duplicate_style());
              if (!r) item.status = r.error();
              else item.dest = r.value().front();
            } catch (...) {
              item.status = mv::status::out_of_memory;
            }
          } else if (work->kind == file_job_kind::recycle) {
            for (auto& item : work->items) {
              auto r = io::recycle_file(item.path);
              if (!r) item.status = r.error();
              else item.refused = r.value() == io::recycle_outcome::refused_no_recycle_bin;
            }
          } else {
            const auto how = work->kind == file_job_kind::copy ? io::transfer_kind::copy
                                                               : io::transfer_kind::move;
            // To or from a share: several files at once, a move's verified
            // copy with several requests in flight (docs/design/12 2026-10-01). A
            // card or a local disk keeps one file at a time, in order.
            const io::copy_profile profile = io::batch_copy_profile(
                io::parent_of(work->items.front().path), *dest);
            // Each item is written only by the one thread that copies it.
            io::for_each_in_flight(work->items.size(), profile.files_in_flight,
                                   [&](std::size_t i) {
                                     auto& item = work->items[i];
                                     auto r = io::transfer_file(item.path, *dest, how, profile);
                                     if (!r) item.status = r.error();
                                     else item.dest = r.value();
                                   });
          }
          auto* posted = new (std::nothrow) file_job_result(std::move(*work));
          if (!posted) return mv::status::out_of_memory;
          // The window may already be gone at exit; then nobody owns it.
          if (!::PostMessageW(notify, kFileJobDoneMessage, 0, reinterpret_cast<LPARAM>(posted))) {
            delete posted;
          }
          return mv::status::ok;
        });
    return id != mv::invalid_job;
  } catch (...) {
    return false;
  }
}

}  // namespace mv::shell
