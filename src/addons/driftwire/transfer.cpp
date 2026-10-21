// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/transfer.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "core/json.h"
#include "io/collision_name.h"
#include "io/content_hash.h"
#include "io/file_port.h"

namespace mv::driftwire {

namespace {

constexpr int kTempAttempts = 16;
// The receiver waits at most this for its "done" to be acknowledged.
constexpr int kFlushMs = 5000;
// How often a receive with an idle limit looks at the clock (#502).
constexpr int kIdlePollMs = 1000;
constexpr int kRenameAttempts = 32;
// The uncached read-back's buffer (aligned, a kIoAlign multiple).
constexpr std::size_t kReadBackBytes = 4u * 1024 * 1024;
// #530: a receive that may be resumed puts the file it is writing on stable
// storage this often at most, and only once this much more has arrived (a
// power cut then costs at most that much again, never a wrong byte: the
// read-back checks the whole file).
constexpr auto kCheckpointEvery = std::chrono::seconds(5);
constexpr std::uint64_t kCheckpointBytes = 32u * 1024 * 1024;

// Where a transfer ended, if not delivered: the source line and the outcome,
// for the host's log (no path, no name).
void note(const hooks& h, const char* side, int line, outcome o) noexcept {
  if (!h.note || o == outcome::delivered) return;
  try {
    char text[96];
    std::snprintf(text, sizeof text, "driftwire: %s ended %s (transfer.cpp:%d)", side, outcome_name(o), line);
    h.note(text);
  } catch (...) {
  }
}

void note_status(const hooks& h, const char* what, int line, status st) noexcept {
  if (!h.note) return;
  try {
    char text[96];
    std::snprintf(text, sizeof text, "driftwire: %s: %s (transfer.cpp:%d)", what, status_name(st), line);
    h.note(text);
  } catch (...) {
  }
}

bool cancelled(const hooks& h) noexcept { return h.cancel && h.cancel->load(std::memory_order_relaxed); }

std::string json_of(const std::function<void(json::writer&)>& fill) {
  json::writer w;
  w.begin_object();
  fill(w);
  w.end_object();
  return w.take();
}

std::optional<std::int64_t> int_field(std::string_view text, const char* key) {
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  return doc->integer(key);
}

bool exists(const std::string& path) { return io::stat_path(path).has_value(); }

// The free name in `dir` for `leaf`: itself, then "leaf (2).ext" ...
std::string free_name(const std::string& dir, std::string_view leaf) {
  const std::string name =
      io::unique_name(leaf, [&](std::string_view candidate) { return exists(io::join_path(dir, candidate)); });
  return name.empty() ? std::string() : io::join_path(dir, name);
}

// The file being received: its temporary, until it is renamed into place.
struct incoming {
  std::uint32_t index = 0;
  std::uint64_t size = 0;
  std::uint64_t got = 0;
  std::int64_t mtime = 0;
  std::string final_path;
  std::string temp_path;
  io::file_writer out;
  io::hasher hash;
  bool open = false;
  bool hidden = false;   // #530: kept to resume, so hidden until renamed
  bool resumed = false;  // #530: written on from a kept temporary; `hash` has only the new bytes
  std::uint64_t durable = 0;  // #530: bytes on stable storage (the last checkpoint)

  // Removes the temporary: nothing partial stays (docs/design/28), on an
  // error path and on any unwinding alike, unless kept (#530: keep()).
  ~incoming() { drop(); }
  void drop() noexcept {
    if (!open) return;
    (void)out.close();
    (void)io::remove_file(temp_path);
    open = false;
  }
  // #530: the bytes so far to stable storage, the temporary closed and left
  // for a later resume. False (and dropped) when it cannot be made durable.
  bool keep() noexcept {
    if (!open) return false;
    const bool ok = out.flush_durable() && out.close();
    open = false;
    if (!ok) {
      (void)io::remove_file(temp_path);
      return false;
    }
    durable = got;
    return true;
  }
};

// The temporary hashed back from the device, not the page cache.
std::optional<io::content_hash> read_back(const std::string& path, const hooks& h) {
  io::file_reader in;
  if (!in.open(path, io::read_mode::uncached)) return std::nullopt;
  io::aligned_buffer buf(kReadBackBytes);
  if (buf.empty()) return std::nullopt;
  io::hasher hs;
  for (;;) {
    if (cancelled(h)) return std::nullopt;
    auto n = in.read({buf.data(), buf.size()});
    if (!n) return std::nullopt;
    if (*n == 0) break;
    hs.update({buf.data(), *n});
    if (h.yield) h.yield();
  }
  return hs.finish();
}

}  // namespace

const char* outcome_name(outcome o) noexcept {
  switch (o) {
    case outcome::delivered: return "delivered";
    case outcome::declined: return "declined";
    case outcome::busy: return "busy";
    case outcome::no_room: return "no_room";
    case outcome::cancelled: return "cancelled";
    case outcome::failed: return "failed";
  }
  return "failed";
}

std::string temp_name(const std::string& final_path, int attempt) {
  std::string out = final_path + ".mvtmp";
  if (attempt > 0) out += std::to_string(attempt + 1);
  return out;
}

// ---- sender ----------------------------------------------------------------------

std::string key_hex(const std::array<std::uint8_t, 32>& key) {
  char hex[65];
  sodium_bin2hex(hex, sizeof hex, key.data(), key.size());
  return std::string(hex, 64);
}

std::optional<std::array<std::uint8_t, 32>> parse_key_hex(std::string_view hex) {
  if (hex.size() != 64) return std::nullopt;
  std::array<std::uint8_t, 32> key{};
  std::size_t n = 0;
  if (sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(), nullptr, &n, nullptr) != 0 || n != key.size()) {
    return std::nullopt;
  }
  return key;
}

