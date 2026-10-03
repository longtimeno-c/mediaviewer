// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/verified_copy.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "io/volume.h"

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
  // Atomic: on the deep path several writer threads share one destination.
  std::atomic<bool> ok{true};
};

// Flips the fault's byte if it falls in [offset, offset + bytes.size()).
// Returns the bytes to write: `bytes`, or `scratch` holding the flipped copy.
std::span<const std::uint8_t> maybe_fault(copy_fault* fault, int index, std::uint64_t offset,
                                          std::span<const std::uint8_t> bytes,
                                          std::vector<std::uint8_t>& scratch) {
  // Offset before `times`: on the deep path only the one thread holding the
  // faulted chunk may touch the counter.
  if (fault && fault->target == index && fault->offset >= offset &&
      fault->offset < offset + bytes.size() && fault->times > 0) {
    scratch.assign(bytes.begin(), bytes.end());
    scratch[static_cast<std::size_t>(fault->offset - offset)] ^= 0x01;
    --fault->times;
    return scratch;
  }
  return bytes;
}

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
      const auto bytes = maybe_fault(fault, index, offset,
                                     std::span<const std::uint8_t>(r.buffers[slot].data(), len),
                                     scratch);
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

// ---------------------------------------------------------------------------
// The deep path (read_depth / write_depth > 1, a network share). Chunk k is
// the bytes at k * chunk; it lives in slot k % n, and slot s serves chunks s,
// s + n, s + 2n ... in turn. Reader threads claim chunks in order and read
// them at their offsets; this thread hashes them in order; each destination's
// writer threads write them at their offsets. A slot is free again once its
// chunk is hashed and every live destination has written it, so the readers
// run at most n chunks ahead of the hash and the caller's yield between
// chunks throttles the whole pipeline.

struct deep_slot {
  std::uint64_t turn = 0;  // the next chunk this slot serves
  bool busy = false;       // holds chunk `turn`
  bool read = false;       // its bytes are in
  bool hashed = false;
  int writes_left = 0;
  std::size_t len = 0;
};

struct deep_ring {
  std::mutex m;
  std::condition_variable cv;
  std::vector<aligned_buffer> buffers;
  std::vector<deep_slot> slots;
  std::uint64_t chunks = 0;
  std::size_t chunk_bytes = 0;
  std::uint64_t next_read = 0;
  std::vector<std::uint64_t> next_write;  // per destination
  int live_writers = 0;
  bool abort = false;
  bool source_failed = false;
};

// Under r.m.
void release_if_done(deep_ring& r, deep_slot& s) {
  if (!s.busy || !s.hashed || s.writes_left > 0) return;
  s.busy = false;
  s.read = false;
  s.hashed = false;
  s.turn += r.slots.size();
}

void deep_reader(deep_ring& r, file_reader& in, std::uint64_t size, bool aligned_tail) {
  const std::size_t n = r.slots.size();
  for (;;) {
    std::uint64_t k = 0;
    std::size_t slot = 0;
    {
      std::unique_lock lock(r.m);
      if (r.abort || r.next_read >= r.chunks) return;
      k = r.next_read++;
      slot = static_cast<std::size_t>(k % n);
      r.cv.wait(lock, [&] { return r.abort || (!r.slots[slot].busy && r.slots[slot].turn == k); });
      if (r.abort) return;
      r.slots[slot].busy = true;
    }
    const std::uint64_t offset = k * r.chunk_bytes;
    const std::size_t want =
        static_cast<std::size_t>(std::min<std::uint64_t>(r.chunk_bytes, size - offset));
    // An uncached read asks for whole sectors; the file's end cuts it short.
    const std::size_t ask = aligned_tail ? ((want + kIoAlign - 1) / kIoAlign) * kIoAlign : want;
    auto got = in.read_at(offset, std::span<std::uint8_t>(r.buffers[slot].data(), ask));
    {
      std::lock_guard lock(r.m);
      if (!got || *got < want) {
        // An error, or the file is shorter than when it was opened.
        r.source_failed = true;
        r.abort = true;
      } else {
        deep_slot& s = r.slots[slot];
        s.len = want;
        s.read = true;
        s.writes_left = r.live_writers;
      }
    }
    r.cv.notify_all();
  }
}

