// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/serve.h"

#include <sodium.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "addons/driftwire/admission.h"
#include "addons/driftwire/channel.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/transfer.h"
#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire::serve {

namespace {

using clock = std::chrono::steady_clock;

// A hello that is not all there this soon is not a phone's (it is about 1.2 KB).
constexpr int kHelloMs = 3000;
constexpr int kServeHelloMs = 5000;
// First messages remembered as seen, against a replay of a recorded one. Only
// ones that opened under the key go in, so a stranger cannot flush it.
constexpr std::size_t kSeenFirsts = 4096;
constexpr int kAcceptPollMs = 500;
// New connections per second across the whole service: the most hello work
// it will take on, whoever is asking.
constexpr double kGlobalPerSecond = 200.0;
constexpr double kGlobalBurst = 400.0;
constexpr int kBacklog = 128;
// A connection's hello gets at least this long before it can be evicted: a
// phone's hello has to be able to arrive.
constexpr auto kMinHelloAge = std::chrono::milliseconds(500);
// When full and no silent connection can go, a handshake that has had this
// long since its reply: past a normal round trip, inside kLibraryServerMs.
constexpr auto kMinReplyAge = std::chrono::milliseconds(1000);
constexpr std::size_t kLimitSources = 4096;  // remembered per limit, so one block cannot flush the table
constexpr int kReserveSockets = 128;
// Interactive chunks in a row before one bulk chunk, when both are waiting.
constexpr int kBulkEvery = 4;

// One per address, or per /64: a host with IPv6 owns a whole /64.
std::string source_of(const net::address& a) {
  std::string key(a.v6 ? 8 : 4, '\0');
  std::memcpy(key.data(), a.bytes.data(), key.size());
  return key;
}

// The coarser bucket: an IPv6 customer often holds a whole /48. Empty for
// IPv4, where the address is already the bucket (source_limit lets empty pass).
std::string block_of(const net::address& a) {
  if (!a.v6) return {};
  return std::string(reinterpret_cast<const char*>(a.bytes.data()), 6);
}

struct stream {
  std::uint32_t id = 0;
  op kind = op::read_range;
  bool bulk = false;
  bool busy = false;       // the pump is reading or sending it
  bool cancelled = false;
  std::uint32_t credit = 0;
  io::file_reader reader;
  std::uint64_t at = 0, remaining = 0, sent = 0;
  std::string body;  // list: its bytes, and how far they have gone
  std::size_t body_at = 0;
  std::uint64_t count = 0;
  bool truncated = false;
};

// The live connection: pre-auth until `ch` is set.
struct conn {
  std::shared_ptr<link> l;
  std::string source, block;
  std::chrono::steady_clock::time_point born = std::chrono::steady_clock::now();
  bool pending_released = false;  // the connection's thread's own
  // 0: its hello has not all arrived; 1: it has, the handshake is running;
  // 2: closed to make room; 3: past the handshake, or failed: never evicted.
  std::atomic<int> stage{0};
  std::chrono::steady_clock::time_point hello_at;  // written before stage becomes 1
  std::mutex m;
  std::unique_ptr<channel> ch;  // under m (set once)
  std::array<std::uint8_t, 32> device{};
  bool has_device = false;  // under m
  std::atomic<bool> done{false};
  std::thread t;
};

}  // namespace

struct server::state {
  config cfg;
  secrets::store* store = nullptr;
  bool loopback_only = false;
  int max_sessions = 8;
  int max_pending = 16;
  int per_minute = 30;

  std::array<std::uint8_t, 32> identity{}, access_key{};
  std::unique_ptr<net::listener> listen;
  std::uint16_t port = 0;
  std::atomic<bool> stopping{false};
  source_limit limits{30, std::chrono::minutes(1), kLimitSources};
  source_limit block_limits{120, std::chrono::minutes(1), kLimitSources};
  double tokens = kGlobalBurst;  // the accept thread's
  clock::time_point tokens_at = clock::now();
  int per_source_pending = 2, per_block_pending = 4;

  std::atomic<int> pending{0}, sessions{0};
  std::mutex conns_m;
  std::list<std::shared_ptr<conn>> conns;  // under conns_m
  std::mutex source_m;
  std::map<std::string, int> in_flight;  // under source_m: handshakes per address