std::string offer_json(const std::vector<outgoing_file>& files, bool handover, bool dht,
                       const std::optional<std::array<std::uint8_t, 32>>& identity) {
  std::uint64_t bytes = 0;
  for (const auto& f : files) bytes += f.size;
  json::writer w;
  w.begin_object().key("count").integer(static_cast<std::int64_t>(files.size()));
  w.key("bytes").integer(static_cast<std::int64_t>(bytes));
  w.key("files").begin_array();
  for (const auto& f : files) {
    w.begin_object().key("rel").string(f.rel).key("size").integer(static_cast<std::int64_t>(f.size));
    w.key("mtime").integer(f.mtime).end_object();
  }
  w.end_array();
  w.key("resume").boolean(true);  // #530: this sender takes "have" and "at"
  if (handover) w.key("handover").boolean(true);
  if (handover && dht) w.key("dht").boolean(true);
  if (identity) w.key("key").string(key_hex(*identity));
  w.end_object();
  return w.take();
}

transfer_result send_files(channel& ch, const std::vector<outgoing_file>& files, const hooks& h,
                           const std::function<void()>& on_accepted) {
  transfer_result res;
  auto stop = [&](outcome o, int line) {
    note(h, "send", line, o);
    res.how = o;
    if (o == outcome::cancelled) (void)ch.cancel(kCancelWaitMs);
    ch.shutdown();
    return res;
  };
  if (files.empty() || files.size() > kMaxFiles) return stop(outcome::failed, __LINE__);
  if (!ch.send(msg::offer, offer_json(files, h.handover, h.handover_dht, h.identity))) return stop(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);

  // The answer, while the person there decides (it declines by itself after
  // kAskMs; this waits a little longer, then gives up).
  message m;
  // #530: what the receiver has already: files whole (not sent) and how far
  // into a file it kept (sent from there).
  std::vector<bool> already(files.size(), false);
  std::vector<std::uint64_t> start_at(files.size(), 0);
  for (;;) {
    if (auto r = ch.receive(m, kAskMs + 30000); !r) {
      return stop(cancelled(h) || r.error() == status::cancelled ? outcome::cancelled : outcome::failed, __LINE__);
    }
    if (m.type == msg::accept) {
      const auto doc = json::parse(m.text(), 4);
      if (const std::string* key = doc ? doc->str("key") : nullptr) res.peer_key = parse_key_hex(*key);
      const json::value* have = doc ? doc->find("have") : nullptr;
      if (have && have->k == json::kind::array) {
        // [first, end): runs of indexes, ascending and apart (any other is
        // passed over, so no accept costs more than one look at each file).
        std::int64_t past = 0;
        for (const json::value& run : have->a) {
          if (run.k != json::kind::array || run.a.size() != 2 || !run.a[0].is_integer || !run.a[1].is_integer) continue;
          const std::int64_t a = run.a[0].i, b = std::min<std::int64_t>(run.a[1].i, static_cast<std::int64_t>(files.size()));
          if (a < past || a >= b) continue;
          past = b;
          for (std::int64_t i = a; i < b; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            already[idx] = true;
            ++res.resumed_files;
            res.resumed_bytes += files[idx].size;
          }
        }
      }
      const json::value* at = doc ? doc->find("at") : nullptr;
      if (at && at->k == json::kind::array) {
        for (const json::value& v : at->a) {
          const auto i = v.integer("i");
          const auto off = v.integer("at");
          if (!i || !off || *i < 0 || static_cast<std::uint64_t>(*i) >= files.size() || *off <= 0) continue;
          const auto idx = static_cast<std::size_t>(*i);
          if (already[idx] || start_at[idx] != 0 || static_cast<std::uint64_t>(*off) >= files[idx].size) continue;
          start_at[idx] = static_cast<std::uint64_t>(*off);
          res.resumed_bytes += start_at[idx];
        }
      }
      break;
    }
    if (m.type == msg::decline) {
      const auto doc = json::parse(m.text(), 4);
      const std::string* why = doc ? doc->str("why") : nullptr;
      res.how = why && *why == "busy" ? outcome::busy : why && *why == "no_room" ? outcome::no_room : outcome::declined;
      ch.shutdown();
      return res;
    }
    if (m.type == msg::cancel) return stop(outcome::declined, __LINE__);
    return stop(outcome::failed, __LINE__);
  }
  if (on_accepted) on_accepted();

  // The receiver's side, read while this thread writes: file_done per file,
  // its final done, or a cancel.
  std::atomic<std::uint32_t> verified{0};
  std::atomic<bool> finished{false}, remote_cancel{false}, remote_error{false}, broken{false};
  std::thread reader([&] {
    message in;
    for (;;) {
      bool got = false;
      try {
        auto r = ch.receive(in);
        got = r.has_value();
        if (!got) note_status(h, "send: reader", __LINE__, r.error());
      } catch (...) {
        got = false;  // out of memory: the link ends, the process does not
      }
      if (!got) {
        broken.store(true);
        ch.shutdown();
        return;
      }
      if (in.type == msg::file_done) {
        bool ok = false;
        try {
          const auto doc = json::parse(in.text(), 4);
          ok = doc && doc->boolean("ok").value_or(false);
        } catch (...) {
        }
        if (ok) verified.fetch_add(1);
      } else if (in.type == msg::done) {
        finished.store(true);
        return;
      } else if (in.type == msg::cancel) {
        // The receiver ended it: its person cancelled, or {"why":"error"}.
        bool error = false;
        try {
          const auto doc = json::parse(in.text(), 4);
          const std::string* why = doc ? doc->str("why") : nullptr;
          error = why && *why == "error";
        } catch (...) {
        }
        remote_error.store(error);
        remote_cancel.store(true);
        ch.shutdown();
        return;
      } else if (in.type == msg::places && h.handover) {
        // #472 PR 5: where the receiver can be reached if the local link goes.
        try {
          if (h.on_places) h.on_places(in.text());
        } catch (...) {
        }
      } else {
        broken.store(true);
        ch.shutdown();
        return;
      }
    }
  });
  auto finish = [&](outcome o, int line) {
    note(h, "send", line, o);
    if (o != outcome::delivered) {
      if (o == outcome::cancelled && !remote_cancel.load()) (void)ch.cancel(kCancelWaitMs);
      ch.shutdown();
    }
    reader.join();
    res.done = verified.load() + res.resumed_files;
    res.how = o;
    if (o == outcome::cancelled && remote_cancel.load()) {
      res.by_peer = true;
      if (remote_error.load()) res.how = outcome::failed;
    }
    if (o == outcome::delivered && (!finished.load() || res.done + res.skipped != files.size())) {
      res.how = outcome::failed;
    }
    ch.shutdown();
    return res;
  };

  std::vector<std::uint8_t> chunk(kChunkBytes);
  std::uint64_t moved = res.resumed_bytes;  // what is there already counts as moved (#530)
  std::uint32_t sent_files = res.resumed_files;
  if (moved > 0 && h.progress) h.progress(moved, sent_files);
  for (std::uint32_t i = 0; i < files.size(); ++i) {
    const outgoing_file& f = files[i];
    if (already[i]) continue;  // whole on the receiver: nothing is said of it
    const std::string idx = json_of([&](json::writer& w) { w.key("i").integer(i); });
    io::file_reader in;
    if (!in.open(f.path, io::read_mode::sequential) || in.size() != f.size) {
      // Gone, unreadable or changed since the offer: said, not sent.
      if (!ch.send(msg::file_skip, idx)) return finish(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);
      ++res.skipped;
      continue;
    }
    const std::string begin = json_of([&](json::writer& w) {
      w.key("i").integer(i).key("size").integer(static_cast<std::int64_t>(f.size)).key("mtime").integer(f.mtime);
      if (start_at[i] != 0) w.key("at").integer(static_cast<std::int64_t>(start_at[i]));
    });
    if (!ch.send(msg::file_begin, begin)) return finish(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);
    io::hasher hs;
    std::uint64_t got = 0;
    // #530: the bytes the receiver kept are read and hashed here, not sent:
    // the hash that ends the file is the whole file's, as always.
    std::uint64_t skip = start_at[i];
    bool readable = true;
    for (;;) {
      if (cancelled(h)) return finish(outcome::cancelled, __LINE__);
      if (remote_cancel.load()) return finish(outcome::cancelled, __LINE__);
      if (broken.load()) return finish(outcome::failed, __LINE__);
      auto n = in.read(chunk);
      if (!n) {
        readable = false;
        break;
      }
      if (*n == 0) break;
      got += *n;
      if (got > f.size) {
        readable = false;  // it grew while we read it
        break;
      }
      hs.update({chunk.data(), *n});
      const std::size_t kept = static_cast<std::size_t>(std::min<std::uint64_t>(skip, *n));
      skip -= kept;
      if (kept == *n) continue;
      const std::span<const std::uint8_t> part(chunk.data() + kept, *n - kept);
      if (auto sent = ch.send(msg::data, part); !sent) {
        note_status(h, "send: data", __LINE__, sent.error());
        return finish(cancelled(h) || remote_cancel.load() ? outcome::cancelled : outcome::failed, __LINE__);
      }
      moved += part.size();
      if (h.progress) h.progress(moved, sent_files);
      if (h.yield) h.yield();
    }
    if (!readable || got != f.size) {
      if (!ch.send(msg::file_skip, idx)) return finish(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);
      ++res.skipped;
      continue;
    }
    const io::content_hash digest = hs.finish();
    if (!ch.send(msg::file_end, std::span<const std::uint8_t>(digest.bytes.data(), digest.bytes.size()))) {
      return finish(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);
    }
    ++sent_files;
    if (h.progress) h.progress(moved, sent_files);
  }
  if (!ch.send(msg::done, std::string_view("{}"))) return finish(outcome::failed, __LINE__);
  // The receiver's done comes after its last read-back and rename.
  reader.join();
  res.done = verified.load() + res.resumed_files;
  res.how = finished.load() && res.done + res.skipped == files.size() ? outcome::delivered
            : remote_cancel.load() && !remote_error.load()            ? outcome::cancelled
                                                                      : outcome::failed;
  res.by_peer = res.how != outcome::delivered && remote_cancel.load();
  ch.shutdown();
  return res;
}

