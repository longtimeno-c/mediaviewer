// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/verified_copy.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mv::io {
namespace {

bool cancelled(const std::atomic<bool>* flag) noexcept {
  return flag && flag->load(std::memory_order_relaxed);
}

// The ring between the reading thread and the writer threads. Slot `seq % n`
// holds chunk `seq` until every live writer has written it.
struct ring {
  std::mutex m;
  std::condition_variable cv;
  std::vector<aligned_buffer> buffers;
  std::vector<std::size_t> lengths;
  std::vector<int> pending;  // writers still to consume the slot
  std::uint64_t produced = 0;  // chunks published
  bool finished = false;       // no more chunks
  bool abort = false;          // cancel or source failure: writers stop
};

struct writer_state {
  std::string temp;
  file_writer out;
  bool ok = true;
};

// One destination's writer loop. Consumes every chunk (so the reader never
// waits on a failed writer) but stops writing after its first failure.
void writer_loop(ring& r, writer_state& w, int index, copy_fault* fault) {
  std::uint64_t seq = 0;
  std::uint64_t offset = 0;
  std::vector<std::uint8_t> scratch;
  const std::size_t n = r.buffers.size();
  for (;;) {
    std::size_t slot = 0;
    std::size_t len = 0;
    {
      std::unique_lock lock(r.m);
      r.cv.wait(lock, [&] { return r.abort || r.produced > seq || r.finished; });
      if (r.abort) return;
      if (r.produced <= seq) return;  // finished and drained
      slot = static_cast<std::size_t>(seq % n);
      len = r.lengths[slot];
    }
    if (w.ok) {
      std::span<const std::uint8_t> bytes(r.buffers[slot].data(), len);
      if (fault && fault->target == index && fault->times > 0 && fault->offset >= offset &&
          fault->offset < offset + len) {
        scratch.assign(bytes.begin(), bytes.end());
        scratch[static_cast<std::size_t>(fault->offset - offset)] ^= 0x01;
        --fault->times;
        bytes = scratch;
      }
      if (!w.out.write(bytes)) w.ok = false;
    }
    offset += len;
    {
      std::lock_guard lock(r.m);
      --r.pending[slot];
    }
    r.cv.notify_all();
    ++seq;
  }
}

struct attempt_result {
  content_hash hash;
  std::uint64_t bytes = 0;
  bool source_failed = false;
  bool was_cancelled = false;
  std::vector<copy_target_outcome> outcomes;
};

// Creates the temporary for `final_path`. Empty string if it could not.
std::string create_temp(const std::string& final_path, file_writer& out) {
  for (int attempt = 0; attempt < kTempAttempts; ++attempt) {
    std::string temp = temp_name_for(final_path, attempt);
    auto created = out.create_new(temp);
    if (!created) return {};
    if (*created == rename_outcome::renamed) return temp;
  }
  return {};
}

attempt_result one_attempt(std::string_view src, std::span<const std::string* const> finals,
                           const copy_options& o) {
  attempt_result res;
  res.outcomes.assign(finals.size(), copy_target_outcome::write_failed);

  file_reader in;
  if (!in.open(src, read_mode::sequential)) {
    res.source_failed = true;
    return res;
  }
  std::int64_t src_mtime = 0;
  if (auto st = stat_path(src)) src_mtime = st->mtime_unix;

  std::vector<writer_state> writers(finals.size());
  int live = 0;
  for (std::size_t i = 0; i < finals.size(); ++i) {
    writers[i].temp = create_temp(*finals[i], writers[i].out);
    if (writers[i].temp.empty()) {
      writers[i].ok = false;
    } else {
      ++live;
    }
  }
  if (live == 0) return res;  // every destination failed to open: nothing to read for

  ring r;
  const std::size_t slots = std::clamp<unsigned>(o.buffers_in_flight, 2, 4);
  r.buffers.reserve(slots);
  for (std::size_t i = 0; i < slots; ++i) {
    r.buffers.emplace_back(std::max<std::size_t>(o.buffer_bytes, kIoAlign));
    if (r.buffers.back().empty()) {
      for (auto& w : writers) {
        if (!w.temp.empty()) {
          (void)w.out.close();
          (void)remove_file(w.temp);
        }
      }
      res.source_failed = true;
      return res;
    }
  }
  r.lengths.assign(slots, 0);
  r.pending.assign(slots, 0);

  std::vector<std::thread> threads;
  threads.reserve(finals.size());
  for (std::size_t i = 0; i < finals.size(); ++i) {
    if (!writers[i].ok) continue;
    threads.emplace_back(writer_loop, std::ref(r), std::ref(writers[i]), static_cast<int>(i),
                         o.fault);
  }

  hasher h;
  for (;;) {
    if (cancelled(o.cancel)) {
      res.was_cancelled = true;
      break;
    }
    if (o.yield) o.yield();
    const std::size_t slot = static_cast<std::size_t>(r.produced % slots);
    {
      std::unique_lock lock(r.m);
      r.cv.wait(lock, [&] { return r.pending[slot] == 0; });
    }
    auto got = in.read(std::span<std::uint8_t>(r.buffers[slot].data(), r.buffers[slot].size()));
    if (!got) {
      res.source_failed = true;
      break;
    }
    if (*got == 0) break;
    h.update(std::span<const std::uint8_t>(r.buffers[slot].data(), *got));
    res.bytes += *got;
    if (o.on_progress) o.on_progress(*got);
    {
      std::lock_guard lock(r.m);
      r.lengths[slot] = *got;
      r.pending[slot] = static_cast<int>(threads.size());
      ++r.produced;
    }
    r.cv.notify_all();
    if (*got < r.buffers[slot].size()) break;  // short read: end of file
  }
  {
    std::lock_guard lock(r.m);
    r.finished = true;
    if (res.was_cancelled || res.source_failed) r.abort = true;
  }
  r.cv.notify_all();
  for (auto& t : threads) t.join();
  in.close();
  res.hash = h.finish();

  for (std::size_t i = 0; i < writers.size(); ++i) {
    writer_state& w = writers[i];
    if (w.temp.empty()) continue;
    const bool aborted = res.was_cancelled || res.source_failed;
    if (!aborted && w.ok && o.keep_mtime && src_mtime != 0) (void)w.out.set_mtime(src_mtime);
    const bool flushed = !aborted && w.ok && w.out.flush_durable().has_value();
    const bool closed = w.out.close().has_value();
    if (aborted) {
      (void)remove_file(w.temp);
      res.outcomes[i] = res.was_cancelled ? copy_target_outcome::cancelled
                                          : copy_target_outcome::write_failed;
      continue;
    }
    if (!w.ok || !flushed || !closed) {
      (void)remove_file(w.temp);
      res.outcomes[i] = copy_target_outcome::write_failed;
      continue;
    }
    if (o.read_back) {
      auto back = hash_file(w.temp, read_mode::uncached, o.cancel, o.yield);
      if (!back) {
        (void)remove_file(w.temp);
        res.outcomes[i] = back.error() == status::cancelled ? copy_target_outcome::cancelled
                                                            : copy_target_outcome::verify_failed;
        continue;
      }
      if (!(*back == res.hash)) {
        (void)remove_file(w.temp);
        res.outcomes[i] = copy_target_outcome::verify_failed;
        continue;
      }
    }
    auto renamed = rename_no_replace(w.temp, *finals[i]);
    if (!renamed || *renamed == rename_outcome::name_taken) {
      (void)remove_file(w.temp);
      res.outcomes[i] = renamed ? copy_target_outcome::name_taken : copy_target_outcome::write_failed;
      continue;
    }
    res.outcomes[i] = o.read_back ? copy_target_outcome::verified
                                  : copy_target_outcome::written_unverified;
  }
  return res;
}

}  // namespace