  std::mutex seen_m;
  std::vector<std::array<std::uint8_t, 32>> seen;  // under seen_m: a ring
  std::size_t seen_next = 0;                       // under seen_m

  std::mutex watch_m;
  std::condition_variable watch_cv;
  std::thread acceptor, watcher;

  std::atomic<std::uint64_t> n_evicted{0};
  std::atomic<std::uint64_t> n_accepted{0}, n_over{0}, n_hello{0}, n_key{0}, n_replay{0}, n_unpaired{0}, n_sessions{0};

  ~state() {
    sodium_memzero(identity.data(), identity.size());
    sodium_memzero(access_key.data(), access_key.size());
  }

  bool fresh(std::span<const std::uint8_t> first) {
    std::array<std::uint8_t, 32> e;
    if (first.size() < e.size()) return false;
    std::memcpy(e.data(), first.data(), e.size());
    std::lock_guard lock(seen_m);
    if (std::find(seen.begin(), seen.end(), e) != seen.end()) {
      n_replay.fetch_add(1);
      return false;
    }
    if (seen.size() < kSeenFirsts) {
      seen.push_back(e);
    } else {
      seen[seen_next] = e;
      seen_next = (seen_next + 1) % kSeenFirsts;
    }
    return true;
  }

  [[nodiscard]] bool paired(const std::array<std::uint8_t, 32>& key) {
    const auto devices = known::load(*store);
    return std::any_of(devices.begin(), devices.end(), [&](const known::device& d) { return d.key == key; });
  }

  void accept_loop();
  bool take_token(clock::time_point now) noexcept;
  bool evict_oldest_hello(clock::time_point now);
  void reap();
  void run(conn* c);
  void serve_connection(conn* c);
  void release_pending(conn& c);
  void session(conn& c);
  void watch_loop();
  void cut_unpaired();
};

// ---- a session ------------------------------------------------------------------------------

namespace {

class session_state {
 public:
  using send_fn = std::function<bool(msg, std::span<const std::uint8_t>)>;
  session_state(send_fn send, const config& cfg, std::uint32_t ops) : send_(std::move(send)), cfg_(cfg), ops_(ops) {}