// ---- receiver ---------------------------------------------------------------------

result<incoming_offer> parse_offer(std::string_view text) {
  const auto doc = json::parse(text, 6);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const auto count = doc->integer("count");
  const auto bytes = doc->integer("bytes");
  const json::value* files = doc->find("files");
  if (!count || !bytes || *count <= 0 || *bytes < 0 || static_cast<std::uint64_t>(*count) > kMaxFiles || !files ||
      files->k != json::kind::array || files->a.size() != static_cast<std::size_t>(*count)) {
    return err(status::corrupt);
  }
  incoming_offer o;
  o.files.reserve(files->a.size());
  std::uint64_t sum = 0;
  for (const json::value& f : files->a) {
    const std::string* rel = f.str("rel");
    const auto size = f.integer("size");
    if (!rel || !safe_relative(*rel) || !size || *size < 0 || static_cast<std::uint64_t>(*size) > kMaxFileBytes) {
      return err(status::corrupt);
    }
    // It must equal what the offer says it totals, and never wrap on the way.
    if (sum > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(*size)) {
      return err(status::corrupt);
    }
    sum += static_cast<std::uint64_t>(*size);
    o.files.push_back({*rel, static_cast<std::uint64_t>(*size), f.integer("mtime").value_or(0)});
  }
  if (sum != static_cast<std::uint64_t>(*bytes)) return err(status::corrupt);
  // #498: the shapes a sender makes: files picked one by one (bare names), or
  // one folder and what is in it. Anything else (photos beside a
  // "Library/LaunchAgents/…") is refused whole.
  o.folder = std::string(top_part(o.files.front().rel));
  if (o.folder.size() == o.files.front().rel.size()) o.folder.clear();
  for (const auto& f : o.files) {
    const std::string_view top = top_part(f.rel);
    const bool bare = top.size() == f.rel.size();
    if (o.folder.empty() ? !bare : (bare || top != o.folder)) return err(status::corrupt);
  }
  o.bytes = sum;
  o.handover = doc->boolean("handover").value_or(false);
  o.dht = o.handover && doc->boolean("dht").value_or(false);
  if (const std::string* key = doc->str("key")) o.key = parse_key_hex(*key);
  o.resume = doc->boolean("resume").value_or(false);
  return o;
}