std::string temp_name_for(std::string_view final_utf8, int attempt) {
  std::string out(final_utf8);
  out += ".mvtmp";
  if (attempt > 0) out += std::to_string(attempt + 1);
  return out;
}

result<content_hash> hash_file(std::string_view utf8_path, read_mode mode,
                               const std::atomic<bool>* cancel, const std::function<void()>& yield,
                               const std::function<void(std::uint64_t)>& on_progress) {
  file_reader in;
  MV_TRY_VOID(in.open(utf8_path, mode));
  aligned_buffer buf(4u << 20);
  if (buf.empty()) return err(status::out_of_memory);
  hasher h;
  for (;;) {
    if (cancelled(cancel)) return err(status::cancelled);
    if (yield) yield();
    MV_TRY(const std::size_t got, in.read(std::span<std::uint8_t>(buf.data(), buf.size())));
    if (got == 0) break;
    h.update(std::span<const std::uint8_t>(buf.data(), got));
    if (on_progress) on_progress(got);
    if (got < buf.size()) break;
  }
  return h.finish();
}

result<copy_outcome> verified_copy(std::string_view src_utf8, std::span<const std::string> targets,
                                   const copy_options& options) {
  if (src_utf8.empty() || targets.empty()) return err(status::invalid_arg);
  copy_outcome out;
  out.targets.resize(targets.size());

  // A final name that is already taken is decided up front: nothing is read
  // for it and it is never overwritten.
  std::vector<std::size_t> todo;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    out.targets[i].path_utf8 = targets[i];
    if (stat_path(targets[i])) {
      out.targets[i].outcome = copy_target_outcome::name_taken;
    } else {
      todo.push_back(i);
    }
  }
  if (todo.empty()) return out;

  bool have_hash = false;
  for (int attempt = 0; attempt <= std::max(0, options.retries) && !todo.empty(); ++attempt) {
    std::vector<const std::string*> finals;
    finals.reserve(todo.size());
    for (std::size_t i : todo) finals.push_back(&targets[i]);
    attempt_result a = one_attempt(src_utf8, finals, options);
    if (a.was_cancelled) {
      for (std::size_t i : todo) out.targets[i].outcome = copy_target_outcome::cancelled;
      return err(status::cancelled);
    }
    if (a.source_failed) {
      if (attempt == 0 && !have_hash) return err(status::io);
      for (std::size_t i : todo) out.targets[i].outcome = copy_target_outcome::write_failed;
      break;
    }
    if (have_hash && !(a.hash == out.source_hash)) out.source_unstable = true;
    out.source_hash = a.hash;
    out.bytes = a.bytes;
    have_hash = true;

    std::vector<std::size_t> again;
    for (std::size_t k = 0; k < todo.size(); ++k) {
      const std::size_t i = todo[k];
      out.targets[i].outcome = a.outcomes[k];
      if (attempt > 0) out.targets[i].retried = true;
      if (a.outcomes[k] == copy_target_outcome::write_failed ||
          a.outcomes[k] == copy_target_outcome::verify_failed) {
        again.push_back(i);
      }
    }
    todo = std::move(again);
  }
  return out;
}

}  // namespace mv::io