  // The reader thread's: a message from the client. False: end the session.
  bool on_message(const message& m) {
    switch (m.type) {
      case msg::stream_open: {
        bool unknown = false;
        auto r = parse_open(m.body, unknown);
        if (!r) return false;  // a client that cannot make a request is not one
        if (unknown) return send_end(r->id, end_code::unsupported);
        const std::uint32_t id = r->id;
        bool full = false;
        {
          std::lock_guard lock(m_);
          full = opens_.size() + streams_.size() >= kMaxStreams;
          if (!full) {
            opens_.push_back(std::move(*r));
            cv_.notify_all();
          }
        }
        // No lock held across a send: the reader never waits on a write.
        return full ? send_end(id, end_code::too_many) : true;
      }
      case msg::stream_credit: {
        const auto c = parse_credit(m.body);
        if (!c) return false;
        std::lock_guard lock(m_);
        for (auto& o : opens_) {
          if (o.id == c->id) {
            o.credit = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{o.credit} + c->bytes, kMaxStreamCredit));
            return true;
          }
        }
        if (c->id == opening_) {
          opening_credit_ = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(std::uint64_t{opening_credit_} + c->bytes, kMaxStreamCredit));
          return true;
        }
        for (auto& s : streams_) {
          if (s->id == c->id) {
            const std::uint64_t total = std::uint64_t{s->credit} + c->bytes;
            s->credit = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, kMaxStreamCredit));
            cv_.notify_all();
            break;
          }
        }
        return true;  // credit for a stream that has ended is not an error
      }
      case msg::stream_cancel: {
        const auto id = parse_cancel(m.body);
        if (!id) return false;
        std::lock_guard lock(m_);
        std::erase_if(opens_, [&](const open_request& o) { return o.id == *id; });
        if (*id == opening_) opening_cancelled_ = true;
        for (auto it = streams_.begin(); it != streams_.end(); ++it) {
          if ((*it)->id == *id) {
            if ((*it)->busy) {
              (*it)->cancelled = true;
            } else {
              streams_.erase(it);
            }
            break;
          }
        }
        return true;
      }
      default:
        return false;  // anything else after the hello is not this protocol
    }
  }

  // The harness: until nothing is left that can run (bounded).
  void settle() {
    for (int i = 0; i < 50; ++i) {
      {
        std::lock_guard lock(m_);
        if (opens_.empty() && opening_ == 0 && !runnable() &&
            std::none_of(streams_.begin(), streams_.end(), [](const auto& t) { return t->busy; })) {
          return;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void stop() {
    std::lock_guard lock(m_);
    stopping_ = true;
    cv_.notify_all();
  }

  // The pump thread: opens requests, and moves bytes, one chunk at a time.
  void pump() {
    std::vector<std::uint8_t> buf(kDataHeaderBytes + kBulkChunk);
    for (;;) {
      std::optional<open_request> request;
      stream* picked = nullptr;
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return stopping_ || !opens_.empty() || runnable(); });
        if (stopping_) return;
        if (!opens_.empty()) {
          request = std::move(opens_.front());
          opens_.pop_front();
          // From here until open() is done the stream is in neither list.
          opening_ = request->id;
          opening_credit_ = 0;
          opening_cancelled_ = false;
        } else {
          picked = pick();
          if (!picked) continue;
          picked->busy = true;
        }
      }
      if (request) {
        if (!open(*request)) return;
        continue;
      }
      if (!step(*picked, buf)) {
        std::lock_guard lock(m_);
        stopping_ = true;
        return;
      }
      std::lock_guard lock(m_);
      picked->busy = false;
      if (picked->cancelled || finished(*picked)) {
        std::erase_if(streams_, [&](const std::unique_ptr<stream>& s) { return s.get() == picked; });
      }
    }
  }

 private:
  [[nodiscard]] bool can_run(const stream& s) const noexcept {
    if (s.busy || s.cancelled || finished(s)) return false;
    return s.credit > 0;
  }
  [[nodiscard]] bool runnable() const noexcept {
    return std::any_of(streams_.begin(), streams_.end(), [&](const auto& s) { return can_run(*s); });
  }
  [[nodiscard]] static bool finished(const stream& s) noexcept {
    return s.kind == op::read_range ? s.remaining == 0 : s.body_at >= s.body.size();
  }
  stream* pick() {
    const std::size_t n = streams_.size();
    stream* interactive = nullptr;
    stream* bulk = nullptr;
    std::size_t interactive_at = 0, bulk_at = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t idx = (cursor_ + i) % n;
      stream& s = *streams_[idx];
      if (!can_run(s)) continue;
      if (s.bulk && !bulk) {
        bulk = &s;
        bulk_at = idx;
      } else if (!s.bulk && !interactive) {
        interactive = &s;
        interactive_at = idx;
      }
    }
    if (interactive && (!bulk || since_bulk_ < kBulkEvery)) {
      cursor_ = interactive_at + 1;
      ++since_bulk_;
      return interactive;
    }
    if (bulk) {
      cursor_ = bulk_at + 1;
      since_bulk_ = 0;
      return bulk;
    }
    return nullptr;
  }

  static end_code end_for(status e) noexcept {
    return e == status::permission_denied ? end_code::refused : end_code::not_found;
  }

  bool send_end(std::uint32_t id, end_code code, std::string_view json = {}) {
    return send_(msg::stream_end, encode_end(id, code, json));
  }

  const root_config* find_root(const std::string& id) const noexcept {
    for (const auto& r : cfg_.roots) {
      if (r.id == id) return &r;
    }
    return nullptr;
  }

  // One request, from the pump. False ends the session.
  bool open(const open_request& r) {
    // Every way out of here leaves nothing marked as opening.
    struct finished_opening {
      session_state& s;
      ~finished_opening() {
        std::lock_guard lock(s.m_);
        s.opening_ = 0;
      }
    } opening_done{*this};
    if ((ops_ & op_bit(r.kind)) == 0) return send_end(r.id, end_code::unsupported);
    if (r.kind == op::roots) {
      std::vector<root_info> out;
      for (const auto& c : cfg_.roots) out.push_back({c.id, c.label});
      return send_end(r.id, end_code::ok, roots_json(out));
    }
    const root_config* root = find_root(r.root);
    // Before any file is looked at.
    if (!root || (!r.rel.empty() && !safe_relative(r.rel))) return send_end(r.id, end_code::refused);
    if (r.kind == op::read_range && r.rel.empty()) return send_end(r.id, end_code::refused);
    if (r.kind == op::stat) {
      auto st = io::stat_beneath(root->path, r.rel);
      if (!st) return send_end(r.id, end_for(st.error()));
      return send_end(r.id, end_code::ok, stat_json({st->size, st->mtime_unix, st->is_directory}));
    }
    auto s = std::make_unique<stream>();
    s->id = r.id;
    s->kind = r.kind;
    s->bulk = r.bulk;
    s->credit = r.credit;
    if (r.kind == op::list) {
      // Listings are held whole until they have gone: at most about one
      // kMaxListBytes per session, however many streams ask.
      std::size_t held = 0;
      {
        std::lock_guard lock(m_);
        for (const auto& t : streams_) held += t->body.size();
      }
      if (held >= kMaxListBytes) return send_end(r.id, end_code::too_many);  // no lock across a send
      auto children = io::list_beneath(root->path, r.rel, true);
      if (!children) return send_end(r.id, end_for(children.error()));
      for (const auto& c : *children) {
        if (!safe_relative(c.name)) continue;  // a hidden name, or one no peer may ask for
        if (s->body.size() >= kMaxListBytes) {
          s->truncated = true;
          break;
        }
        s->body += list_line({c.name, c.size, c.mtime_unix, c.is_directory});
        ++s->count;
      }
    } else {
      if (auto opened = s->reader.open_beneath(root->path, r.rel); !opened) return send_end(r.id, end_for(opened.error()));
      const std::uint64_t size = s->reader.size();
      s->at = std::min(r.at, size);
      s->remaining = std::min(r.length, size - s->at);
      if (s->remaining == 0) return send_end(r.id, end_code::ok, done_json(0));
    }
    if (r.kind == op::list && s->body.empty()) {
      // Nothing to stream: its own end, with the count.
      json::writer w2;
      w2.begin_object().key("count").integer(0).key("truncated").boolean(false).end_object();
      return send_end(r.id, end_code::ok, w2.str());
    }
    bool duplicate = false;
    {
      std::lock_guard lock(m_);
      opening_ = 0;
      if (opening_cancelled_) return true;  // cancelled while it was being opened
      s->credit = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(std::uint64_t{s->credit} + opening_credit_, kMaxStreamCredit));
      duplicate = std::any_of(streams_.begin(), streams_.end(), [&](const auto& t) { return t->id == r.id; });
      if (!duplicate) {
        streams_.push_back(std::move(s));
        cv_.notify_all();
      }
    }
    return duplicate ? send_end(r.id, end_code::bad_request) : true;
  }

  static std::string done_json(std::uint64_t sent) {
    json::writer w;
    w.begin_object().key("sent").integer(static_cast<std::int64_t>(sent)).end_object();
    return w.take();
  }

  // One chunk of `s`. False ends the session.
  bool step(stream& s, std::vector<std::uint8_t>& buf) {
    if (s.kind == op::read_range) {
      const std::size_t chunk = s.bulk ? kBulkChunk : kInteractiveChunk;
      std::size_t want = 0;
      {
        std::lock_guard lock(m_);
        want = static_cast<std::size_t>(std::min<std::uint64_t>({chunk, s.credit, s.remaining}));
      }
      auto got = s.reader.read_at(s.at, std::span<std::uint8_t>(buf).subspan(kDataHeaderBytes, want));
      if (!got) {
        { std::lock_guard lock(m_); s.remaining = 0; }
        return send_end(s.id, end_code::error);
      }
      if (*got == 0) {
        // The file got shorter under it: what was sent is what there was.
        { std::lock_guard lock(m_); s.remaining = 0; }
        return send_end(s.id, end_code::ok, done_json(s.sent));
      }
      put_data_header(std::span<std::uint8_t, kDataHeaderBytes>(buf.data(), kDataHeaderBytes), s.id, s.at);
      if (!send_(msg::stream_data, std::span<const std::uint8_t>(buf).first(kDataHeaderBytes + *got))) return false;
      std::uint64_t left = 0;
      {
        std::lock_guard lock(m_);
        s.credit -= static_cast<std::uint32_t>(*got);
        s.at += *got;
        s.sent += *got;
        s.remaining -= *got;
        left = s.remaining;
      }
      if (left == 0) return send_end(s.id, end_code::ok, done_json(s.sent));
      return true;
    }
    // list: its bytes, as NDJSON, then its end.
    std::size_t want = 0;
    {
      std::lock_guard lock(m_);
      want = std::min<std::size_t>({kInteractiveChunk, s.credit, s.body.size() - s.body_at});
    }
    put_data_header(std::span<std::uint8_t, kDataHeaderBytes>(buf.data(), kDataHeaderBytes), s.id, s.body_at);
    std::memcpy(buf.data() + kDataHeaderBytes, s.body.data() + s.body_at, want);
    if (!send_(msg::stream_data, std::span<const std::uint8_t>(buf).first(kDataHeaderBytes + want))) return false;
    std::size_t done = 0;
    {
      std::lock_guard lock(m_);
      s.credit -= static_cast<std::uint32_t>(want);
      s.body_at += want;
      done = s.body.size() - s.body_at;
    }
    if (done == 0) {
      json::writer w;
      w.begin_object()
          .key("count")
          .integer(static_cast<std::int64_t>(s.count))
          .key("truncated")
          .boolean(s.truncated)
          .end_object();
      return send_end(s.id, end_code::ok, w.str());
    }
    return true;
  }

  send_fn send_;
  const config& cfg_;
  const std::uint32_t ops_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stopping_ = false;                         // under m_
  std::deque<open_request> opens_;                // under m_
  std::vector<std::unique_ptr<stream>> streams_;  // under m_; stream credit, busy, cancelled, at, remaining too
  std::size_t cursor_ = 0;                        // under m_
  std::uint32_t opening_ = 0;                     // under m_: the id open() is working on (ids are never 0)
  std::uint32_t opening_credit_ = 0;              // under m_
  bool opening_cancelled_ = false;                // under m_
  int since_bulk_ = 0;                            // under m_
};

}  // namespace