expected send_places(channel& ch, const std::vector<net::endpoint>& places, bool dht) {
  return ch.send(msg::places, json_of([&](json::writer& w) {
    w.key("places").begin_array();
    for (std::size_t i = 0; i < places.size() && i < kMaxPlaces; ++i) {
      w.begin_object().key("a").string(places[i].address).key("p").integer(places[i].port).end_object();
    }
    w.end_array();
    if (dht) w.key("dht").boolean(true);
  }));
}

std::vector<net::endpoint> parse_places(std::string_view text) {
  std::vector<net::endpoint> out;
  const auto doc = json::parse(text, 4);
  const json::value* list = doc ? doc->find("places") : nullptr;
  if (!list || list->k != json::kind::array) return out;
  for (const json::value& p : list->a) {
    if (out.size() == kMaxPlaces) break;
    const std::string* a = p.str("a");
    const auto port = p.integer("p");
    if (!a || !port || *port <= 0 || *port > 65535 || !net::parse_address(*a)) continue;
    out.push_back({*a, static_cast<std::uint16_t>(*port)});
  }
  return out;
}

bool places_dht(std::string_view text) {
  const auto doc = json::parse(text, 4);
  return doc && doc->boolean("dht").value_or(false);
}

result<incoming_offer> read_offer(channel& ch, int timeout_ms) {
  message m;
  if (auto r = ch.receive(m, timeout_ms); !r) return err(r.error());
  if (m.type != msg::offer) return err(status::corrupt);
  return parse_offer(m.text());
}

