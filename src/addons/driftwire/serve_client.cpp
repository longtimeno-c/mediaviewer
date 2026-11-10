// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/serve_client.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include "addons/driftwire/transfer.h"
#include "core/json.h"

namespace mv::driftwire::serve {

namespace {

constexpr int kAnswerMs = 8000;     // the caps
constexpr int kRequestMs = 15000;   // a list, stat or roots

// How far a read may be consumed before more credit goes out: half a window.
constexpr std::uint32_t kCreditRefill = 2;

}  // namespace

struct client::state {
  std::unique_ptr<channel> ch;
  caps_msg caps;
  std::atomic<std::uint32_t> next_id{1};

  // Per-stream, kept while its id can still come back.
  struct stream_slot {
    std::mutex m;
    std::condition_variable cv;
    std::deque<read_stream::chunk> queue;
    std::string collected;  // list: what the data frames added up to
    bool collecting = false;
    std::optional<end_code> ended;
    std::string end_json;
    bool cancelled = false;
    std::uint32_t consumed = 0;  // since credit last went out
    std::uint32_t window = 0;
    std::uint64_t queued = 0;    // bytes in `queue`
  };

  std::mutex m;
  std::map<std::uint32_t, std::shared_ptr<stream_slot>> slots;  // under m
  std::thread reader;
  std::atomic<bool> closing{false};
  mutable std::mutex why_m;
  std::string ended_why;  // under why_m