void drive_requests(const config& cfg, std::uint32_t ops, std::span<const std::uint8_t> input) {
  session_state s([](msg, std::span<const std::uint8_t>) { return true; }, cfg, ops);
  std::thread pump([&s] { s.pump(); });
  message m;
  while (input.size() >= 3) {
    m.type = static_cast<msg>(input[0]);
    const std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(input[1]) | static_cast<std::size_t>(input[2]) << 8,
                                                  input.size() - 3);
    m.body.assign(input.begin() + 3, input.begin() + 3 + static_cast<std::ptrdiff_t>(len));
    input = input.subspan(3 + len);
    if (!s.on_message(m)) break;
  }
  s.settle();
  s.stop();
  pump.join();
}

void server::state::session(conn& c) {
  channel& ch = *c.ch;
  message m;
  // The first message after ident: what the client can do.
  if (auto r = ch.receive(m, kServeHelloMs); !r || m.type != msg::serve_hello) {
    (void)ch.cancel(kCancelWaitMs, "protocol");
    return;
  }
  const auto hello = parse_hello(m.text());
  std::uint32_t ops = 0;
  std::uint32_t version = kVersion;
  if (hello) {
    version = std::clamp(hello->version, kMinVersion, kVersion);
    if (hello->min_version <= kVersion && hello->version >= kMinVersion) ops = hello->ops & kAllOps;
  }
  if (!ch.send(msg::serve_caps, caps_json(version, ops))) return;
  if (ops == 0) {
    // Nothing in common (one side too old). The answer said so.
    ch.flush(kCancelWaitMs);
    (void)ch.cancel(kCancelWaitMs, "versions");
    return;
  }
  session_state s([&ch](msg type, std::span<const std::uint8_t> body) { return ch.send(type, body).has_value(); }, cfg,
                  ops);
  // Out of memory (a huge folder listed on a small NAS) ends this session, never the service.
  std::thread pump([&s, &ch] {
    try {
      s.pump();
    } catch (...) {
      ch.shutdown();
    }
  });
  try {
    for (;;) {
      if (auto r = ch.receive(m); !r) break;
      if (m.type == msg::cancel) break;
      if (!s.on_message(m)) {
        (void)ch.cancel(kCancelWaitMs, "protocol");
        break;
      }
    }
  } catch (...) {
  }
  s.stop();
  ch.shutdown();
  pump.join();
}