expected send_accept(channel& ch, const std::optional<std::array<std::uint8_t, 32>>& identity,
                     const resume_plan* plan) {
  return ch.send(msg::accept, json_of([&](json::writer& w) {
    if (identity) w.key("key").string(key_hex(*identity));
    if (!plan || plan->empty()) return;
    // #530: "have", the files already here whole, as runs [first, end) of
    // indexes (plan_resume keeps them within kMaxHaveRuns); "at", how far
    // into one.
    w.key("have").begin_array();
    for (std::size_t k = 0; k < plan->have.size();) {
      std::size_t e = k + 1;
      while (e < plan->have.size() && plan->have[e].index == plan->have[e - 1].index + 1) ++e;
      w.begin_array().integer(plan->have[k].index).integer(plan->have[e - 1].index + 1).end_array();
      k = e;
    }
    w.end_array();
    if (plan->partial) {
      w.key("at").begin_array();
      w.begin_object().key("i").integer(plan->partial->index);
      w.key("at").integer(static_cast<std::int64_t>(plan->partial->at)).end_object();
      w.end_array();
    }
  }));
}

expected send_decline(channel& ch, outcome why) {
  const char* text = why == outcome::busy ? "busy" : why == outcome::no_room ? "no_room" : "declined";
  return ch.send(msg::decline, json_of([&](json::writer& w) { w.key("why").string(text); }));
}

namespace {

// The receiver's verification of whole files, off the thread reading the link
// (#442): flush to stable storage, read back uncached and hash again, rename
// with no-replace, then file_done. While file N is flushed and read back, file
// N+1 is already arriving. Every rule of verified_copy holds for each file
// exactly as before; only the next file no longer waits for it. kVerifiers
// threads take files in order; at most kVerifyAhead wait (each holds an open
// temporary), past which the link reader waits.
class verifier {
 public:
  verifier(channel& ch, const hooks& h) : ch_(ch), h_(h) {
    // A file that arrived whole is checked to the end even after a cancel,
    // as before the pipeline: what finished stays. The yield still applies.
    h_.cancel = nullptr;
    for (int i = 0; i < kVerifiers; ++i) threads_.emplace_back([this] { run(); });
  }
  ~verifier() { finish(); }

  // Waits while kVerifyAhead files are queued. False once a file_done could
  // not be sent (the link is gone).
  bool push(std::unique_ptr<incoming> f, const io::content_hash& sent) {
    std::unique_lock lock(m_);
    room_.wait(lock, [&] { return queue_.size() < kVerifyAhead || broken_; });
    if (broken_) return false;
    pending_.fetch_add(1);
    queue_.push_back({std::move(f), sent});
    work_.notify_one();
    return true;
  }
  // Every queued file verified (or dropped); the threads end.
  void finish() {
    {
      std::lock_guard lock(m_);
      if (closing_) return;
      closing_ = true;
      work_.notify_all();
    }
    for (auto& t : threads_) t.join();
  }
  [[nodiscard]] std::uint32_t done() const { return done_.load(); }
  // A file queued or being read back (#502: the receive is not idle).
  [[nodiscard]] bool checking() const { return pending_.load() != 0; }
  [[nodiscard]] std::uint32_t failed() const { return failed_.load(); }
  [[nodiscard]] bool broken() {
    std::lock_guard lock(m_);
    return broken_;
  }
  // In index order, with the files that were here already (#530).
  std::vector<std::string> take_written(const std::vector<resume_plan::whole_file>& already) {
    std::lock_guard lock(m_);
    if (!already_added_) {
      for (const auto& f : already) written_.emplace_back(f.index, f.final_path);
      already_added_ = true;
    }
    std::sort(written_.begin(), written_.end());
    std::vector<std::string> out;
    out.reserve(written_.size());
    for (const auto& [i, path] : written_) out.push_back(path);  // copied: a record of what is kept may follow (#530)
    return out;
  }
  // #530: the files verified here so far (not those that were here before),
  // for a record to resume from.
  std::vector<resume_plan::whole_file> written_now(const std::vector<bool>& had) {
    std::lock_guard lock(m_);
    std::vector<resume_plan::whole_file> out;
    out.reserve(written_.size());
    for (const auto& [i, path] : written_)
      if (i >= had.size() || !had[i]) out.push_back({i, path});
    return out;
  }

 private:
  static constexpr int kVerifiers = 2;
  static constexpr std::size_t kVerifyAhead = 4;
  struct job {
    std::unique_ptr<incoming> f;
    io::content_hash sent;
  };