  void reader_loop();
  std::shared_ptr<stream_slot> open_slot(open_request& r);
  void remove(std::uint32_t id);
};

void client::state::remove(std::uint32_t id) {
  std::lock_guard lock(m);
  slots.erase(id);
}

std::shared_ptr<client::state::stream_slot> client::state::open_slot(open_request& r) {
  auto slot = std::make_shared<stream_slot>();
  r.id = next_id.fetch_add(1);
  std::lock_guard lock(m);
  slots[r.id] = slot;
  return slot;
}

void client::state::reader_loop() {
  message m;
  for (;;) {
    if (auto r = ch->receive(m); !r) break;
    if (m.type == msg::cancel) {
      const auto doc = json::parse(m.text(), 4);
      const std::string* why = doc ? doc->str("why") : nullptr;
      std::lock_guard lock(why_m);
      ended_why = why ? *why : "cancelled";
      break;
    }
    if (m.type == msg::stream_data) {
      const auto d = parse_data(m.body);
      if (!d) break;
      std::shared_ptr<stream_slot> slot;
      {
        std::lock_guard lock(this->m);
        const auto it = slots.find(d->id);
        if (it != slots.end()) slot = it->second;
      }
      if (!slot) continue;  // a stream cancelled here: what was on the wire
      bool over = false;
      {
        std::lock_guard lock(slot->m);
        if (slot->collecting) {
          slot->collected.append(reinterpret_cast<const char*>(d->bytes.data()), d->bytes.size());
          over = slot->collected.size() > kMaxListBytes + kBulkChunk;
        } else {
          slot->queue.push_back({d->offset, {d->bytes.begin(), d->bytes.end()}});
          slot->queued += d->bytes.size();
          over = slot->queued > std::uint64_t{slot->window} + kBulkChunk;  // past the credit it was given
        }
        slot->cv.notify_all();
      }
      if (over) {
        ch->shutdown();
        break;
      }
    } else if (m.type == msg::stream_end) {
      const auto e = parse_end(m.body);
      if (!e) break;
      std::shared_ptr<stream_slot> slot;
      {
        std::lock_guard lock(this->m);
        const auto it = slots.find(e->id);
        if (it != slots.end()) slot = it->second;
      }
      if (!slot) continue;
      std::lock_guard lock(slot->m);
      slot->ended = e->code;
      slot->end_json = std::string(e->json);
      slot->cv.notify_all();
    }
  }
  // The session is over: every wait ends. Never a slot's lock under m: a
  // reader of a slot takes m while it is done with the slot.
  std::vector<std::shared_ptr<stream_slot>> waiting;
  {
    std::lock_guard lock(this->m);
    closing.store(true);
    for (auto& [id, slot] : slots) waiting.push_back(slot);
  }
  for (auto& slot : waiting) {
    std::lock_guard l(slot->m);
    slot->cv.notify_all();
  }
}

result<std::unique_ptr<client>> client::open(std::shared_ptr<link> l, const options& o, refusal* why) {
  if (why) *why = refusal::none;
  channel::options opts;
  opts.access_key = o.access_key;
  opts.static_key = o.device_key;
  auto ch = channel::open_library(l, o.id, o.name, opts);
  if (!ch) return err(ch.error());
  if (o.service_key && (*ch)->peer_key() != o.service_key) {
    if (why) *why = refusal::wrong_service;
    (*ch)->shutdown();
    return err(status::permission_denied);
  }
  const auto refused = [&why](const message& m) {
    const auto doc = json::parse(m.text(), 4);
    const std::string* text = doc ? doc->str("why") : nullptr;
    if (why && text) {
      *why = *text == "busy"       ? refusal::busy
             : *text == "bad-code" ? refusal::bad_code
             : *text == "too-many" ? refusal::too_many
                                   : refusal::not_paired;
    }
  };
  message m;
  if (!o.pair_code.empty()) {
    (void)(*ch)->send(msg::serve_pair, pair_json(o.pair_code, o.device_name));
    if (auto r = (*ch)->receive(m, kAnswerMs); !r) return err(r.error());
    if (m.type == msg::cancel) {
      refused(m);
      return err(status::permission_denied);
    }
    if (m.type != msg::serve_paired) return err(status::corrupt);
  }
  // Not checked here: a service that refused this device has said so and may
  // already be closing, and the refusal is what the receive below reads.
  (void)(*ch)->send(msg::serve_hello, hello_json(o.ops));
  if (auto r = (*ch)->receive(m, kAnswerMs); !r) return err(r.error());
  if (m.type == msg::cancel) {
    refused(m);
    return err(status::permission_denied);
  }
  if (m.type != msg::serve_caps) return err(status::corrupt);
  const auto caps = parse_caps(m.text());
  if (!caps) return err(status::corrupt);
  if ((caps->ops & o.ops) == 0) {
    if (why) *why = refusal::too_old;
    (*ch)->shutdown();
    return err(status::unsupported_format);
  }
  std::unique_ptr<client> c(new client());
  c->st_ = std::make_shared<state>();
  c->st_->ch = std::move(*ch);
  c->st_->caps = *caps;
  state* st = c->st_.get();
  st->reader = std::thread([st] { st->reader_loop(); });
  return c;
}

client::~client() {
  close();
  if (st_ && st_->reader.joinable()) st_->reader.join();
}

const caps_msg& client::caps() const noexcept { return st_->caps; }

void client::close() noexcept {
  if (!st_) return;
  st_->closing.store(true);
  if (st_->ch) (void)st_->ch->cancel(kCancelWaitMs);
}

bool client::closed() const noexcept { return st_->closing.load() || st_->ch->closed(); }

std::string client::ended_why() const {
  std::lock_guard lock(st_->why_m);
  return st_->ended_why;
}

// ---- reads -----------------------------------------------------------------------------------

struct client::read_stream::state {
  std::shared_ptr<client::state> c;
  std::shared_ptr<client::state::stream_slot> slot;
  std::uint32_t id = 0;
};

client::read_stream::~read_stream() {
  if (st_) cancel();
}

end_code client::read_stream::code() const {
  std::lock_guard lock(st_->slot->m);
  return st_->slot->ended.value_or(end_code::ok);
}

void client::read_stream::cancel() {
  if (!st_) return;
  bool over = false;
  {
    std::lock_guard lock(st_->slot->m);
    over = st_->slot->cancelled || st_->slot->ended.has_value();
    st_->slot->cancelled = true;
  }
  if (over) {
    st_->c->remove(st_->id);
    return;
  }
  (void)st_->c->ch->send(msg::stream_cancel, encode_cancel(st_->id));
  st_->c->remove(st_->id);
}

result<std::optional<client::read_stream::chunk>> client::read_stream::next(int timeout_ms) {
  auto& slot = *st_->slot;
  std::unique_lock lock(slot.m);
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (slot.queue.empty() && !slot.ended) {
    if (st_->c->closing.load() || st_->c->ch->closed()) return err(status::io);
    if (slot.cv.wait_until(lock, std::min(until, std::chrono::steady_clock::now() + std::chrono::milliseconds(200))) ==
            std::cv_status::timeout &&
        std::chrono::steady_clock::now() >= until && slot.queue.empty() && !slot.ended) {
      return err(status::timeout);
    }
  }
  if (slot.queue.empty()) {
    lock.unlock();  // remove() takes the client's lock, which is taken before a slot's
    st_->c->remove(st_->id);
    return std::optional<chunk>{};
  }
  chunk out = std::move(slot.queue.front());
  slot.queue.pop_front();
  slot.queued -= out.bytes.size();
  slot.consumed += static_cast<std::uint32_t>(out.bytes.size());
  std::uint32_t refill = 0;
  if (slot.consumed >= slot.window / kCreditRefill) {
    refill = slot.consumed;
    slot.consumed = 0;
  }
  const bool done = slot.ended.has_value();
  lock.unlock();
  if (refill != 0 && !done) (void)st_->c->ch->send(msg::stream_credit, encode_credit(st_->id, refill));
  return std::optional<chunk>(std::move(out));
}

result<std::unique_ptr<client::read_stream>> client::read_range(const std::string& root, const std::string& rel,
                                                                std::uint64_t at, std::uint64_t length,
                                                                std::uint32_t window, bool bulk) {
  open_request r;
  r.kind = op::read_range;
  r.root = root;
  r.rel = rel;
  r.at = at;
  r.length = length;
  r.credit = std::min(window, kMaxStreamCredit);
  r.bulk = bulk;
  auto slot = st_->open_slot(r);
  slot->window = r.credit;
  if (auto s = st_->ch->send(msg::stream_open, encode_open(r)); !s) {
    st_->remove(r.id);
    return err(s.error());
  }
  std::unique_ptr<read_stream> rs(new read_stream());
  rs->st_ = std::make_shared<read_stream::state>();
  rs->st_->c = st_;
  rs->st_->slot = std::move(slot);
  rs->st_->id = r.id;
  return rs;
}

// ---- the one-shot operations ----------------------------------------------------------------

namespace {

// Waits for `slot`'s end, a window's worth of bytes at a time if it streams.
template <typename StatePtr, typename SlotPtr>
result<end_code> finish(const StatePtr& st, const SlotPtr& slot, std::uint32_t id, end_code* out) {
  // A deadline for silence, not for a listing that is still arriving.
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestMs);
  std::unique_lock lock(slot->m);
  while (!slot->ended) {
    if (st->closing.load() || st->ch->closed()) return err(status::io);
    // Everything a list has streamed so far is consumed: more credit.
    if (slot->collecting && slot->collected.size() > slot->consumed) {
      const auto n = static_cast<std::uint32_t>(slot->collected.size() - slot->consumed);
      slot->consumed = static_cast<std::uint32_t>(slot->collected.size());
      until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestMs);
      lock.unlock();
      (void)st->ch->send(msg::stream_credit, encode_credit(id, n));
      lock.lock();
      continue;
    }
    if (slot->cv.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout &&
        std::chrono::steady_clock::now() >= until) {
      lock.unlock();
      (void)st->ch->send(msg::stream_cancel, encode_cancel(id));  // the service stops holding it
      return err(status::timeout);
    }
  }
  if (out) *out = *slot->ended;
  return *slot->ended;
}

}  // namespace