void server::state::release_pending(conn& c) {
  if (c.pending_released) return;
  c.pending_released = true;
  c.stage.store(3);
  pending.fetch_sub(1);
  std::lock_guard lock(source_m);
  for (const std::string* key : {&c.source, &c.block}) {
    if (key->empty()) continue;
    const auto it = in_flight.find(*key);
    if (it != in_flight.end() && --it->second <= 0) in_flight.erase(it);
  }
}

void server::state::run(conn* c) {
  try {
    serve_connection(c);
  } catch (...) {
    c->l->shutdown();
  }
  release_pending(*c);
  c->done.store(true);
}

void server::state::serve_connection(conn* c) {
  std::unique_ptr<channel> opened;
  {
    auto h = channel::read_library_hello(*c->l, clock::now() + std::chrono::milliseconds(kHelloMs));
    if (!h) {
      c->l->shutdown();
      n_hello.fetch_add(1);
      return;
    }
    c->hello_at = clock::now();
    int waiting = 0;
    (void)c->stage.compare_exchange_strong(waiting, 1);  // its hello is all here: the last to be evicted
    channel::options o;
    o.access_key = access_key;
    o.static_key = identity;
    o.fresh_first = [this](std::span<const std::uint8_t> first) { return fresh(first); };
    auto ch = channel::open_library_server(c->l, std::move(*h), "0000000000000000", cfg.name, o);
    release_pending(*c);
    if (!ch) {
      if (ch.error() == status::permission_denied) n_key.fetch_add(1);
      c->l->shutdown();
      return;
    }
    opened = std::move(*ch);
  }
  const auto peer = opened->peer_key();
  if (!peer || !paired(*peer)) {
    n_unpaired.fetch_add(1);
    (void)opened->cancel(kCancelWaitMs, "not-paired");
    return;
  }
  const int before = sessions.fetch_add(1);
  // Held to the end of this function, however it ends.
  struct slot {
    std::atomic<int>& n;
    ~slot() { n.fetch_sub(1); }
  } held{sessions};
  if (before >= max_sessions) {
    (void)opened->cancel(kCancelWaitMs, "busy");
    return;
  }
  {
    std::lock_guard lock(c->m);
    c->ch = std::move(opened);
    c->device = *peer;
    c->has_device = true;
  }
  n_sessions.fetch_add(1);
  session(*c);
  {
    std::lock_guard lock(c->m);
    c->has_device = false;
  }
}

