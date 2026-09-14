// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/file_jobs.h"

#include <memory>
#include <new>

#include "io/file_ops.h"

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
  if (kind != file_job_kind::recycle && dest_dir.empty()) return false;
  try {
    // job_fn is a std::function, so what it captures must be copyable.
    auto work = std::make_shared<file_job_result>();
    work->kind = kind;
    work->token = token;
    work->items.reserve(paths.size());
    for (auto& p : paths) {
      file_job_item item;
      item.path = std::move(p);
      work->items.push_back(std::move(item));
    }
    auto dest = std::make_shared<std::string>(std::move(dest_dir));

    const mv::job_id id = pool_.submit_at(
        mv::background_generation, [work, dest, notify](const mv::job_context&) -> mv::status {
          for (auto& item : work->items) {
            if (work->kind == file_job_kind::recycle) {
              auto r = io::recycle_file(item.path);
              if (!r) item.status = r.error();
              else item.refused = r.value() == io::recycle_outcome::refused_no_recycle_bin;
            } else {
              const auto how = work->kind == file_job_kind::copy ? io::transfer_kind::copy
                                                                 : io::transfer_kind::move;
              auto r = io::transfer_file(item.path, *dest, how);
              if (!r) item.status = r.error();
              else item.dest = r.value();
            }
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
