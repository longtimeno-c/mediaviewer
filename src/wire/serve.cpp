// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/serve.h"

#include <sodium.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "wire/admission.h"
#include "wire/channel.h"
#include "wire/grants.h"
#include "wire/known.h"
#include "wire/net.h"
#include "wire/keys.h"
#include "wire/owner_pass.h"
#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire::serve {

namespace {

using clock = std::chrono::steady_clock;

// A hello that is not all there this soon is not a phone's (it is about 1.2 KB).
constexpr int kHelloMs = 3000;
constexpr int kServeHelloMs = 5000;
// The desktop writing the claim's nonce over SMB, at most.
constexpr int kClaimProofMs = 12000;
// The desktop's answer to the passphrase step of a claim: a person may be
// typing it, then Argon2id runs (about a second at 256 MiB).
constexpr int kClaimAnswerMs = 120000;
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
  net::address from;
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
  bool is_owner = false;    // set before its session starts
  // A device with a limit (grants.h): the grant it came in with, set before
  // its session starts; nullopt for an unlimited one and the owner.
  std::optional<grants::grant> grant;
  // The owner's passphrase on this session (its session thread's own):
  // the challenge outstanding, and the unlock with the verifier it was for.
  owner::key32 nonce{};
  bool has_nonce = false;
  std::chrono::steady_clock::time_point nonce_at{};
  std::chrono::steady_clock::time_point unlocked_until{};  // steady: a clock set back never stretches it
  owner::key32 unlocked_pk{};
  std::int64_t unlocked_set = 0;
  std::atomic<bool> done{false};
  std::thread t;
};

struct waiting_code {
  std::array<std::uint8_t, 32> hash{};
  clock::time_point deadline;
  grants::limit gives;  // a guest code's limit; none: an unlimited pairing
};

// The Catalogue root, which every grant may read (docs/plans/nas-owner-and-guests.md §5.2).
bool is_catalogue(const root_config& r) {
  const std::string tail = std::string("/") + kCatalogueFolder;
  return r.label == kCatalogueLabel && r.path.size() >= tail.size() &&
         r.path.compare(r.path.size() - tail.size(), tail.size(), tail) == 0;
}

std::array<std::uint8_t, 32> code_hash(std::string_view code) {
  std::array<std::uint8_t, 32> h{};
  crypto_generichash(h.data(), h.size(), reinterpret_cast<const unsigned char*>(code.data()), code.size(), nullptr, 0);
  return h;
}

bool lower_hex(std::string_view s, std::size_t n) noexcept {
  return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string new_root_id() {
  std::array<std::uint8_t, 8> b{};
  randombytes_buf(b.data(), b.size());
  std::string out(16, '0');
  sodium_bin2hex(out.data(), out.size() + 1, b.data(), b.size());
  return out;
}

}  // namespace

struct server::state {
  // The named folders change while sessions run (the owner's add_root and
  // remove_root): each request takes a snapshot, and a session keeps none.
  mutable std::mutex cfg_m;
  std::shared_ptr<const config> cfg_now;  // under cfg_m
  std::string service_name;
  [[nodiscard]] std::shared_ptr<const config> cfg() const {
    std::lock_guard lock(cfg_m);
    return cfg_now;
  }
  bool claimable = false, lan_only = false;
  std::function<bool(const net::address&)> own_network;
  std::function<bool(const config&)> save_config;
  std::string devices_lock;
  std::mutex devices_m, claim_m;
  std::mutex codes_m;
  std::vector<waiting_code> codes;  // under codes_m
  // Codes taken whose device is not saved yet: still "waiting" to the owner,
  // so the desktop never sees a spent code before the phone it added.
  int redeeming = 0;  // under codes_m

  [[nodiscard]] bool on_own_network(const net::address& a) const {
    if (own_network) return own_network(a);
    const auto nets = net::local_subnets();
    return net::on_link(a, nets);
  }
  [[nodiscard]] std::optional<std::array<std::uint8_t, 32>> owner() const {
    auto v = store->get(kOwnerName);
    if (!v || v->size() != 32) return std::nullopt;
    std::array<std::uint8_t, 32> k;
    std::memcpy(k.data(), v->data(), k.size());
    return k;
  }
  // Single use: it is gone from the list before anything else can look.
  // The limit it gives (none: an unlimited pairing), or nullopt.
  [[nodiscard]] std::optional<grants::limit> take_code(std::string_view code, bool redeem = false) {
    if (!lower_hex(code, kPairCodeChars)) return std::nullopt;
    const auto want = code_hash(code);
    const auto now = clock::now();
    std::lock_guard lock(codes_m);
    std::erase_if(codes, [&](const waiting_code& c) { return c.deadline <= now; });
    for (auto it = codes.begin(); it != codes.end(); ++it) {
      if (sodium_memcmp(it->hash.data(), want.data(), want.size()) == 0) {
        grants::limit gives = it->gives;
        codes.erase(it);
        if (redeem) ++redeeming;
        return gives;
      }
    }
    return std::nullopt;
  }
  // The file lock against `serve add` / `serve revoke`, when there is one.
  [[nodiscard]] std::unique_ptr<net::instance_lock> lock_devices() const {
    return devices_lock.empty() ? nullptr : net::try_lock_instance(devices_lock);
  }
  [[nodiscard]] bool may_change_devices(std::unique_ptr<net::instance_lock>& held) const {
    held = lock_devices();
    return devices_lock.empty() || held != nullptr;
  }
  [[nodiscard]] bool add_device(const std::string& name, const std::array<std::uint8_t, 32>& key, bool& full);
  // Puts a device on the right list (grants.h): unlimited (`g` nullopt) in
  // the paired list, limited in the grants, never both. `full`: no room.
  [[nodiscard]] bool set_access(const std::string& name, const std::array<std::uint8_t, 32>& key,
                                const std::optional<grants::grant>& g, bool& full);
  enum class access { none, full, live, expired, unreadable };
  struct device_access {
    access a = access::none;
    std::optional<grants::grant> g;
  };
  [[nodiscard]] device_access access_of(const std::array<std::uint8_t, 32>& key);
  struct lists {
    std::vector<known::device> devices;
    std::vector<grants::grant> limited;
    bool grants_ok = false;
  };
  [[nodiscard]] lists read_lists();
  // Why a running session must end now, or nullptr.
  [[nodiscard]] const char* verdict(const lists& l, const conn& c, std::int64_t t) const;
  [[nodiscard]] std::shared_ptr<const config> cfg_for(const std::optional<grants::grant>& g) const;
  [[nodiscard]] bool publish_config(config next);
  // Every session but `except`: the named folders changed under it.
  void cut_sessions(const conn* except);
  // The claim keys on the share, each with the named folder it was found under.
  [[nodiscard]] std::vector<std::pair<std::array<std::uint8_t, 32>, std::string>> claim_tokens() const;
  void run_claim(conn& c, std::unique_ptr<channel> ch, const std::string& folder);
  void send_claimed(channel& ch);
  // A serve_pair from a device that is not unlimited (`m`): spends the code
  // and puts the device on the list its limit says. `live`: the device has a
  // live grant, so a code that is no good is not a refusal (it goes on, as an
  // unlimited phone that scans again does). False: refused, and said.
  enum class redeemed { paired, bad_code_live, refused };
  [[nodiscard]] redeemed redeem(channel& ch, const std::array<std::uint8_t, 32>& peer, const message& m, bool live);
  [[nodiscard]] std::pair<end_code, std::string> admin(conn& c, std::string_view request);