void deep_writer(deep_ring& r, std::size_t dest, writer_state& w, int index, copy_fault* fault) {
  const std::size_t n = r.slots.size();
  std::vector<std::uint8_t> scratch;
  for (;;) {
    std::uint64_t k = 0;
    std::size_t slot = 0;
    std::size_t len = 0;
    {
      std::unique_lock lock(r.m);
      if (r.abort || r.next_write[dest] >= r.chunks) return;
      k = r.next_write[dest]++;
      slot = static_cast<std::size_t>(k % n);
      r.cv.wait(lock, [&] {
        return r.abort || (r.slots[slot].busy && r.slots[slot].turn == k && r.slots[slot].read);
      });
      if (r.abort) return;
      len = r.slots[slot].len;
    }
    if (w.ok) {
      const std::uint64_t offset = k * r.chunk_bytes;
      // Only the writer holding the faulted chunk of the faulted target gets
      // here with a match, so `fault` is touched by one thread per attempt.
      const auto bytes = maybe_fault(fault, index, offset,
                                     std::span<const std::uint8_t>(r.buffers[slot].data(), len),
                                     scratch);
      if (!w.out.write_at(offset, bytes)) w.ok = false;
    }
    {
      std::lock_guard lock(r.m);
      deep_slot& s = r.slots[slot];
      --s.writes_left;
      release_if_done(r, s);
    }
    r.cv.notify_all();
  }
}

struct deep_outcome {
  content_hash hash;
  std::uint64_t bytes = 0;
  bool source_failed = false;
  bool was_cancelled = false;
};

// Reads `in` (opened concurrent) through the deep ring, hashing in order, and
// writes every chunk to each writer whose `ok` is set. No writers: a hash only.
deep_outcome run_deep(file_reader& in, bool aligned_tail, std::size_t chunk_bytes,
                      unsigned read_depth, unsigned write_depth,
                      std::span<writer_state* const> writers, std::span<const std::size_t> indices,
                      const std::atomic<bool>* cancel, const std::function<void()>& yield,
                      const std::function<void(std::uint64_t)>& on_progress, copy_fault* fault) {
  deep_outcome out;
  const std::uint64_t size = in.size();
  read_depth = std::clamp(read_depth, 1u, kMaxIoDepth);
  write_depth = std::clamp(write_depth, 1u, kMaxIoDepth);

  deep_ring r;
  r.chunk_bytes = ((std::max<std::size_t>(chunk_bytes, kIoAlign) + kIoAlign - 1) / kIoAlign) *
                  kIoAlign;
  r.chunks = (size + r.chunk_bytes - 1) / r.chunk_bytes;
  const std::size_t n = std::max(read_depth, write_depth) + 2;
  r.buffers.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    r.buffers.emplace_back(r.chunk_bytes);
    if (r.buffers.back().empty()) {
      out.source_failed = true;
      return out;
    }
  }
  r.slots.resize(n);
  for (std::size_t i = 0; i < n; ++i) r.slots[i].turn = i;
  r.next_write.assign(writers.size(), 0);
  for (writer_state* w : writers) {
    if (w->ok) ++r.live_writers;
  }

  std::vector<std::thread> threads;
  const auto readers = static_cast<unsigned>(std::min<std::uint64_t>(read_depth, r.chunks));
  for (unsigned i = 0; i < readers; ++i) {
    threads.emplace_back(deep_reader, std::ref(r), std::ref(in), size, aligned_tail);
  }
  const auto per_dest = static_cast<unsigned>(std::min<std::uint64_t>(write_depth, r.chunks));
  for (std::size_t d = 0; d < writers.size(); ++d) {
    if (!writers[d]->ok) continue;
    for (unsigned i = 0; i < per_dest; ++i) {
      threads.emplace_back(deep_writer, std::ref(r), d, std::ref(*writers[d]),
                           static_cast<int>(indices[d]), fault);
    }
  }
  // A destination that failed to open still owes its share of each slot:
  // writes_left counts live writers only, so nothing waits on it.

  hasher h;
  for (std::uint64_t k = 0; k < r.chunks; ++k) {
    if (cancelled(cancel)) {
      out.was_cancelled = true;
      break;
    }
    if (yield) yield();
    const std::size_t slot = static_cast<std::size_t>(k % n);
    std::size_t len = 0;
    {
      std::unique_lock lock(r.m);
      r.cv.wait(lock, [&] {
        return r.abort || (r.slots[slot].busy && r.slots[slot].turn == k && r.slots[slot].read);
      });
      if (r.abort) break;
      len = r.slots[slot].len;
    }
    h.update(std::span<const std::uint8_t>(r.buffers[slot].data(), len));
    out.bytes += len;
    if (on_progress) on_progress(len);
    {
      std::lock_guard lock(r.m);
      deep_slot& s = r.slots[slot];
      s.hashed = true;
      release_if_done(r, s);
    }
    r.cv.notify_all();
  }
  {
    std::lock_guard lock(r.m);
    if (out.was_cancelled) r.abort = true;
    out.source_failed = r.source_failed;
    if (out.bytes < size && !out.was_cancelled) r.abort = true;
  }
  r.cv.notify_all();
  for (auto& t : threads) t.join();
  {
    std::lock_guard lock(r.m);
    out.source_failed = out.source_failed || r.source_failed;
  }
  out.hash = h.finish();
  return out;
}