  void run() {
    for (;;) {
      job j;
      {
        std::unique_lock lock(m_);
        work_.wait(lock, [&] { return closing_ || !queue_.empty(); });
        if (queue_.empty()) return;
        j = std::move(queue_.front());
        queue_.pop_front();
        room_.notify_all();
      }
      bool ok = false;
      try {
        ok = verify(*j.f, j.sent);
      } catch (...) {
        ok = false;
      }
      if (ok) {
        j.f->open = false;  // renamed into place: the temporary is the final file
        done_.fetch_add(1);
        std::lock_guard lock(m_);
        written_.emplace_back(j.f->index, j.f->final_path);
      } else {
        j.f->drop();
        failed_.fetch_add(1);
      }
      if (h_.progress) h_.progress(moved_hint_.load(), done_.load());
      const std::uint32_t i = j.f->index;
      if (!ch_.send(msg::file_done, json_of([&](json::writer& w) { w.key("i").integer(i).key("ok").boolean(ok); }))) {
        std::lock_guard lock(m_);
        broken_ = true;
        room_.notify_all();
      }
      pending_.fetch_sub(1);
    }
  }

  // Flushed, read back from the device, renamed never over anything.
  bool verify(incoming& f, const io::content_hash& sent) {
    if (!f.out.flush_durable()) return false;
    if (f.mtime != 0) (void)f.out.set_mtime(f.mtime);
    if (!f.out.close()) return false;
    const auto back = read_back(f.temp_path, h_);
    if (!back || !(*back == sent)) return false;
    // #530: shown again before its rename (the flag travels with it).
    if (f.hidden) (void)io::set_hidden(f.temp_path, false);
    const std::string dir(io::parent_of(f.final_path));
    const std::string leaf(io::file_name_of(f.final_path));
    for (int a = 0; a < kRenameAttempts; ++a) {
      auto r = io::rename_no_replace(f.temp_path, f.final_path);
      if (!r) return false;
      if (*r == io::rename_outcome::renamed) return true;
      // A name taken since it was chosen (by another file of this offer still
      // being verified, or by anyone): keep both.
      f.final_path = free_name(dir, leaf);
      if (f.final_path.empty()) return false;
    }
    return false;
  }

 public:
  std::atomic<std::uint64_t> moved_hint_{0};

 private:
  channel& ch_;
  hooks h_;
  std::mutex m_;
  std::condition_variable work_, room_;
  std::deque<job> queue_;
  bool closing_ = false;
  bool broken_ = false;
  bool already_added_ = false;
  std::vector<std::pair<std::uint32_t, std::string>> written_;
  std::atomic<std::uint32_t> done_{0}, failed_{0};
  std::atomic<int> pending_{0};
  std::vector<std::thread> threads_;
};

}  // namespace