  // ---- the owner's passphrase (owner_pass.h) ----
  std::function<std::int64_t()> now_unix;
  std::function<clock::time_point()> now_steady;
  [[nodiscard]] clock::time_point steady() const { return now_steady ? now_steady() : clock::now(); }
  std::chrono::milliseconds min_answer_gap{1000};
  std::mutex pass_m;  // the lockout: read, changed and written under it
  clock::time_point last_answer[2]{};  // under pass_m: claim, owner
  enum which_counter : int { claim_counter = 0, owner_counter = 1 };
  [[nodiscard]] std::int64_t now() const { return now_unix ? now_unix() : static_cast<std::int64_t>(std::time(nullptr)); }
  // The counter's lock, if it is locked now (0: not).
  [[nodiscard]] std::int64_t locked_until(which_counter which);
  // A signed answer to `nonce`, checked and counted. `why` and `until` say
  // why not: "locked-out" (until), "slow-down", "passphrase".
  // `v` is the verifier the challenge was made from: one changed since (a
  // reset on the NAS while the answer was being typed) refuses it.
  [[nodiscard]] bool answer(which_counter which, const owner::verifier& v, const owner::signature& sig, const channel& ch,
                            const owner::key32& nonce, std::string& why, std::int64_t& until);
  // Whether `c` may make a change that adds access now (§3.4). Not: the
  // refusal to send.
  [[nodiscard]] std::optional<std::pair<end_code, std::string>> needs_unlock(conn& c);
  [[nodiscard]] std::string owner_fields();

  secrets::store* store = nullptr;
  bool loopback_only = false;
  int max_sessions = 8;
  int max_pending = 16;
  int per_minute = 30;

  std::array<std::uint8_t, 32> identity{}, access_key{};
  std::array<std::uint8_t, 32> identity_public{};  // what clients pin, and sign over
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
  std::atomic<std::uint64_t> n_paired{0}, n_bad_codes{0}, n_wrong_pass{0}, n_unlocks{0}, n_expired{0};

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


  void accept_loop();
  bool take_token(clock::time_point now) noexcept;
  bool evict_oldest_hello(clock::time_point now);
  void reap();
  void run(conn* c);
  void serve_connection(conn* c);
  void release_pending(conn& c);
  void session(conn& c, std::optional<message> hello);
  void watch_loop();
  void cut_unpaired();
};

// ---- a session ------------------------------------------------------------------------------