struct attempt_result {
  content_hash hash;
  std::uint64_t bytes = 0;
  bool source_failed = false;
  bool was_cancelled = false;
  std::vector<copy_target_outcome> outcomes;
};

// Creates the temporary for `final_path`. Empty string if it could not.
std::string create_temp(const std::string& final_path, file_writer& out, bool concurrent) {
  for (int attempt = 0; attempt < kTempAttempts; ++attempt) {
    std::string temp = temp_name_for(final_path, attempt);
    auto created = out.create_new(temp, concurrent);
    if (!created) return {};
    if (*created == rename_outcome::renamed) return temp;
  }
  return {};
}

void drop_temps(std::vector<writer_state>& writers) {
  for (auto& w : writers) {
    if (!w.temp.empty()) {
      (void)w.out.close();
      (void)remove_file(w.temp);
    }
  }
}

// The sequential data phase: this thread reads, one writer thread per
// destination writes. Fills `res` hash / bytes / source_failed / was_cancelled.
void run_sequential(file_reader& in, std::vector<writer_state>& writers,
                    std::span<const std::size_t> indices, const copy_options& o,
                    attempt_result& res) {
  ring r;
  const std::size_t slots = std::clamp<unsigned>(o.buffers_in_flight, 2, 4);
  r.buffers.reserve(slots);
  for (std::size_t i = 0; i < slots; ++i) {
    r.buffers.emplace_back(std::max<std::size_t>(o.buffer_bytes, kIoAlign));
    if (r.buffers.back().empty()) {
      res.source_failed = true;
      return;
    }
  }
  r.lengths.assign(slots, 0);
  r.pending.assign(slots, 0);

  std::vector<std::thread> threads;
  threads.reserve(writers.size());
  for (std::size_t i = 0; i < writers.size(); ++i) {
    if (!writers[i].ok) continue;
    threads.emplace_back(writer_loop, std::ref(r), std::ref(writers[i]),
                         static_cast<int>(indices[i]), o.fault);
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
  res.hash = h.finish();
}

// `indices[i]` is finals[i]'s position in the caller's target list: a retry
// copies only the targets that failed, and a fault is aimed by that position.
attempt_result one_attempt(std::string_view src, std::span<const std::string* const> finals,
                           std::span<const std::size_t> indices, const copy_options& o) {
  attempt_result res;
  res.outcomes.assign(finals.size(), copy_target_outcome::write_failed);
  const bool deep = o.read_depth > 1 || o.write_depth > 1;

  file_reader in;
  if (!in.open(src, read_mode::sequential, deep)) {
    res.source_failed = true;
    return res;
  }
  std::int64_t src_mtime = 0;
  if (auto st = stat_path(src)) src_mtime = st->mtime_unix;

  std::vector<writer_state> writers(finals.size());
  int live = 0;
  for (std::size_t i = 0; i < finals.size(); ++i) {
    writers[i].temp = create_temp(*finals[i], writers[i].out, deep);
    if (writers[i].temp.empty()) {
      writers[i].ok = false;
      continue;
    }
    // Sized up front: no positional write extends the file.
    if (deep && in.size() > 0 && !writers[i].out.set_size(in.size())) {
      writers[i].ok = false;  // the result loop removes its temporary
      continue;
    }
    ++live;
  }
  if (live == 0) {
    drop_temps(writers);
    return res;  // every destination failed to open: nothing to read for
  }

  if (deep) {
    std::vector<writer_state*> ptrs;
    ptrs.reserve(writers.size());
    for (auto& w : writers) ptrs.push_back(&w);
    const deep_outcome d = run_deep(in, false, o.buffer_bytes, o.read_depth, o.write_depth, ptrs,
                                    indices, o.cancel, o.yield, o.on_progress, o.fault);
    res.hash = d.hash;
    res.bytes = d.bytes;
    res.source_failed = d.source_failed;
    res.was_cancelled = d.was_cancelled;
  } else {
    run_sequential(in, writers, indices, o, res);
  }
  in.close();

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
      auto back = hash_file(w.temp, read_mode::uncached, o.cancel, o.yield, {},
                            deep ? std::max(o.read_depth, o.write_depth) : 1u, o.buffer_bytes);
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

copy_profile copy_profile_for(std::string_view src_utf8, std::string_view dest_utf8) noexcept {
  copy_profile p;
  if (is_network_path(src_utf8)) p.read_depth = kNetworkIoDepth;
  if (is_network_path(dest_utf8)) p.write_depth = kNetworkIoDepth;
  if (p.read_depth > 1 || p.write_depth > 1) p.buffer_bytes = kNetworkChunkBytes;
  return p;
}

copy_profile batch_copy_profile(std::string_view src_dir_utf8, std::string_view dest_dir_utf8) {
  copy_profile p = copy_profile_for(src_dir_utf8, dest_dir_utf8);
  if (p.read_depth == 1 && p.write_depth == 1) return p;  // no share at either end
  // A card is read one request at a time, one file at a time (plan/18).
  const auto vol = volume_of(src_dir_utf8);
  if (vol && vol->removable) {
    p.read_depth = 1;
    return p;
  }
  p.files_in_flight = kNetworkFilesInFlight;
  return p;
}

result<content_hash> hash_file(std::string_view utf8_path, read_mode mode,
                               const std::atomic<bool>* cancel, const std::function<void()>& yield,
                               const std::function<void(std::uint64_t)>& on_progress,
                               unsigned read_depth, std::size_t buffer_bytes) {
  file_reader in;
  if (read_depth > 1) {
    MV_TRY_VOID(in.open(utf8_path, mode, true));
    const deep_outcome d = run_deep(in, mode == read_mode::uncached, buffer_bytes, read_depth, 1,
                                    {}, {}, cancel, yield, on_progress, nullptr);
    if (d.was_cancelled) return err(status::cancelled);
    if (d.source_failed) return err(status::io);
    return d.hash;
  }
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
    attempt_result a = one_attempt(src_utf8, finals, todo, options);
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