result<std::vector<root_info>> client::roots(end_code* code) {
  open_request r;
  r.kind = op::roots;
  auto slot = st_->open_slot(r);
  if (auto s = st_->ch->send(msg::stream_open, encode_open(r)); !s) {
    st_->remove(r.id);
    return err(s.error());
  }
  auto done = finish(st_, slot, r.id, code);
  st_->remove(r.id);
  if (!done) return err(done.error());
  if (*done != end_code::ok) return err(status::permission_denied);
  auto parsed = parse_roots(slot->end_json);
  if (!parsed) return err(status::corrupt);
  return *parsed;
}

result<stat_info> client::stat(const std::string& root, const std::string& rel, end_code* code) {
  open_request r;
  r.kind = op::stat;
  r.root = root;
  r.rel = rel;
  auto slot = st_->open_slot(r);
  if (auto s = st_->ch->send(msg::stream_open, encode_open(r)); !s) {
    st_->remove(r.id);
    return err(s.error());
  }
  auto done = finish(st_, slot, r.id, code);
  st_->remove(r.id);
  if (!done) return err(done.error());
  if (*done == end_code::not_found) return err(status::not_found);
  if (*done != end_code::ok) return err(status::permission_denied);
  auto parsed = parse_stat(slot->end_json);
  if (!parsed) return err(status::corrupt);
  return *parsed;
}