namespace {

class session_state {
 public:
  using send_fn = std::function<bool(msg, std::span<const std::uint8_t>)>;
  using config_fn = std::function<std::shared_ptr<const config>()>;
  using admin_fn = std::function<std::pair<end_code, std::string>(std::string_view)>;
  session_state(send_fn send, config_fn cfg, std::uint32_t ops, admin_fn admin = {})
      : send_(std::move(send)), cfg_(std::move(cfg)), ops_(ops), admin_(std::move(admin)) {}

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
      case msg::serve_admin: {
        const auto a = parse_admin(m.body);
        if (!a) return false;
        // Only an owner's session has the handler; to anyone else the answer is no.
        if (!admin_ || (ops_ & kAdminOp) == 0) return send_end(a->id, end_code::refused);
        const auto [code, json] = admin_(a->json);
        return send_end(a->id, code, json);
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

  static const root_config* find_root(const config& cfg, const std::string& id) noexcept {
    for (const auto& r : cfg.roots) {
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
    // This request's view of the named folders, held until it has opened.
    const auto cfg = cfg_();
    if (r.kind == op::roots) {
      std::vector<root_info> out;
      for (const auto& c : cfg->roots) out.push_back({c.id, c.label});
      return send_end(r.id, end_code::ok, roots_json(out));
    }
    const root_config* root = find_root(*cfg, r.root);
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
  config_fn cfg_;
  const std::uint32_t ops_;
  admin_fn admin_;
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
  const auto held = std::make_shared<const config>(cfg);
  session_state s([](msg, std::span<const std::uint8_t>) { return true; }, [&held] { return held; }, ops);
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

void server::state::session(conn& c, std::optional<message> first_message) {
  channel& ch = *c.ch;
  message m;
  // The first message after ident: what the client can do (already read for
  // a device with a limit).
  bool first = true;
  if (first_message) {
    m = std::move(*first_message);
  } else {
    first = ch.receive(m, kServeHelloMs).has_value();
  }
  // An unlimited phone that scans again: it is told so and goes on, unlimited
  // (a guest code never limits it). A code it carries is spent here, so that
  // code can pair nothing else.
  if (first && m.type == msg::serve_pair) {
    if (const auto p = parse_pair(m.text())) (void)take_code(p->code);
    if (!ch.send(msg::serve_paired, std::string_view("{}"))) return;
    first = ch.receive(m, kServeHelloMs).has_value();
  }
  if (!first || m.type != msg::serve_hello) {
    (void)ch.cancel(kCancelWaitMs, "protocol");
    return;
  }
  const auto hello = parse_hello(m.text());
  std::uint32_t ops = 0;
  std::uint32_t version = kVersion;
  if (hello) {
    version = std::clamp(hello->version, kMinVersion, kVersion);
    if (hello->min_version <= kVersion && hello->version >= kMinVersion) ops = hello->ops & (kAllOps | (c.is_owner ? kAdminOp : 0));
  }
  std::string extra = (ops & kAdminOp) != 0 ? owner_fields() : std::string();
  if (c.grant) {
    // A limited phone is told its limit, so it can say so, and say it ended even offline.
    json::writer w;
    w.begin_object().key("grant").begin_object().key("expires").integer(c.grant->expires);
    w.key("scoped").boolean(c.grant->roots.has_value()).end_object().end_object();
    const std::string g = w.take();
    extra = g.substr(1, g.size() - 2);
  }
  if (!ch.send(msg::serve_caps, caps_json(version, ops, extra))) return;
  if (ops == 0) {
    // Nothing in common (one side too old). The answer said so.
    ch.flush(kCancelWaitMs);
    (void)ch.cancel(kCancelWaitMs, "versions");
    return;
  }
  session_state::admin_fn admin_handler;
  if (c.is_owner) admin_handler = [this, &c](std::string_view request) { return admin(c, request); };
  session_state s([&ch](msg type, std::span<const std::uint8_t> body) { return ch.send(type, body).has_value(); },
                  [this, g = c.grant] { return cfg_for(g); }, ops, std::move(admin_handler));
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
    // A claim: only from its own network, and the share is read only while
    // nobody owns the service, or (owned) for a hello the access key does not
    // open, so a phone's session never touches it. Owned, a claim is the
    // owner's own claiming again after its answer was lost; anyone else's
    // ends in the same silence as a wrong key.
    std::string claim_folder;
    bool claiming = false;
    if (claimable && on_own_network(c->from) &&
        (!store->exists(kOwnerName) || !channel::hello_opens_under(*h, access_key, identity))) {
      for (const auto& [key, folder] : claim_tokens()) {
        if (channel::hello_opens_under(*h, key, identity)) {
          o.access_key = key;
          claim_folder = folder;
          claiming = true;
          break;
        }
      }
    }
    o.fresh_first = [this](std::span<const std::uint8_t> first) { return fresh(first); };
    auto ch = channel::open_library_server(c->l, std::move(*h), "0000000000000000", service_name, o);
    release_pending(*c);
    if (!ch) {
      if (ch.error() == status::permission_denied) n_key.fetch_add(1);
      c->l->shutdown();
      return;
    }
    if (claiming) {
      run_claim(*c, std::move(*ch), claim_folder);
      return;
    }
    opened = std::move(*ch);
  }
  const auto peer = opened->peer_key();
  const auto owner_key = peer ? owner() : std::nullopt;
  c->is_owner = peer && owner_key && *owner_key == *peer;
  if (!peer) {
    n_unpaired.fetch_add(1);
    (void)opened->cancel(kCancelWaitMs, "not-paired");
    return;
  }
  // An unlimited device goes straight on; anyone else's first word decides
  // (docs/plans/nas-owner-and-guests.md §5.2): a code is redeemed, even by a
  // guest whose time is up (re-inviting it); otherwise only a live grant
  // goes on, with its hello.
  std::optional<message> hello;
  if (!c->is_owner) {
    auto acc = access_of(*peer);
    if (acc.a != access::full) {
      message m;
      if (!opened->receive(m, kServeHelloMs)) {
        n_unpaired.fetch_add(1);
        (void)opened->cancel(kCancelWaitMs, "not-paired");
        return;
      }
      if (m.type == msg::serve_pair) {
        const auto r = redeem(*opened, *peer, m, acc.a == access::live);
        if (r == redeemed::refused) {
          n_unpaired.fetch_add(1);
          (void)opened->cancel(kCancelWaitMs, "not-paired");  // says nothing new after a bad code's own reason
          return;
        }
        if (r == redeemed::bad_code_live && !opened->send(msg::serve_paired, std::string_view("{}"))) return;
        acc = access_of(*peer);
        if (acc.a != access::full && acc.a != access::live) {
          (void)opened->cancel(kCancelWaitMs, "error");
          return;
        }
      } else if (acc.a == access::live) {
        hello = std::move(m);
      } else {
        if (acc.a == access::expired) {
          n_expired.fetch_add(1);
          (void)opened->cancel(kCancelWaitMs, "expired");
        } else {
          n_unpaired.fetch_add(1);
          (void)opened->cancel(kCancelWaitMs, "not-paired");
        }
        return;
      }
    }
    c->grant = acc.a == access::live ? acc.g : std::nullopt;
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
  session(*c, std::move(hello));
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
    // The desktop's nearby service: this network only, and silently.
    if (room && lan_only && !on_own_network(from)) room = false;
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
    c->from = from;
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

std::vector<std::pair<std::array<std::uint8_t, 32>, std::string>> server::state::claim_tokens() const {
  std::vector<std::pair<std::array<std::uint8_t, 32>, std::string>> out;
  const auto now = cfg();
  for (const root_config& r : now->roots) {
    io::file_reader f;
    if (!f.open_beneath(r.path, kClaimTokenFile)) continue;
    if (f.size() < 64 || f.size() > 66) continue;
    std::array<std::uint8_t, 66> buf;
    auto got = f.read_at(0, std::span<std::uint8_t>(buf).first(static_cast<std::size_t>(f.size())));
    if (!got || *got < 64) continue;
    const std::string_view token(reinterpret_cast<const char*>(buf.data()), 64);
    if (!lower_hex(token, 64)) continue;
    out.emplace_back(claim_key(token), r.path);
  }
  return out;
}

bool server::state::publish_config(config next) {
  if (save_config && !save_config(next)) return false;
  auto made = std::make_shared<const config>(std::move(next));
  std::lock_guard lock(cfg_m);
  cfg_now = std::move(made);
  return true;
}

// ---- the owner's passphrase -----------------------------------------------------------------

namespace {

std::string hex_of(std::span<const std::uint8_t> b) {
  std::string out(b.size() * 2, '0');
  sodium_bin2hex(out.data(), out.size() + 1, b.data(), b.size());
  return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> from_hex(const std::string* s) {
  if (!s || s->size() != N * 2) return std::nullopt;
  std::array<std::uint8_t, N> out{};
  std::size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), s->data(), s->size(), nullptr, &len, nullptr) != 0 || len != N) return std::nullopt;
  return out;
}

std::pair<end_code, std::string> refusal_json(std::string_view why, std::int64_t until = 0) {
  json::writer w;
  w.begin_object().key("why").string(why);
  if (until > 0) w.key("until").integer(until);
  w.end_object();
  return {end_code::refused, w.take()};
}

// The salt, the cost and a nonce: what a client needs to sign.
void challenge_fields(json::writer& w, const owner::verifier& v, const owner::key32& nonce) {
  w.key("salt").string(hex_of(v.salt));
  w.key("ops").integer(static_cast<std::int64_t>(v.ops));
  w.key("mem").integer(static_cast<std::int64_t>(v.mem));
  w.key("nonce").string(hex_of(nonce));
}

// {"salt","ops","mem","pk"}: a verifier a client made, checked before it is kept.
std::optional<owner::verifier> parse_new_verifier(const json::value* v, std::int64_t now) {
  if (!v || v->k != json::kind::object) return std::nullopt;
  const auto ops = v->integer("ops");
  const auto mem = v->integer("mem");
  const auto salt = from_hex<owner::kSaltBytes>(v->str("salt"));
  const auto pk = from_hex<32>(v->str("pk"));
  if (!ops || !mem || *ops <= 0 || *mem <= 0 || !salt || !pk) return std::nullopt;
  if (!owner::sane_cost(static_cast<std::uint64_t>(*ops), static_cast<std::uint64_t>(*mem))) return std::nullopt;
  owner::verifier out;
  out.ops = static_cast<std::uint64_t>(*ops);
  out.mem = static_cast<std::uint64_t>(*mem);
  out.salt = *salt;
  out.pk = *pk;
  out.set = now;
  return out;
}

const char* stored_name(owner::stored s) {
  return s == owner::stored::set ? "set" : s == owner::stored::none ? "none" : "broken";
}

}  // namespace

std::int64_t server::state::locked_until(which_counter which) {
  std::lock_guard lock(pass_m);
  const auto t = now();
  const auto l = owner::load_lockout(*store, t);
  const owner::counter& c = which == claim_counter ? l.claim : l.owner;
  return c.locked(t) ? c.until : 0;
}

bool server::state::answer(which_counter which, const owner::verifier& v, const owner::signature& sig, const channel& ch,
                           const owner::key32& nonce, std::string& why, std::int64_t& until) {
  const auto peer = ch.peer_key();
  if (!peer) {
    why = "passphrase";
    return false;
  }
  std::lock_guard lock(pass_m);
  owner::verifier current;
  if (owner::load(*store, current) != owner::stored::set || current.pk != v.pk || current.set != v.set) {
    why = "passphrase-changed";
    return false;
  }
  const auto t = now();
  auto l = owner::load_lockout(*store, t);
  owner::counter& c = which == claim_counter ? l.claim : l.owner;
  if (c.locked(t)) {
    why = "locked-out";
    until = c.until;
    return false;
  }
  const auto at = steady();
  if (last_answer[which] != clock::time_point{} && at - last_answer[which] < min_answer_gap) {
    why = "slow-down";
    return false;
  }
  last_answer[which] = at;
  // Counted as a miss before it is checked: a store that cannot be written
  // must never mean guesses that are never counted.
  c.miss(t);
  if (!owner::save_lockout(*store, l)) {
    why = "error";
    return false;
  }
  if (owner::check(current, sig, std::span<const std::uint8_t, owner::kBindingBytes>(ch.handshake_hash()), nonce,
                   identity_public, *peer)) {
    c.success();
    (void)owner::save_lockout(*store, l);  // if this fails the miss stays counted: never in the guesser's favour
    return true;
  }
  n_wrong_pass.fetch_add(1);
  why = c.locked(t) ? "locked-out" : "passphrase";
  until = c.locked(t) ? c.until : 0;
  return false;
}

std::optional<std::pair<end_code, std::string>> server::state::needs_unlock(conn& c) {
  owner::verifier v;
  switch (owner::load(*store, v)) {
    case owner::stored::none:
      return std::nullopt;  // none set yet: as before this existed (call C4)
    case owner::stored::unreadable:
      return std::pair<end_code, std::string>{end_code::error, {}};
    case owner::stored::set:
      break;
  }
  if (c.unlocked_until > steady() && c.unlocked_pk == v.pk && c.unlocked_set == v.set) return std::nullopt;
  return refusal_json("needs-unlock");
}

std::string server::state::owner_fields() {
  owner::verifier v;
  const auto s = owner::load(*store, v);
  json::writer w;
  w.begin_object().key("features").begin_array().string(kFeaturePassphrase).end_array();
  w.key("passphrase").string(stored_name(s)).end_object();
  std::string out = w.take();
  return out.substr(1, out.size() - 2);
}

void server::state::run_claim(conn& c, std::unique_ptr<channel> ch, const std::string& folder) {
  message m;
  const auto peer = ch->peer_key();
  if (!peer || !ch->receive(m, kServeHelloMs) || m.type != msg::serve_claim) {
    (void)ch->cancel(kCancelWaitMs, "protocol");
    return;
  }
  (void)c;
  // A desktop that can take the passphrase step says so; one from before it
  // does not, and claims as it always did while no passphrase is set.
  bool speaks_passphrase = false;
  if (const auto first = json::parse(m.text(), 2)) speaks_passphrase = first->integer("passphrase") == 1;
  // Owned and not its owner: closed with no reason given, before any
  // challenge, so this ends as a refusal and never as "it may have gone
  // through". The caller can still tell this from a challenge (unowned) or a
  // failed handshake (no token), but only a caller that can read the token on
  // the share gets this far: reading it is enough to learn whether the
  // service is owned (claiming still needs the write proof).
  if (store->exists(kOwnerName)) {
    const auto held = owner();
    if (!held || *held != *peer) {
      ch->shutdown();
      return;
    }
  }
  // The token proves this desktop can read the share; anyone with a read-only
  // account could. Now it must write: a nonce of ours, into the same folder.
  std::array<std::uint8_t, 32> nonce_bytes{};
  randombytes_buf(nonce_bytes.data(), nonce_bytes.size());
  const std::string nonce = key_hex(nonce_bytes);
  {
    json::writer w;
    w.begin_object().key("nonce").string(nonce).end_object();
    if (!ch->send(msg::serve_claim_check, w.str()) || !ch->receive(m, kClaimProofMs) || m.type != msg::serve_claim) {
      (void)ch->cancel(kCancelWaitMs, "protocol");
      return;
    }
    io::file_reader f;
    std::array<std::uint8_t, 64> got{};
    bool proven = f.open_beneath(folder, kClaimProofFile) && f.size() >= got.size() && f.size() <= got.size() + 2;
    if (proven) {
      auto n = f.read_at(0, got);
      proven = n && *n == got.size() && sodium_memcmp(got.data(), nonce.data(), got.size()) == 0;
    }
    if (!proven) {
      (void)ch->cancel(kCancelWaitMs, "not-written");
      return;
    }
  }
  // Only now, past the write proof, is anything said about a passphrase
  // (docs/plans/nas-owner-and-guests.md §3.3): a reader of the share learns
  // nothing about it. The owner's own re-claim is counted on the owner's
  // counter, so guesses at claims cannot lock it out.
  // Owned since the top of this function (another desktop got there while
  // this one wrote its proof): only that owner goes on, and only the owner's
  // own claim is counted on the owner's counter.
  which_counter counter = claim_counter;
  if (store->exists(kOwnerName)) {
    const auto held = owner();
    if (!held || *held != *peer) {
      ch->shutdown();
      return;
    }
    counter = owner_counter;
  }
  std::optional<owner::verifier> to_set;
  {
    owner::verifier v;
    const auto s = owner::load(*store, v);
    if (s == owner::stored::unreadable) {
      (void)ch->cancel(kCancelWaitMs, "error");
      return;
    }
    if (s == owner::stored::set) {
      if (!speaks_passphrase) {
        (void)ch->cancel(kCancelWaitMs, "passphrase");  // an older desktop: it must be updated
        return;
      }
      if (locked_until(counter) != 0) {
        (void)ch->cancel(kCancelWaitMs, "locked-out");
        return;
      }
      owner::key32 nonce{};
      randombytes_buf(nonce.data(), nonce.size());
      json::writer w;
      w.begin_object().key("passphrase").string("set");
      challenge_fields(w, v, nonce);
      w.end_object();
      if (!ch->send(msg::serve_claim_challenge, w.str()) || !ch->receive(m, kClaimAnswerMs) || m.type != msg::serve_claim) {
        (void)ch->cancel(kCancelWaitMs, "protocol");
        return;
      }
      const auto doc = json::parse(m.text(), 2);
      const auto sig = doc ? from_hex<64>(doc->str("sig")) : std::nullopt;
      std::string why;
      std::int64_t until = 0;
      if (!sig) {
        (void)ch->cancel(kCancelWaitMs, "protocol");
        return;
      }
      if (!answer(counter, v, *sig, *ch, nonce, why, until)) {
        (void)ch->cancel(kCancelWaitMs, why);
        return;
      }
    } else if (speaks_passphrase) {
      // None yet: the claim chooses it, and only its public key comes here.
      if (!ch->send(msg::serve_claim_challenge, std::string_view("{\"passphrase\":\"none\"}")) || !ch->receive(m, kClaimAnswerMs) ||
          m.type != msg::serve_claim) {
        (void)ch->cancel(kCancelWaitMs, "protocol");
        return;
      }
      const auto doc = json::parse(m.text(), 3);
      to_set = doc ? parse_new_verifier(doc->find("set"), now()) : std::nullopt;
      if (!to_set) {
        (void)ch->cancel(kCancelWaitMs, "protocol");
        return;
      }
    }
  }
  // Kept before the owner, so a service is never owned with the passphrase
  // its claim chose missing; never over one that is there (a racing claim
  // that set one first wins it, and this one loses below).
  // A passphrase that appeared meanwhile (the command line, or a racing
  // set_passphrase) is not this claim's: it fails rather than let the
  // desktop believe its own was kept.
  bool passphrase_taken = false;
  const auto keep_passphrase = [&]() -> bool {
    if (!to_set) return true;
    std::lock_guard pass(pass_m);
    owner::verifier there;
    if (owner::load(*store, there) != owner::stored::none) {
      passphrase_taken = true;
      return false;
    }
    return owner::save(*store, *to_set);
  };
  std::unique_lock lock(claim_m);
  // Two desktops at once: the first through this lock is the owner. The owner
  // claiming again (its first answer was lost) gets the same answer; nothing
  // changes.
  if (store->exists(kOwnerName)) {
    const auto held = owner();
    lock.unlock();
    if (!held || *held != *peer) {
      ch->shutdown();  // a racing claim lost: no reason given (see above for what the caller can tell)
      return;
    }
    if (!keep_passphrase()) {
      (void)ch->cancel(kCancelWaitMs, passphrase_taken ? "passphrase" : "error");
      return;
    }
    send_claimed(*ch);
    return;
  }
  const std::string catalogue = folder + "/" + kCatalogueFolder;
  config next = *cfg();
  if (std::none_of(next.roots.begin(), next.roots.end(), [&](const root_config& r) { return r.path == catalogue; }) &&
      next.roots.size() < kMaxRoots) {
    next.roots.push_back({new_root_id(), kCatalogueLabel, catalogue});
  }
  if (!keep_passphrase() || !publish_config(std::move(next))) {
    lock.unlock();
    (void)ch->cancel(kCancelWaitMs, passphrase_taken ? "passphrase" : "error");
    return;
  }
  if (!store->put(kOwnerName, *peer) || !owner()) {
    lock.unlock();
    (void)ch->cancel(kCancelWaitMs, "error");
    return;
  }
  lock.unlock();
  send_claimed(*ch);
}

void server::state::send_claimed(channel& ch) {
  std::vector<root_info> list;
  for (const auto& r : cfg()->roots) list.push_back({r.id, r.label});
  json::writer w;
  w.begin_object();
  w.key("access_key").string(key_hex(access_key));
  w.key("roots").raw(roots_json(list));
  w.end_object();
  (void)ch.send(msg::serve_claimed, w.str());
  ch.flush(kCancelWaitMs);
  (void)ch.cancel(kCancelWaitMs, "done");
}

bool server::state::add_device(const std::string& name, const std::array<std::uint8_t, 32>& key, bool& full) {
  return set_access(name, key, std::nullopt, full);
}

bool server::state::set_access(const std::string& name, const std::array<std::uint8_t, 32>& key,
                               const std::optional<grants::grant>& g, bool& full) {
  full = false;
  std::lock_guard lock(devices_m);
  std::unique_ptr<net::instance_lock> held;
  if (!may_change_devices(held)) return false;
  std::vector<known::device> devices;
  if (store->exists(known::kDevicesName)) {
    auto v = store->get(known::kDevicesName);
    if (!v) return false;  // a list that cannot be read is never replaced
    auto decoded = known::decode(*v);
    sodium_memzero(v->data(), v->size());
    if (!decoded) return false;
    devices = std::move(*decoded);
  }
  std::vector<grants::grant> limited;
  if (!grants::load(*store, limited)) return false;
  const auto t = now();
  (void)grants::prune(limited, t);
  const bool was_unlimited = std::any_of(devices.begin(), devices.end(), [&](const known::device& d) { return d.key == key; });
  const bool was_live = std::any_of(limited.begin(), limited.end(), [&](const grants::grant& x) { return x.key == key && !x.expired(t); });
  std::erase_if(devices, [&](const known::device& d) { return d.key == key; });
  std::erase_if(limited, [&](const grants::grant& x) { return x.key == key; });
  // Room for it, unless it is already one of the live ones (moving lists, or
  // renamed). An expired guest coming back is a new live one.
  const bool counted = was_unlimited || was_live;
  if (!counted && !grants::make_room(limited, devices.size(), t)) {
    full = true;
    return false;
  }
  if (g) {
    grants::grant next = *g;
    next.key = key;
    next.name = name;
    limited.push_back(std::move(next));
  } else {
    known::device d;
    d.name = name;
    d.key = key;
    devices.push_back(std::move(d));
  }
  // The list it goes to is written first, then the one it leaves, so it is
  // never, even for a moment, on neither (the watch would cut it). On both
  // for a moment, or after a failed second write, the grant wins (access_of,
  // cut_unpaired): never more access than either list gives.
  if (g) {
    if (!grants::save(*store, limited)) return false;
    if (!known::save(*store, devices)) return false;
  } else {
    if (!known::save(*store, devices) || known::load(*store).size() != devices.size()) return false;
    if (!grants::save(*store, limited)) return false;
  }
  return true;
}

server::state::device_access server::state::access_of(const std::array<std::uint8_t, 32>& key) {
  const auto once = [&](const lists& l) {
    device_access out;
    // A grant wins over the paired list: a key on both (a move between them
    // half done) gets the limit, never more.
    if (!l.grants_ok) {
      out.a = access::unreadable;  // nobody gets in on a list that cannot be read; the paired list still can
    } else {
      for (const auto& g : l.limited) {
        if (g.key != key) continue;
        out.a = g.expired(now()) ? access::expired : access::live;
        out.g = g;
        return out;
      }
    }
    if (std::any_of(l.devices.begin(), l.devices.end(), [&](const known::device& d) { return d.key == key; })) {
      out.a = access::full;
      return out;
    }
    if (out.a != access::unreadable) out.a = access::none;
    return out;
  };
  auto out = once(read_lists());
  if (out.a == access::none) {
    // The command line moves a device between the lists from another process:
    // a device on neither is read again before it is turned away.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    out = once(read_lists());
  }
  return out;
}

std::shared_ptr<const config> server::state::cfg_for(const std::optional<grants::grant>& g) const {
  auto all = cfg();
  if (!g || !g->roots) return all;
  auto mine = std::make_shared<config>(*all);
  std::erase_if(mine->roots, [&](const root_config& r) { return !g->allows(r.id, is_catalogue(r)); });
  return mine;
}

server::state::redeemed server::state::redeem(channel& ch, const std::array<std::uint8_t, 32>& peer, const message& m,
                                              bool live) {
  const auto p = parse_pair(m.text());
  // Taken and counted as redeeming under one lock, so the owner's devices
  // answer sees the code waiting or the phone saved, never neither.
  std::optional<grants::limit> gives = p ? take_code(p->code, true) : std::nullopt;  // counted by take_code
  // A code from `driftwire serve pair-code` (no desktop) is kept in the store
  // instead: unlimited in the old list, limited in its own. Spent under the
  // same locks as the lists (and the command line's file lock), so two
  // connections with one code cannot both read it before either spends it.
  if (p && !gives) {
    std::lock_guard lock(devices_m);
    std::unique_ptr<net::instance_lock> held;
    if (!may_change_devices(held)) {
      // The command line is changing the lists: not a bad code, try again.
      (void)ch.cancel(kCancelWaitMs, "busy");
      return redeemed::refused;
    }
    if (redeem_pair_code(*store, p->code, std::time(nullptr))) {
      gives = grants::limit{};
    } else {
      gives = grants::redeem_guest_code(*store, p->code, now());
    }
    if (gives) {
      std::lock_guard counted(codes_m);
      ++redeeming;
    }
  }
  if (!gives) {
    n_bad_codes.fetch_add(1);
    if (live) return redeemed::bad_code_live;
    (void)ch.cancel(kCancelWaitMs, "bad-code");
    return redeemed::refused;
  }
  struct done_redeeming {
    state& s;
    ~done_redeeming() {
      std::lock_guard lock(s.codes_m);
      --s.redeeming;
    }
  } counted{*this};
  std::optional<grants::grant> g;
  if (gives->any()) {
    grants::grant x;
    const auto t = now();
    x.expires = gives->for_s != 0 ? t + gives->for_s : 0;
    x.roots = gives->roots;
    x.guest = true;
    x.created = t;
    g = std::move(x);
  }
  bool full = false;
  if (!set_access(p->name.empty() ? std::string("Phone") : p->name, peer, g, full)) {
    (void)ch.cancel(kCancelWaitMs, full ? "too-many" : "error");
    return redeemed::refused;
  }
  n_paired.fetch_add(1);
  return ch.send(msg::serve_paired, std::string_view("{}")).has_value() ? redeemed::paired : redeemed::refused;
}

std::pair<end_code, std::string> server::state::admin(conn& c, std::string_view request) {
  const auto doc = json::parse(request, 4);
  const std::string* what = doc ? doc->str("do") : nullptr;
  if (!what) return {end_code::bad_request, {}};
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc->str(k);
    return v ? *v : std::string();
  };
  const auto device_key = [&]() -> std::optional<std::array<std::uint8_t, 32>> { return parse_key_hex(text("key")); };

  // ---- the passphrase (docs/plans/nas-owner-and-guests.md §3) ----
  if (*what == "owner_status") {
    owner::verifier v;
    const auto s = owner::load(*store, v);
    const bool unlocked = s == owner::stored::set && !needs_unlock(c);
    json::writer w;
    w.begin_object().key("passphrase").string(stored_name(s));
    w.key("unlocked_s").integer(
        unlocked ? std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(c.unlocked_until - steady()).count()) : 0);
    w.key("locked_until").integer(locked_until(owner_counter));
    w.end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "challenge") {
    owner::verifier v;
    const auto s = owner::load(*store, v);
    if (s == owner::stored::unreadable) return {end_code::error, {}};
    if (s == owner::stored::none) return refusal_json("no-passphrase");
    if (const auto until = locked_until(owner_counter)) return refusal_json("locked-out", until);
    randombytes_buf(c.nonce.data(), c.nonce.size());
    c.has_nonce = true;
    c.nonce_at = steady();
    json::writer w;
    w.begin_object();
    challenge_fields(w, v, c.nonce);
    w.end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "unlock") {
    const auto sig = from_hex<64>(doc->str("sig"));
    if (!sig) return {end_code::bad_request, {}};
    // One answer per challenge, and only a fresh one.
    const bool fresh = c.has_nonce && steady() - c.nonce_at <= std::chrono::seconds(owner::kChallengeSeconds);
    c.has_nonce = false;
    if (!fresh) return refusal_json("no-challenge");
    owner::verifier v;
    const auto s = owner::load(*store, v);
    if (s == owner::stored::unreadable) return {end_code::error, {}};
    if (s == owner::stored::none) return refusal_json("no-passphrase");
    std::string why;
    std::int64_t until = 0;
    if (!answer(owner_counter, v, *sig, *c.ch, c.nonce, why, until)) return refusal_json(why, until);
    n_unlocks.fetch_add(1);
    c.unlocked_until = steady() + std::chrono::seconds(owner::kUnlockSeconds);
    c.unlocked_pk = v.pk;
    c.unlocked_set = v.set;
    json::writer w;
    w.begin_object().key("unlocked_s").integer(owner::kUnlockSeconds).end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "lock") {
    c.unlocked_until = {};
    c.has_nonce = false;
    return {end_code::ok, "{}"};
  }
  if (*what == "set_passphrase") {
    // None yet: the owner's device key is enough (call C4). Set: unlocked.
    if (auto refused = needs_unlock(c)) return *refused;
    auto next = parse_new_verifier(doc->find("set"), now());
    if (!next) return {end_code::bad_request, {}};
    std::lock_guard lock(pass_m);
    if (!owner::save(*store, *next)) return {end_code::error, {}};
    owner::verifier back;
    if (owner::load(*store, back) != owner::stored::set || back.pk != next->pk) return {end_code::error, {}};
    c.unlocked_until = {};  // this session unlocks again, with the new one
    return {end_code::ok, "{}"};
  }
  // What adds access, or changes what is served, needs an unlocked session
  // once there is a passphrase (§3.4). Revoking and reading never do.
  if (*what == "add_device" || *what == "pair_code" || *what == "add_root" || *what == "remove_root") {
    // (grant and guest_code check for themselves.)
    if (auto refused = needs_unlock(c)) return *refused;
  }

  if (*what == "devices") {
    bool waiting = false;
    {
      std::lock_guard lock(codes_m);
      const auto now = clock::now();
      std::erase_if(codes, [&](const waiting_code& w) { return w.deadline <= now; });
      waiting = !codes.empty() || redeeming > 0;
    }
    json::writer w;
    w.begin_object().key("code_waiting").boolean(waiting).key("devices").begin_array();
    for (const auto& d : known::load(*store)) {
      w.begin_object().key("name").string(d.name).key("key").string(key_hex(d.key)).end_object();
    }
    w.end_array().end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "add_device") {
    const auto key = device_key();
    if (!key) return {end_code::bad_request, {}};
    bool full = false;
    if (!add_device(text("name"), *key, full)) return {full ? end_code::too_many : end_code::error, {}};
    return {end_code::ok, "{}"};
  }
  if (*what == "revoke") {
    const auto key = device_key();
    if (!key) return {end_code::bad_request, {}};
    {
      std::lock_guard lock(devices_m);
      std::unique_ptr<net::instance_lock> held;
      if (!may_change_devices(held)) return {end_code::error, {}};
      auto devices = known::load(*store);
      std::vector<grants::grant> limited;
      if (!grants::load(*store, limited)) return {end_code::error, {}};
      const auto before = devices.size() + limited.size();
      std::erase_if(devices, [&](const known::device& d) { return d.key == *key; });
      std::erase_if(limited, [&](const grants::grant& g) { return g.key == *key; });
      if (devices.size() + limited.size() == before) return {end_code::not_found, {}};
      if (!grants::save(*store, limited)) return {end_code::error, {}};
      if (!known::save(*store, devices) || known::load(*store).size() != devices.size()) return {end_code::error, {}};
    }
    cut_unpaired();
    return {end_code::ok, "{}"};
  }
  // ---- timed and scoped access (docs/plans/nas-owner-and-guests.md §5) ----
  if (*what == "grants") {
    // The limited devices, and expired ones still listed. `devices` never
    // lists them: a desktop from before this would serve them unlimited.
    std::vector<grants::grant> limited;
    if (!grants::load(*store, limited)) return {end_code::error, {}};
    const auto t = now();
    json::writer w;
    w.begin_object().key("now").integer(t).key("grants").begin_array();
    for (const auto& g : limited) {
      if (t >= g.expires + grants::kExpiredKeptSeconds && g.expires != 0) continue;
      w.begin_object().key("name").string(g.name).key("key").string(key_hex(g.key));
      w.key("expires").integer(g.expires).key("expired").boolean(g.expired(t)).key("guest").boolean(g.guest);
      if (g.roots) {
        w.key("roots").begin_array();
        for (const auto& r : *g.roots) w.string(r);
        w.end_array();
      }
      w.end_object();
    }
    w.end_array().end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "grant") {
    // {"key", "for_s" | "forever":true, "roots":[...] | "all_roots":true}:
    // a device's limit from now. Extending, widening or lifting it adds access
    // and needs an unlock; shortening or narrowing does not (§3.4).
    const auto key = device_key();
    if (!key) return {end_code::bad_request, {}};
    const auto acc = access_of(*key);
    if (acc.a == access::none) return {end_code::not_found, {}};
    if (acc.a == access::unreadable) return {end_code::error, {}};
    const auto t = now();
    const auto for_s = doc->integer("for_s");
    const bool forever = doc->boolean("forever") == true;
    if (forever == for_s.has_value() || (for_s && (*for_s <= 0 || *for_s > grants::kMaxSeconds))) return {end_code::bad_request, {}};
    std::optional<std::vector<std::string>> roots;
    const bool all_roots = doc->boolean("all_roots") == true;
    if (const json::value* r = doc->find("roots")) {
      if (all_roots || r->k != json::kind::array || r->a.size() > grants::kMaxRootIds) return {end_code::bad_request, {}};
      std::vector<std::string> ids;
      for (const auto& e : r->a) {
        if (e.k != json::kind::string || !lower_hex(e.s, 16)) return {end_code::bad_request, {}};
        ids.push_back(e.s);
      }
      roots = std::move(ids);
    } else if (!all_roots) {
      roots = acc.g ? acc.g->roots : std::nullopt;  // unchanged
    }
    const std::int64_t old_expires = acc.a == access::full ? 0 : acc.g->expires;
    const auto old_roots = acc.a == access::full ? std::nullopt : acc.g->roots;
    const std::int64_t new_expires = forever ? 0 : t + *for_s;
    // Later than it was (or none, where it had one), or a folder it could not see.
    const bool longer = (new_expires == 0 && old_expires != 0) || (old_expires != 0 && new_expires > std::max(old_expires, t));
    const bool wider = old_roots.has_value() && (!roots || std::any_of(roots->begin(), roots->end(), [&](const std::string& id) {
      return std::find(old_roots->begin(), old_roots->end(), id) == old_roots->end();
    }));
    if (longer || wider) {
      if (auto refused = needs_unlock(c)) return *refused;
    }
    std::optional<grants::grant> g;
    if (new_expires != 0 || roots) {
      grants::grant x;
      x.expires = new_expires;
      x.roots = roots;
      x.guest = acc.g && acc.g->guest;
      x.created = acc.g ? acc.g->created : t;
      g = std::move(x);
    }
    const std::string name = acc.a == access::full ? [&] {
      for (const auto& d : known::load(*store)) {
        if (d.key == *key) return d.name;
      }
      return std::string("Phone");
    }() : acc.g->name;
    bool full = false;
    if (!set_access(name, *key, g, full)) return {full ? end_code::too_many : end_code::error, {}};
    cut_unpaired();  // a shortened or narrowed one takes effect now
    json::writer w;
    w.begin_object().key("expires").integer(new_expires).end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "guest_code") {
    // {"code","ttl_ms","for_s","roots"?}: a pairing code whose phone gets a
    // limit, from when it is redeemed (call C7). Its own verb, so a service
    // from before this answers `unsupported` rather than pairing it unlimited.
    if (auto refused = needs_unlock(c)) return *refused;
    const std::string code = text("code");
    const auto ttl = doc->integer("ttl_ms");
    const auto for_s = doc->integer("for_s");
    if (!lower_hex(code, kPairCodeChars) || !ttl || *ttl <= 0 || *ttl > kMaxPairTtlMs || !for_s || *for_s <= 0 ||
        *for_s > grants::kMaxSeconds) {
      return {end_code::bad_request, {}};
    }
    grants::limit gives;
    gives.for_s = *for_s;
    if (const json::value* r = doc->find("roots")) {
      if (r->k != json::kind::array || r->a.size() > grants::kMaxRootIds) return {end_code::bad_request, {}};
      std::vector<std::string> ids;
      for (const auto& e : r->a) {
        if (e.k != json::kind::string || !lower_hex(e.s, 16)) return {end_code::bad_request, {}};
        ids.push_back(e.s);
      }
      gives.roots = std::move(ids);
    }
    std::lock_guard lock(codes_m);
    codes.clear();  // one code at a time, as pair_code
    codes.push_back({code_hash(code), clock::now() + std::chrono::milliseconds(*ttl), std::move(gives)});
    return {end_code::ok, "{}"};
  }
  if (*what == "pair_code") {
    const std::string code = text("code");
    const auto ttl = doc->integer("ttl_ms");
    if (!lower_hex(code, kPairCodeChars) || !ttl || *ttl <= 0 || *ttl > kMaxPairTtlMs) return {end_code::bad_request, {}};
    // One code at a time: a new one withdraws the last, so a photo of an
    // older QR is worth nothing once the card has moved on.
    std::lock_guard lock(codes_m);
    codes.clear();
    codes.push_back({code_hash(code), clock::now() + std::chrono::milliseconds(*ttl), {}});
    return {end_code::ok, "{}"};
  }
  if (*what == "pair_cancel") {
    std::lock_guard lock(codes_m);
    codes.clear();
    return {end_code::ok, "{}"};
  }
  if (*what == "roots") {
    // To the owner alone: the NAS's own paths, which no phone is ever told.
    json::writer w;
    w.begin_object().key("roots").begin_array();
    for (const auto& r : cfg()->roots) {
      w.begin_object().key("id").string(r.id).key("label").string(r.label).key("path").string(r.path).end_object();
    }
    w.end_array().end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "add_root") {
    std::string label = text("label");
    const std::string path = text("path");
    const bool absolute = !path.empty() && (path.front() == '/' || (path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')));
    if (!absolute || label.empty() || label.size() > kMaxLabelBytes) return {end_code::bad_request, {}};
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), ec)) {
      return {end_code::not_found, {}};
    }
    std::lock_guard lock(claim_m);  // one change of the folders at a time
    config next = *cfg();
    if (next.roots.size() >= kMaxRoots) return {end_code::too_many, {}};
    const std::string id = new_root_id();
    next.roots.push_back({id, std::move(label), path});
    if (!publish_config(std::move(next))) return {end_code::error, {}};
    json::writer w;
    w.begin_object().key("id").string(id).end_object();
    return {end_code::ok, w.take()};
  }
  if (*what == "remove_root") {
    const std::string id = text("id");
    {
      std::lock_guard lock(claim_m);
      config next = *cfg();
      const auto before = next.roots.size();
      std::erase_if(next.roots, [&](const root_config& r) { return r.id == id; });
      if (next.roots.size() == before) return {end_code::not_found, {}};
      if (next.roots.empty()) return {end_code::refused, {}};  // a service must serve something
      if (!publish_config(std::move(next))) return {end_code::error, {}};
    }
    cut_sessions(&c);  // a read of that folder stops now, and the phone comes back without it
    return {end_code::ok, "{}"};
  }
  return {end_code::unsupported, {}};
}

void server::state::cut_sessions(const conn* except) {
  std::vector<std::shared_ptr<conn>> live;
  {
    std::lock_guard all(conns_m);
    for (auto& c : conns) {
      if (c.get() == except) continue;
      std::lock_guard lock(c->m);
      if (c->has_device && c->ch) {
        c->has_device = false;
        live.push_back(c);
      }
    }
  }
  for (auto& c : live) (void)c->ch->cancel(kCancelWaitMs, "folders-changed");
}

server::state::lists server::state::read_lists() {
  // Both read under the lock every change in this process takes, so a move
  // between them is never seen half done from here.
  std::lock_guard lock(devices_m);
  lists out;
  out.devices = known::load(*store);
  out.grants_ok = grants::load(*store, out.limited);
  return out;
}

const char* server::state::verdict(const lists& l, const conn& c, std::int64_t t) const {
  const bool unlimited = std::any_of(l.devices.begin(), l.devices.end(), [&](const known::device& d) { return d.key == c.device; });
  const grants::grant* g = nullptr;
  if (l.grants_ok) {
    for (const auto& x : l.limited) {
      if (x.key == c.device) g = &x;
    }
  }
  if (!l.grants_ok && !unlimited) return "revoked";  // a grants list that cannot be read keeps nobody on it
  if (!g && unlimited) {
    // Made unlimited while connected: it reconnects so its caps lose the limit.
    return c.grant ? "folders-changed" : nullptr;
  }
  if (!g) return "revoked";
  if (g->expired(t)) return "expired";
  // Its limit changed (an extension, other folders, or a limit on an unlimited
  // phone): it reconnects and is told the one it has now. "folders-changed" is
  // what every app already answers by reconnecting.
  if (!c.grant || g->roots != c.grant->roots || g->expires != c.grant->expires) return "folders-changed";
  return nullptr;
}

void server::state::cut_unpaired() {
  auto l = read_lists();
  const auto t = now();
  // The cuts wait on sockets, so they happen with no lock held; each conn is
  // kept alive by its reference here, and its channel is set only once.
  std::vector<std::pair<std::shared_ptr<conn>, const char*>> cut;
  const auto decide = [&](const lists& with) {
    cut.clear();
    std::lock_guard all(conns_m);
    for (auto& c : conns) {
      std::lock_guard lock(c->m);
      if (!c->has_device || !c->ch || c->is_owner) continue;
      if (const char* why = verdict(with, *c, t)) cut.emplace_back(c, why);
    }
  };
  decide(l);
  // `driftwire serve` on the command line writes the two lists from another
  // process: a revoke seen there is read again before it cuts anyone.
  if (std::any_of(cut.begin(), cut.end(), [](const auto& x) { return std::strcmp(x.second, "revoked") == 0; })) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    l = read_lists();
    decide(l);
  }
  {
    std::lock_guard all(conns_m);
    std::erase_if(cut, [](auto& x) {
      std::lock_guard lock(x.first->m);
      if (!x.first->has_device) return true;  // gone meanwhile
      x.first->has_device = false;
      return false;
    });
  }
  for (auto& [c, why] : cut) {
    if (std::strcmp(why, "expired") == 0) n_expired.fetch_add(1);
    (void)c->ch->cancel(kCancelWaitMs, why);
  }
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
  st.cfg_now = std::make_shared<const config>(o.cfg);
  st.service_name = o.cfg.name;
  st.claimable = o.claimable;
  st.lan_only = o.lan_only;
  st.own_network = o.own_network;
  st.save_config = o.save_config;
  st.devices_lock = o.devices_lock;
  st.now_unix = o.now_unix;
  st.now_steady = o.now_steady;
  st.min_answer_gap = std::chrono::milliseconds(std::max(0, o.min_answer_gap_ms));
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
  crypto_scalarmult_base(st.identity_public.data(), st.identity.data());
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

std::vector<root_config> server::roots() const { return st_->cfg()->roots; }

counters server::stats() const noexcept {
  counters c;
  c.accepted = st_->n_accepted.load();
  c.over_limit = st_->n_over.load();
  c.bad_hello = st_->n_hello.load();
  c.no_key = st_->n_key.load();
  c.replayed = st_->n_replay.load();
  c.not_paired = st_->n_unpaired.load();
  c.paired = st_->n_paired.load();
  c.bad_codes = st_->n_bad_codes.load();
  c.sessions = st_->n_sessions.load();
  c.evicted = st_->n_evicted.load();
  c.wrong_passphrase = st_->n_wrong_pass.load();
  c.unlocks = st_->n_unlocks.load();
  c.expired = st_->n_expired.load();
  return c;
}

std::array<std::uint8_t, 32> claim_key(std::string_view token_hex) {
  static const unsigned char label[] = "mediaviewer.driftwire.claim.v1";
  std::array<std::uint8_t, 32> k{};
  crypto_generichash(k.data(), k.size(), reinterpret_cast<const unsigned char*>(token_hex.data()), token_hex.size(), label, sizeof label - 1);
  return k;
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

namespace {

constexpr std::size_t kPairCodeRow = 8 + 32;  // expiry, code_hash

std::int64_t row_expiry(const std::uint8_t* row) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | row[i];
  return static_cast<std::int64_t>(v);
}

// The stored rows still in date; an unreadable value is an empty list only
// when it is absent (a value that is there and bad is refused by the caller).
std::vector<std::uint8_t> live_rows(secrets::store& store, std::int64_t now_unix, bool* bad = nullptr) {
  std::vector<std::uint8_t> kept;
  if (bad) *bad = false;
  if (!store.exists(kPairCodesName)) return kept;
  auto got = store.get(kPairCodesName);
  if (!got || got->size() % kPairCodeRow != 0) {
    if (bad) *bad = true;
    return kept;
  }
  for (std::size_t at = 0; at < got->size(); at += kPairCodeRow) {
    if (row_expiry(got->data() + at) > now_unix) kept.insert(kept.end(), got->begin() + static_cast<std::ptrdiff_t>(at), got->begin() + static_cast<std::ptrdiff_t>(at + kPairCodeRow));
  }
  sodium_memzero(got->data(), got->size());
  return kept;
}

}  // namespace

std::string issue_pair_code(secrets::store& store, std::int64_t seconds, std::int64_t now_unix) {
  if (sodium_init() < 0 || seconds <= 0) return {};
  bool bad = false;
  auto rows = live_rows(store, now_unix, &bad);
  if (bad) return {};
  std::array<std::uint8_t, 16> raw{};
  randombytes_buf(raw.data(), raw.size());
  char hex[33];
  sodium_bin2hex(hex, sizeof hex, raw.data(), raw.size());
  std::string code(hex);
  sodium_memzero(raw.data(), raw.size());
  const auto h = code_hash(code);
  const auto expiry = static_cast<std::uint64_t>(now_unix + seconds);
  for (int i = 0; i < 8; ++i) rows.push_back(static_cast<std::uint8_t>(expiry >> (8 * i)));
  rows.insert(rows.end(), h.begin(), h.end());
  if (rows.size() > kPairCodesKept * kPairCodeRow) rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(kPairCodesKept * kPairCodeRow));
  if (!store.put(kPairCodesName, rows)) return {};
  return code;
}

bool redeem_pair_code(secrets::store& store, std::string_view code, std::int64_t now_unix) {
  if (code.empty() || code.size() > kMaxPairCodeBytes || sodium_init() < 0) return false;
  bool bad = false;
  auto rows = live_rows(store, now_unix, &bad);
  if (bad) return false;
  const auto h = code_hash(code);
  for (std::size_t at = 0; at < rows.size(); at += kPairCodeRow) {
    if (sodium_memcmp(rows.data() + at + 8, h.data(), h.size()) == 0) {
      rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(at), rows.begin() + static_cast<std::ptrdiff_t>(at + kPairCodeRow));
      return store.put(kPairCodesName, rows);
    }
  }
  return false;
}

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