void server::state::reap() {
  std::vector<std::shared_ptr<conn>> gone;
  {
    std::lock_guard lock(conns_m);
    for (auto it = conns.begin(); it != conns.end();) {
      if ((*it)->done.load()) {
        gone.push_back(std::move(*it));
        it = conns.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& c : gone) {
    if (c->t.joinable()) c->t.join();
  }
}

bool server::state::take_token(clock::time_point now) noexcept {
  const double elapsed = std::chrono::duration<double>(now - tokens_at).count();
  tokens_at = now;
  tokens = std::min(kGlobalBurst, tokens + elapsed * kGlobalPerSecond);
  if (tokens < 1.0) return false;
  tokens -= 1.0;
  return true;
}

bool server::state::evict_oldest_hello(clock::time_point now) {
  std::lock_guard lock(conns_m);
  const auto evict = [&](conn& c, int from) {
    int expected = from;
    if (!c.stage.compare_exchange_strong(expected, 2)) return false;
    c.l->shutdown();  // its thread leaves the read and gives the slot back
    n_evicted.fetch_add(1);
    return true;
  };
  for (auto& c : conns) {  // in the order they arrived
    if (now - c->born < kMinHelloAge) break;  // and everything after it is younger
    if (evict(*c, 0)) return true;
  }
  // Nothing silent to take: a handshake that holds the key but has stalled
  // (a revoked phone, a recording). Its reply went out at least this long ago.
  for (auto& c : conns) {
    if (c->stage.load() == 1 && now - c->hello_at >= kMinReplyAge && evict(*c, 1)) return true;
  }
  return false;
}

void server::state::accept_loop() {
  while (!stopping.load()) {
    net::address from;
    auto accepted = listen->accept_from(kAcceptPollMs, from);
    reap();
    if (!accepted) {
      if (accepted.error() == status::cancelled) return;
      // Out of descriptors, say: not a loop at full speed.
      if (accepted.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    n_accepted.fetch_add(1);
    const std::string source = source_of(from);
    const std::string block = block_of(from);
    const auto now = clock::now();
    bool room = true;
    {
      // The coarser bucket first: a block over its limit never reaches the
      // per-address table, so it cannot push other addresses out of it.
      std::lock_guard lock(source_m);
      if (!block.empty()) {
        const auto at_block = in_flight.find(block);
        room = at_block == in_flight.end() || at_block->second < per_block_pending;
      }
      if (room) {
        const auto at_source = in_flight.find(source);
        room = at_source == in_flight.end() || at_source->second < per_source_pending;
      }
    }
    // The block is charged only once the address has passed, so one address
    // over its limit does not spend its neighbours' budget.
    room = room && block_limits.has_room(block, now) && limits.allow(source, now) && block_limits.allow(block, now) &&
           pending.load() < 2 * max_pending;
    // The global budget is last, and only for a connection that gets in, so
    // what the per-address limits drop never spends it. Full: the oldest
    // connection still saying nothing makes way (from kMinHelloAge), else
    // the oldest handshake waiting past kMinReplyAge.
    if (room && !take_token(now)) room = false;
    if (room && pending.load() >= max_pending && !evict_oldest_hello(now)) {
      tokens = std::min(kGlobalBurst, tokens + 1.0);  // it did not get in
      room = false;
    }
    if (!room) {
      n_over.fetch_add(1);
      continue;  // destroying the link closes it; nothing is said
    }
    auto c = std::make_shared<conn>();
    c->l = std::shared_ptr<link>(std::move(*accepted));
    c->source = source;
    c->block = block;
    pending.fetch_add(1);
    {
      std::lock_guard lock(source_m);
      ++in_flight[source];
      if (!block.empty()) ++in_flight[block];
    }
    {
      std::lock_guard lock(conns_m);
      conns.push_back(c);
    }
    conn* raw = c.get();
    try {
      raw->t = std::thread([this, raw] { run(raw); });
    } catch (...) {
      // No thread to be had: this connection goes, the service stays.
      c->l->shutdown();
      release_pending(*raw);
      c->done.store(true);
    }
  }
}

void server::state::cut_unpaired() {
  const auto devices = known::load(*store);
  // The cuts wait on sockets, so they happen with no lock held; each conn is
  // kept alive by its reference here, and its channel is set only once.
  std::vector<std::shared_ptr<conn>> revoked;
  {
    std::lock_guard all(conns_m);
    for (auto& c : conns) {
      std::lock_guard lock(c->m);
      if (!c->has_device || !c->ch) continue;
      if (std::none_of(devices.begin(), devices.end(), [&](const known::device& d) { return d.key == c->device; })) {
        c->has_device = false;
        revoked.push_back(c);
      }
    }
  }
  for (auto& c : revoked) (void)c->ch->cancel(kCancelWaitMs, "revoked");
}

void server::state::watch_loop() {
  std::unique_lock lock(watch_m);
  while (!stopping.load()) {
    watch_cv.wait_for(lock, std::chrono::milliseconds(kDevicesRecheckMs), [&] { return stopping.load(); });
    if (stopping.load()) return;
    lock.unlock();
    cut_unpaired();
    lock.lock();
  }
}

result<std::unique_ptr<server>> server::start(const options& o) {
  if (!o.store || o.cfg.roots.empty()) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);
  auto key = o.store->get(kAccessKeyName);
  if (!key || key->size() != 32) return err(status::not_found);
  std::unique_ptr<server> srv(new server());
  srv->st_ = std::make_unique<state>();
  state& st = *srv->st_;
  st.cfg = o.cfg;
  st.store = o.store;
  st.loopback_only = o.loopback_only;
  st.max_sessions = o.max_sessions;
  st.max_pending = o.max_pending;
  st.per_minute = o.per_minute;
  st.limits = source_limit(o.per_minute, std::chrono::minutes(1), kLimitSources);
  st.block_limits = source_limit(o.per_minute > INT_MAX / 4 ? INT_MAX : o.per_minute * 4, std::chrono::minutes(1), kLimitSources);
  st.per_source_pending = o.per_source_pending;
  st.per_block_pending = o.per_block_pending;
  std::memcpy(st.access_key.data(), key->data(), 32);
  sodium_memzero(key->data(), key->size());
  // Read, never made here: a key that cannot be read (its mode changed by a
  // restore, say) must not be replaced, or every phone that pinned it is lost.
  auto identity = o.store->get(known::kIdentityName);
  if (!identity || identity->size() != st.identity.size()) return err(status::not_found);
  std::memcpy(st.identity.data(), identity->data(), st.identity.size());
  sodium_memzero(identity->data(), identity->size());
  net::reserve_sockets(kReserveSockets);
  if (o.make_listener) {
    st.listen = o.make_listener();
  } else {
    auto l = net::listen_internet(o.cfg.port, o.loopback_only, kBacklog);
    if (!l) return err(l.error());
    st.listen = std::move(*l);
  }
  st.port = st.listen->port();
  st.acceptor = std::thread([&st] { st.accept_loop(); });
  st.watcher = std::thread([&st] { st.watch_loop(); });
  return srv;
}

std::uint16_t server::port() const noexcept { return st_->port; }

counters server::stats() const noexcept {
  counters c;
  c.accepted = st_->n_accepted.load();
  c.over_limit = st_->n_over.load();
  c.bad_hello = st_->n_hello.load();
  c.no_key = st_->n_key.load();
  c.replayed = st_->n_replay.load();
  c.not_paired = st_->n_unpaired.load();
  c.sessions = st_->n_sessions.load();
  c.evicted = st_->n_evicted.load();
  return c;
}

void server::recheck_devices() noexcept {
  try {
    st_->cut_unpaired();
  } catch (...) {
  }
}

void server::stop() noexcept {
  if (!st_ || st_->stopping.exchange(true)) return;
  st_->watch_cv.notify_all();
  if (st_->listen) st_->listen->shutdown();  // a start that failed has none
  if (st_->acceptor.joinable()) st_->acceptor.join();
  if (st_->watcher.joinable()) st_->watcher.join();
  std::vector<std::shared_ptr<conn>> live;
  {
    std::lock_guard lock(st_->conns_m);
    for (auto& c : st_->conns) live.push_back(c);
  }
  for (auto& c : live) {
    c->l->shutdown();  // a handshake still in progress returns
    std::lock_guard lock(c->m);
    if (c->ch) c->ch->shutdown();
  }
  std::list<std::shared_ptr<conn>> all;
  {
    std::lock_guard lock(st_->conns_m);
    all.swap(st_->conns);
  }
  for (auto& c : all) {
    if (c->t.joinable()) c->t.join();
  }
}

server::~server() { stop(); }

bool set_up(secrets::store& store, std::array<std::uint8_t, 32>& identity_public, std::array<std::uint8_t, 32>& access_key,
            bool& made) {
  if (sodium_init() < 0) return false;
  made = false;
  // Something there that cannot be used is not "nothing yet".
  const auto unusable = [&store](const char* name) {
    if (!store.exists(name)) return false;
    auto v = store.get(name);
    const bool bad = !v || v->size() != 32;
    if (v) sodium_memzero(v->data(), v->size());
    return bad;
  };
  if (unusable(known::kIdentityName) || unusable(kAccessKeyName)) return false;
  bool kept = false;
  auto identity = known::identity(&store, kept);
  if (!kept) return false;
  crypto_scalarmult_base(identity_public.data(), identity.data());
  sodium_memzero(identity.data(), identity.size());
  auto existing = store.get(kAccessKeyName);
  if (existing && existing->size() == access_key.size()) {
    std::memcpy(access_key.data(), existing->data(), access_key.size());
    sodium_memzero(existing->data(), existing->size());
    return true;
  }
  randombytes_buf(access_key.data(), access_key.size());
  if (!store.put(kAccessKeyName, access_key)) return false;
  auto back = store.get(kAccessKeyName);
  if (!back || back->size() != access_key.size()) return false;
  sodium_memzero(back->data(), back->size());
  made = true;
  return true;
}

}  // namespace mv::driftwire::serve