result<std::vector<list_entry>> client::list(const std::string& root, const std::string& rel, end_code* code) {
  open_request r;
  r.kind = op::list;
  r.root = root;
  r.rel = rel;
  r.credit = 1024 * 1024;
  auto slot = st_->open_slot(r);
  {
    std::lock_guard lock(slot->m);
    slot->collecting = true;
    slot->window = r.credit;
  }
  if (auto s = st_->ch->send(msg::stream_open, encode_open(r)); !s) {
    st_->remove(r.id);
    return err(s.error());
  }
  auto done = finish(st_, slot, r.id, code);
  st_->remove(r.id);
  if (!done) return err(done.error());
  if (*done == end_code::not_found) return err(status::not_found);
  if (*done != end_code::ok) return err(status::permission_denied);
  auto parsed = parse_listing(slot->collected);
  if (!parsed) return err(status::corrupt);
  return *parsed;
}

result<std::string> client::admin(const std::string& request_json, end_code* code) {
  open_request r;
  auto slot = st_->open_slot(r);
  if (auto s = st_->ch->send(msg::serve_admin, encode_admin(r.id, request_json)); !s) {
    st_->remove(r.id);
    return err(s.error());
  }
  auto done = finish(st_, slot, r.id, code);
  st_->remove(r.id);
  if (!done) return err(done.error());
  switch (*done) {
    case end_code::ok:
      return slot->end_json;
    case end_code::not_found:
      return err(status::not_found);
    case end_code::bad_request:
      return err(status::invalid_arg);
    case end_code::too_many:
      return err(status::busy);
    case end_code::error:
      return err(status::io);
    default:
      return err(status::permission_denied);
  }
}

result<claim_answer> claim(std::shared_ptr<link> l, const std::array<std::uint8_t, 32>& key,
                           const std::array<std::uint8_t, 32>& device_key, const std::string& name,
                           const std::function<bool(std::string_view nonce)>& prove,
                           std::chrono::steady_clock::time_point deadline) {
  const auto left_ms = [&] {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::max<std::int64_t>(1, ms));  // 1: the channel reads 0 as no limit at all
  };
  channel::options opts;
  opts.access_key = key;
  opts.static_key = device_key;
  auto ch = channel::open_library(l, "0000000000000000", name, opts);
  if (!ch) return err(status::permission_denied);  // silence: owned, not its network, or no token here
  json::writer w;
  w.begin_object().key("name").string(name).end_object();
  (void)(*ch)->send(msg::serve_claim, w.str());
  message m;
  if (auto r = (*ch)->receive(m, std::min(kAnswerMs, left_ms())); !r) return err(status::permission_denied);
  if (m.type != msg::serve_claim_check) return err(status::permission_denied);
  const auto check = json::parse(m.text(), 4);
  const std::string* nonce = check ? check->str("nonce") : nullptr;
  if (!nonce || nonce->size() != 64 || !prove || !prove(*nonce)) {
    (*ch)->shutdown();
    return err(status::permission_denied);
  }
  (void)(*ch)->send(msg::serve_claim, w.str());
  if (auto r = (*ch)->receive(m, left_ms()); !r) {
    // The proof is on the share: the service may have taken it, whatever
    // happened to its answer. The caller claims once more to find out.
    return err(status::timeout);
  }
  if (m.type != msg::serve_claimed) return err(status::permission_denied);
  const auto doc = json::parse(m.text(), 6);
  const std::string* access = doc ? doc->str("access_key") : nullptr;
  const auto parsed = access ? parse_key_hex(*access) : std::nullopt;
  const auto peer = (*ch)->peer_key();
  if (!parsed || !peer) return err(status::corrupt);
  claim_answer out;
  out.access_key = *parsed;
  out.identity = *peer;
  return out;
}

}  // namespace mv::driftwire::serve
