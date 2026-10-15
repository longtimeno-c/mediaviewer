// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/transfer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>

#include "core/json.h"
#include "io/collision_name.h"
#include "io/content_hash.h"
#include "io/file_port.h"

namespace mv::sharepoint {

namespace {

constexpr int kTempAttempts = 16;
constexpr int kRenameAttempts = 32;
// The uncached read-back's buffer (aligned, a kIoAlign multiple).
constexpr std::size_t kReadBackBytes = 4u * 1024 * 1024;

// Where a transfer ended, if not delivered: the source line and the outcome,
// for the host's log (no path, no name).
void note(const hooks& h, const char* side, int line, outcome o) noexcept {
  if (!h.note || o == outcome::delivered) return;
  try {
    char text[96];
    std::snprintf(text, sizeof text, "sharepoint: %s ended %s (transfer.cpp:%d)", side, outcome_name(o), line);
    h.note(text);
  } catch (...) {
  }
}

void note_status(const hooks& h, const char* what, int line, status st) noexcept {
  if (!h.note) return;
  try {
    char text[96];
    std::snprintf(text, sizeof text, "sharepoint: %s: %s (transfer.cpp:%d)", what, status_name(st), line);
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

  // Removes the temporary: nothing partial stays (docs/design/28), on an
  // error path and on any unwinding alike.
  ~incoming() { drop(); }
  void drop() noexcept {
    if (!open) return;
    (void)out.close();
    (void)io::remove_file(temp_path);
    open = false;
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

std::string offer_json(const std::vector<outgoing_file>& files) {
  std::uint64_t bytes = 0;
  for (const auto& f : files) bytes += f.size;
  json::writer w;
  w.begin_object().key("count").integer(static_cast<std::int64_t>(files.size()));
  w.key("bytes").integer(static_cast<std::int64_t>(bytes));
  w.key("files").begin_array();
  for (const auto& f : files) {
    w.begin_object().key("rel").string(f.rel).key("size").integer(static_cast<std::int64_t>(f.size)).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

transfer_result send_files(channel& ch, const std::vector<outgoing_file>& files, const hooks& h,
                           const std::function<void()>& on_accepted) {
  transfer_result res;
  auto stop = [&](outcome o, int line) {
    note(h, "send", line, o);
    res.how = o;
    if (o == outcome::cancelled) (void)ch.try_send(msg::cancel, "{}");
    ch.shutdown();
    return res;
  };
  if (files.empty() || files.size() > kMaxFiles) return stop(outcome::failed, __LINE__);
  if (!ch.send(msg::offer, offer_json(files))) return stop(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);

  // The answer, while the person there decides (it declines by itself after
  // kAskMs; this waits a little longer, then gives up).
  message m;
  for (;;) {
    if (auto r = ch.receive(m, kAskMs + 30000); !r) {
      return stop(cancelled(h) || r.error() == status::cancelled ? outcome::cancelled : outcome::failed, __LINE__);
    }
    if (m.type == msg::accept) break;
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
  std::atomic<bool> finished{false}, remote_cancel{false}, broken{false};
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
        remote_cancel.store(true);
        ch.shutdown();
        return;
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
      if (o == outcome::cancelled && !remote_cancel.load()) (void)ch.try_send(msg::cancel, "{}");
      ch.shutdown();
    }
    reader.join();
    res.done = verified.load();
    res.how = o;
    if (o == outcome::delivered && (!finished.load() || res.done + res.skipped != files.size())) {
      res.how = outcome::failed;
    }
    ch.shutdown();
    return res;
  };

  std::vector<std::uint8_t> chunk(kChunkBytes);
  std::uint64_t moved = 0;
  std::uint32_t sent_files = 0;
  for (std::uint32_t i = 0; i < files.size(); ++i) {
    const outgoing_file& f = files[i];
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
    });
    if (!ch.send(msg::file_begin, begin)) return finish(cancelled(h) ? outcome::cancelled : outcome::failed, __LINE__);
    io::hasher hs;
    std::uint64_t got = 0;
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
      if (auto sent = ch.send(msg::data, std::span<const std::uint8_t>(chunk.data(), *n)); !sent) {
        note_status(h, "send: data", __LINE__, sent.error());
        return finish(cancelled(h) || remote_cancel.load() ? outcome::cancelled : outcome::failed, __LINE__);
      }
      moved += *n;
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
  res.done = verified.load();
  res.how = finished.load() && res.done + res.skipped == files.size() ? outcome::delivered
            : remote_cancel.load()                                    ? outcome::cancelled
                                                                      : outcome::failed;
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
    // Each size is at most 2^50 and there are at most 10^5: the sum cannot
    // wrap, and it must equal what the offer says it totals.
    sum += static_cast<std::uint64_t>(*size);
    o.files.push_back({*rel, static_cast<std::uint64_t>(*size)});
  }
  if (sum != static_cast<std::uint64_t>(*bytes)) return err(status::corrupt);
  o.bytes = sum;
  return o;
}

result<incoming_offer> read_offer(channel& ch, int timeout_ms) {
  message m;
  if (auto r = ch.receive(m, timeout_ms); !r) return err(r.error());
  if (m.type != msg::offer) return err(status::corrupt);
  return parse_offer(m.text());
}

expected send_accept(channel& ch) { return ch.send(msg::accept, std::string_view("{}")); }

expected send_decline(channel& ch, outcome why) {
  const char* text = why == outcome::busy ? "busy" : why == outcome::no_room ? "no_room" : "declined";
  return ch.send(msg::decline, json_of([&](json::writer& w) { w.key("why").string(text); }));
}

transfer_result receive_files(channel& ch, const incoming_offer& offer, const std::string& dest_dir, const hooks& h) {
  transfer_result res;
  std::unique_ptr<incoming> cur;  // the file being written, if any
  std::uint32_t next = 0;
  std::uint32_t failed = 0;
  std::uint64_t moved = 0;
  auto end = [&](outcome o, bool tell, int line) {
    note(h, "receive", line, o);
    if (cur) cur->drop();
    if (tell) (void)ch.send(msg::cancel, std::string_view("{}"));
    ch.shutdown();
    res.how = o;
    return res;
  };

  message m;
  for (;;) {
    if (cancelled(h)) return end(outcome::cancelled, true, __LINE__);
    if (auto r = ch.receive(m); !r) {
      note_status(h, "receive: link", __LINE__, r.error());
      return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
    }
    switch (m.type) {
      case msg::file_begin: {
        const auto doc = json::parse(m.text(), 4);
        const auto i = doc ? doc->integer("i") : std::nullopt;
        const auto size = doc ? doc->integer("size") : std::nullopt;
        const auto mtime = doc ? doc->integer("mtime") : std::nullopt;
        if ((cur && cur->open) || !i || *i != static_cast<std::int64_t>(next) || next >= offer.files.size() || !size ||
            static_cast<std::uint64_t>(*size) != offer.files[next].size) {
          return end(outcome::failed, true, __LINE__);
        }
        const std::string& rel = offer.files[next].rel;
        const std::string target = io::join_path(dest_dir, io::native_relative(rel));
        const std::string dir(io::parent_of(target));
        if (!io::make_directories(dir)) return end(outcome::failed, true, __LINE__);
        cur = std::make_unique<incoming>();
        cur->index = next;
        cur->size = static_cast<std::uint64_t>(*size);
        cur->mtime = mtime.value_or(0);
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
        cur->open = true;
        break;
      }
      case msg::data: {
        if (!cur || !cur->open || cur->got + m.body.size() > cur->size) return end(outcome::failed, true, __LINE__);
        if (!cur->out.write(m.body)) return end(outcome::failed, true, __LINE__);
        cur->hash.update(m.body);
        cur->got += m.body.size();
        moved += m.body.size();
        if (h.progress) h.progress(moved, res.done);
        if (h.yield) h.yield();
        break;
      }
      case msg::file_end: {
        if (!cur || !cur->open || m.body.size() != 32 || cur->got != cur->size) return end(outcome::failed, true, __LINE__);
        io::content_hash sent;
        std::memcpy(sent.bytes.data(), m.body.data(), 32);
        bool ok = cur->hash.finish() == sent && cur->out.flush_durable().has_value();
        if (ok && cur->mtime != 0) (void)cur->out.set_mtime(cur->mtime);
        ok = ok && cur->out.close().has_value();
        // From the device, as verified_copy's read-back.
        if (ok) {
          const auto back = read_back(cur->temp_path, h);
          if (!back && cancelled(h)) return end(outcome::cancelled, true, __LINE__);
          ok = back && *back == sent;
        }
        if (ok) {
          // Never over anything: a name taken since we chose it moves on.
          ok = false;
          const std::string dir(io::parent_of(cur->final_path));
          const std::string leaf(io::file_name_of(cur->final_path));
          for (int a = 0; a < kRenameAttempts; ++a) {
            auto r = io::rename_no_replace(cur->temp_path, cur->final_path);
            if (!r) break;
            if (*r == io::rename_outcome::renamed) {
              ok = true;
              break;
            }
            cur->final_path = free_name(dir, leaf);
            if (cur->final_path.empty()) break;
          }
        }
        if (ok) {
          cur->open = false;
          ++res.done;
          res.written.push_back(cur->final_path);
        } else {
          cur->drop();
          ++failed;
        }
        const std::uint32_t i = cur->index;
        ++next;
        if (h.progress) h.progress(moved, res.done);
        if (!ch.send(msg::file_done, json_of([&](json::writer& w) { w.key("i").integer(i).key("ok").boolean(ok); }))) {
          return end(cancelled(h) ? outcome::cancelled : outcome::failed, false, __LINE__);
        }
        break;
      }
      case msg::file_skip: {
        const auto i = int_field(m.text(), "i");
        if (!i || *i != static_cast<std::int64_t>(next)) return end(outcome::failed, true, __LINE__);
        if (cur) cur->drop();
        ++next;
        ++res.skipped;
        break;
      }
      case msg::done: {
        if ((cur && cur->open) || next != offer.files.size()) return end(outcome::failed, true, __LINE__);
        (void)ch.send(msg::done, std::string_view("{}"));
        ch.shutdown();
        res.how = failed == 0 ? outcome::delivered : outcome::failed;
        return res;
      }
      case msg::cancel:
        return end(outcome::cancelled, false, __LINE__);
      default:
        return end(outcome::failed, true, __LINE__);
    }
  }
}

}  // namespace mv::sharepoint