transfer_result receive_files(channel& ch, const incoming_offer& offer, const std::string& dest_dir, const hooks& h,
                              const resume_plan* plan) {
  transfer_result res;
  std::unique_ptr<incoming> cur;  // the file being written, if any
  std::uint32_t next = 0;
  std::uint32_t failed = 0;  // files whose streamed hash did not match
  std::uint64_t moved = 0;
  verifier checks(ch, h);
  // #530: what is here already (said in the accept), and keeping what a
  // receive that fails holds, for a later one to take up.
  static const std::vector<resume_plan::whole_file> kNone;
  const std::vector<resume_plan::whole_file>& already = plan ? plan->have : kNone;
  std::vector<bool> have(offer.files.size(), false);
  for (const auto& f : already) {
    if (f.index < have.size() && !have[f.index]) {
      have[f.index] = true;
      ++res.resumed_files;
      res.resumed_bytes += offer.files[f.index].size;
    }
  }
  const std::optional<resume_plan::partial_file> partial =
      plan && plan->partial && plan->partial->index < offer.files.size() && !have[plan->partial->index]
          ? plan->partial
          : std::nullopt;
  if (partial) res.resumed_bytes += partial->at;
  moved = res.resumed_bytes;
  const bool keeping = static_cast<bool>(h.keep);
  std::string landed;  // the sent folder's name here, when the offer is one folder
  auto skip_had = [&] {
    while (next < offer.files.size() && have[next]) ++next;
  };
  auto kept_now = [&](bool with_partial) {
    kept_state k;
    k.landed = landed;
    k.whole = already;
    for (auto& w : checks.written_now(have)) k.whole.push_back(std::move(w));
    if (with_partial && cur && cur->durable > 0) {
      k.partial = resume_plan::partial_file{cur->index, cur->durable, cur->temp_path, cur->final_path};
    }
    return k;
  };
  // At most every second while files arrive; the partial checkpointed (to
  // stable storage) at most every few seconds and only once 32 MB more is in.
  auto last_kept = std::chrono::steady_clock::now();
  auto last_flush = last_kept;
  std::uint32_t kept_done = 0;
  auto keep_as_we_go = [&] {
    if (!keeping) return;
    const auto now = std::chrono::steady_clock::now();
    bool changed = checks.done() != kept_done;
    if (cur && cur->open && cur->got - cur->durable >= kCheckpointBytes && now - last_flush >= kCheckpointEvery) {
      last_flush = now;
      if (cur->out.flush_durable()) {
        cur->durable = cur->got;
        changed = true;
      }
    }
    if (!changed || now - last_kept < std::chrono::seconds(1)) return;
    last_kept = now;
    kept_done = checks.done();
    try {
      h.keep(kept_now(true));
    } catch (...) {
    }
  };
  auto settle = [&] {
    checks.finish();
    res.done = checks.done() + res.resumed_files;
    res.written = checks.take_written(already);
    failed += checks.failed();
  };
  auto end = [&](outcome o, bool tell, int line) {
    note(h, "receive", line, o);
    // #530: a receive that failed (not one a person cancelled; MediaViewer
    // quitting is not that) keeps the file it was writing for a later
    // resume; otherwise nothing partial stays.
    const bool stopped = o == outcome::failed || (o == outcome::cancelled && h.quitting && h.quitting->load());
    const bool keep = keeping && stopped && cur && cur->keep();
    if (cur && !keep) cur->drop();
    // Said reliably and final (#511): the sender stops, and takes it for
    // neither a lost link nor this person's cancel when it is a failure.
    if (tell) (void)ch.cancel(kCancelWaitMs, o == outcome::cancelled ? std::string_view() : std::string_view("error"));
    ch.shutdown();
    // Files already whole finish their checks: what arrived whole stays.
    settle();
    if (keeping && stopped) {
      try {
        h.keep(kept_now(keep));
      } catch (...) {
      }
    }
    res.how = o;
    return res;
  };

  // #498: the destination is the person's own choice; below it nothing goes
  // through a link, and a sent folder never merges into one already there
  // ("Trip" beside an existing "Trip" lands as "Trip (2)") but the one a
  // resumed receive of it made (#530).
  if (!io::make_directories(dest_dir)) return end(outcome::failed, true, __LINE__);
  if (!offer.folder.empty()) {
    if (plan && !plan->landed.empty() && top_part(plan->landed).size() == plan->landed.size() &&
        safe_relative(plan->landed)) {
      const auto st = io::stat_path(io::join_path(dest_dir, plan->landed));
      if (st && st->is_directory) landed = plan->landed;
    }
    if (landed.empty()) landed = std::string(io::file_name_of(free_name(dest_dir, offer.folder)));
    if (landed.empty() || !io::make_directories_beneath(dest_dir, landed)) {
      return end(outcome::failed, true, __LINE__);
    }
  }
  const auto latest_mtime = static_cast<std::int64_t>(std::time(nullptr)) + 24 * 60 * 60;
  if (moved > 0 && h.progress) h.progress(moved, res.resumed_files);

  message m;
  // #502: the last time the files moved (a message, or a file read back).
  auto moving = std::chrono::steady_clock::now();
  for (;;) {
    if (cancelled(h)) return end(outcome::cancelled, true, __LINE__);
    if (checks.broken()) return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
    if (auto r = ch.receive(m, h.idle_ms > 0 ? kIdlePollMs : -1); !r) {
      if (r.error() == status::timeout && h.idle_ms > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (checks.checking()) moving = now;
        if (now - moving < std::chrono::milliseconds(h.idle_ms)) continue;
        return end(outcome::failed, true, __LINE__);  // a sender that holds the slot and sends nothing
      }
      note_status(h, "receive: link", __LINE__, r.error());
      return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
    }
    moving = std::chrono::steady_clock::now();
    switch (m.type) {
      case msg::file_begin: {
        skip_had();  // the files said to be here already never come
        const auto doc = json::parse(m.text(), 4);
        const auto i = doc ? doc->integer("i") : std::nullopt;
        const auto size = doc ? doc->integer("size") : std::nullopt;
        const auto mtime = doc ? doc->integer("mtime") : std::nullopt;
        const std::int64_t at = doc ? doc->integer("at").value_or(0) : 0;
        if ((cur && cur->open) || !i || *i != static_cast<std::int64_t>(next) || next >= offer.files.size() || !size ||
            static_cast<std::uint64_t>(*size) != offer.files[next].size) {
          return end(outcome::failed, true, __LINE__);
        }
        // #530: from part way only where this side said so, and exactly there.
        const bool resuming = partial && partial->index == next;
        if (at != (resuming ? static_cast<std::int64_t>(partial->at) : 0)) return end(outcome::failed, true, __LINE__);
        const std::string& rel = offer.files[next].rel;
        const auto slash = rel.rfind('/');
        std::string rel_dir = slash == std::string::npos ? std::string() : rel.substr(0, slash);
        if (!landed.empty() && top_part(rel) == offer.folder) rel_dir = landed + rel_dir.substr(offer.folder.size());
        auto made_dir = rel_dir.empty() ? result<std::string>(dest_dir) : io::make_directories_beneath(dest_dir, rel_dir);
        if (!made_dir) {
          note_status(h, "receive: folder", __LINE__, made_dir.error());
          return end(outcome::failed, true, __LINE__);
        }
        const std::string& dir = *made_dir;
        cur = std::make_unique<incoming>();
        cur->index = next;
        cur->size = static_cast<std::uint64_t>(*size);
        // The sender's clock says when it was made: never before 1970 or after tomorrow.
        cur->mtime = std::clamp<std::int64_t>(mtime.value_or(0), 0, latest_mtime);
        if (resuming) {
          // The kept temporary, cut to the bytes known durable and written on
          // from there; the read-back after file_end checks the whole file.
          // Its final name is chosen again then if it has been taken since.
          cur->final_path = partial->final_path;
          cur->temp_path = partial->temp_path;
          if (io::parent_of(cur->temp_path) != dir || io::parent_of(cur->final_path) != dir ||
              !cur->out.open_resume(cur->temp_path, partial->at)) {
            return end(outcome::failed, true, __LINE__);
          }
          cur->got = cur->durable = partial->at;
          cur->resumed = true;
          cur->hidden = true;
          cur->open = true;
          break;
        }
        cur->final_path = free_name(dir, leaf_name(rel));
        if (cur->final_path.empty()) return end(outcome::failed, true, __LINE__);
        bool made = false;
        for (int a = 0; a < kTempAttempts && !made; ++a) {
          cur->temp_path = temp_name(cur->final_path, a);
          auto c = cur->out.create_new(cur->temp_path);
          if (!c) return end(outcome::failed, true, __LINE__);
          made = *c == io::rename_outcome::renamed;
        }
        if (!made) return end(outcome::failed, true, __LINE__);
        // #498: from another computer, so Gatekeeper / SmartScreen check it
        // before it runs (an app in a zip inherits the mark when extracted).
        // Best effort, as a browser's: a FAT card keeps no mark.
        (void)io::mark_from_elsewhere(cur->temp_path, "MediaViewer Driftwire");
        // #530: a temporary that may be kept is hidden, so a half-received
        // file is not taken for the finished one.
        if (keeping) cur->hidden = io::set_hidden(cur->temp_path, true).has_value();
        cur->open = true;
        break;
      }
      case msg::data: {
        if (!cur || !cur->open || cur->got + m.body.size() > cur->size) return end(outcome::failed, true, __LINE__);
        if (!cur->out.write(m.body)) return end(outcome::failed, true, __LINE__);
        cur->hash.update(m.body);
        cur->got += m.body.size();
        moved += m.body.size();
        checks.moved_hint_.store(moved);
        if (h.progress) h.progress(moved, checks.done() + res.resumed_files);
        keep_as_we_go();
        if (h.yield) h.yield();
        break;
      }
      case msg::file_end: {
        if (!cur || !cur->open || m.body.size() != 32 || cur->got != cur->size) return end(outcome::failed, true, __LINE__);
        io::content_hash sent;
        std::memcpy(sent.bytes.data(), m.body.data(), 32);
        ++next;
        // A resumed file's streamed hash has only the bytes sent this time:
        // its read-back is the check (as for every file).
        if (cur->resumed || cur->hash.finish() == sent) {
          // Flushed, read back and renamed on the verifier while the next
          // file arrives; its file_done goes from there.
          if (!checks.push(std::move(cur), sent)) {
            return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
          }
        } else {
          const std::uint32_t i = cur->index;
          cur->drop();
          cur.reset();
          ++failed;
          if (!ch.send(msg::file_done, json_of([&](json::writer& w) { w.key("i").integer(i).key("ok").boolean(false); }))) {
            return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
          }
        }
        keep_as_we_go();
        break;
      }
      case msg::file_skip: {
        skip_had();
        const auto i = int_field(m.text(), "i");
        if (!i || *i != static_cast<std::int64_t>(next)) return end(outcome::failed, true, __LINE__);
        if (cur) cur->drop();
        ++next;
        ++res.skipped;
        break;
      }
      case msg::done: {
        skip_had();
        if ((cur && cur->open) || next != offer.files.size()) return end(outcome::failed, true, __LINE__);
        // The last files' checks finish before done: the sender's done means
        // every file is in place or reported.
        settle();
        if (checks.broken()) {
          ch.shutdown();
          res.how = cancelled(h) ? outcome::cancelled : outcome::failed;
          return res;
        }
        if (h.progress) h.progress(moved, res.done);
        (void)ch.send(msg::done, std::string_view("{}"));
        // Protocol 2: until the sender has it (a link that drops now is
        // rejoined first), so the sender never waits out a lost "done".
        ch.flush(kFlushMs);
        ch.shutdown();
        res.how = failed == 0 ? outcome::delivered : outcome::failed;
        return res;
      }
      case msg::cancel:
        res.by_peer = true;  // the sender ended it
        return end(outcome::cancelled, false, __LINE__);
      default:
        return end(outcome::failed, true, __LINE__);
    }
  }
}

}  // namespace mv::driftwire
