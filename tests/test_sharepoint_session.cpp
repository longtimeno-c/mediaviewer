// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint protocol 2's session (docs/design/28 "The session", issue #472,
// PR 1): one transfer that survives its link. As far as one machine can run
// the issue's verify: a link that drops is rejoined on another and every
// message arrives once, in order, with nothing whole sent again; a faster
// link is opened alongside and the switch loses nothing; a rejoin without the
// session's secret, or a replayed one, is refused while the session carries
// on; three rejoins in 10 s and 20 s without a link each end it; protocol 1
// peers still meet protocol 2 ones. And two engines: a set over Bluetooth's
// old 32 MB limit starts on "Bluetooth" (a slowed loopback link that says it
// is one) and moves to Wi-Fi when the peer's beacon appears; a Wi-Fi link
// that drops moves to Bluetooth, which goes out of range and comes back.
// Radios are stand-ins: no real Bluetooth or Wi-Fi is measured here.
#include "sharepoint_fixture.h"

namespace {

using clock_type = std::chrono::steady_clock;

// A link that says which bearer it is, can be slowed to a byte rate (a
// Bluetooth stand-in), and whose socket the test can cut.
// Frozen, it goes silent the way a radio link does: what is written is lost,
// nothing more arrives, and nothing says it closed.
class bearer_link final : public sp::link {
 public:
  bearer_link(std::shared_ptr<sp::link> inner, sp::transport via, const std::atomic<std::size_t>* rate = nullptr,
              std::shared_ptr<std::atomic<bool>> frozen = nullptr)
      : inner_(std::move(inner)), via_(via), rate_(rate), frozen_(std::move(frozen)) {}
  ~bearer_link() override { inner_->shutdown(); }
  mv::expected write_all(std::span<const std::uint8_t> b) override {
    if (frozen_ && frozen_->load()) return {};
    const std::size_t r = rate_ ? rate_->load() : 0;
    if (r == 0) return inner_->write_all(b);
    // Paced against a schedule, not by a sleep per piece: a sleep that runs
    // long (Windows sleeps in ~15.6 ms ticks) is made up, not added.
    constexpr std::size_t kPiece = 16 * 1024;
    std::lock_guard lock(pace_m_);
    if (paced_ == 0) pace_start_ = clock_type::now();
    for (std::size_t at = 0; at < b.size(); at += kPiece) {
      const std::size_t n = std::min(kPiece, b.size() - at);
      if (auto x = inner_->write_all(b.subspan(at, n)); !x) return x;
      paced_ += n;
      const auto due = pace_start_ + std::chrono::microseconds(static_cast<long long>(paced_ / r * 1000000 +
                                                                                       paced_ % r * 1000000 / r));
      if (clock_type::now() < due) std::this_thread::sleep_until(due);
    }
    return {};
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    if (frozen_ && frozen_->load()) {
      // Silent until shut down (a rejoin closes it) or the wait is over.
      const auto until = clock_type::now() + std::chrono::milliseconds(timeout_ms < 0 ? 60000 : timeout_ms);
      while (clock_type::now() < until) {
        if (down_.load()) return mv::err(mv::status::cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return mv::err(mv::status::timeout);
    }
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override {
    down_.store(true);
    inner_->shutdown();
  }
  sp::transport via() const noexcept override { return via_; }

 private:
  std::shared_ptr<sp::link> inner_;
  sp::transport via_;
  const std::atomic<std::size_t>* rate_;
  std::shared_ptr<std::atomic<bool>> frozen_;
  std::atomic<bool> down_{false};
  std::mutex pace_m_;
  std::uint64_t paced_ = 0;  // under pace_m_: bytes written since the first
  clock_type::time_point pace_start_{};
};

// A loopback pair without REQUIRE (an engine's thread makes these).
std::optional<std::pair<std::shared_ptr<sp::link>, std::shared_ptr<sp::link>>> try_loopback_pair() {
  auto l = sp::net::listen(true);
  if (!l) return std::nullopt;
  std::unique_ptr<sp::link> server;
  std::thread t([&] {
    auto a = (*l)->accept(5000);
    if (a) server = std::move(*a);
  });
  auto c = sp::net::connect({"127.0.0.1", (*l)->port()}, 5000);
  t.join();
  if (!c || !server) return std::nullopt;
  return std::make_pair(std::shared_ptr<sp::link>(std::move(*c)), std::shared_ptr<sp::link>(std::move(server)));
}

// The sockets under every link a world made, to cut them.
struct cuttable {
  std::mutex m;
  std::vector<std::shared_ptr<sp::link>> sockets;
  void keep(std::shared_ptr<sp::link> s) {
    std::lock_guard lock(m);
    sockets.push_back(std::move(s));
  }
  void cut_all() {
    std::lock_guard lock(m);
    for (auto& s : sockets) s->shutdown();
    sockets.clear();
  }
};

// Bluetooth for engines in one process: each hears the others (while
// `audible`), and a connect is a loopback pair, slowed to `rate`, handed to
// the other engine as its incoming link.
struct bt_world {
  std::mutex m;
  struct member {
    std::string id, name;
    sp::bluetooth::link_fn on_link;
  };
  std::map<std::string, member> members;
  std::atomic<bool> audible{true};
  std::atomic<bool> reachable{true};
  std::atomic<std::size_t> rate{0};
  std::atomic<int> connects{0};
  std::atomic<long long> last_connect_ms{-1};  // how long the last connect took (the timeline)
  cuttable links;
};

class world_bluetooth final : public sp::bluetooth {
 public:
  explicit world_bluetooth(bt_world& w) : w_(w) {}
  ~world_bluetooth() override { stop(); }
  bool start(const std::string& id, const std::string& name, peer_fn on_peer, link_fn on_link, std::string&) override {
    id_ = id;
    {
      std::lock_guard lock(w_.m);
      w_.members[id] = {id, name, std::move(on_link)};
    }
    stop_.store(false);
    thread_ = std::thread([this, on_peer = std::move(on_peer)] {
      while (!stop_.load()) {
        std::vector<sp::bt_peer> heard;
        if (w_.audible.load()) {
          std::lock_guard lock(w_.m);
          for (const auto& [k, mem] : w_.members) {
            if (k != id_) heard.push_back({k, k, mem.name, sp::kProtocolVersion});
          }
        }
        for (const auto& p : heard) on_peer(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
    return true;
  }
  void stop() noexcept override {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(w_.m);
    w_.members.erase(id_);
  }
  mv::result<std::unique_ptr<sp::link>> connect(const std::string& handle, int) override {
    ++w_.connects;
    if (!w_.reachable.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));  // out of range: nothing answers
      return mv::err(mv::status::timeout);
    }
    sp::bluetooth::link_fn deliver;
    {
      std::lock_guard lock(w_.m);
      const auto it = w_.members.find(handle);
      if (it == w_.members.end()) return mv::err(mv::status::not_found);
      deliver = it->second.on_link;
    }
    const auto began = clock_type::now();
    auto pair = try_loopback_pair();
    if (!pair) return mv::err(mv::status::io);
    w_.links.keep(pair->first);
    deliver(std::make_unique<bearer_link>(pair->second, sp::transport::bluetooth, &w_.rate));
    w_.last_connect_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - began).count());
    return std::unique_ptr<sp::link>(std::make_unique<bearer_link>(pair->first, sp::transport::bluetooth, &w_.rate));
  }

 private:
  bt_world& w_;
  std::string id_;
  std::atomic<bool> stop_{true};
  std::thread thread_;
};

// Wi-Fi connects an engine makes, slowed and cuttable; `refuse` makes them fail
// (the peer's network is gone).
struct wifi_world {
  std::atomic<std::size_t> rate{0};
  std::atomic<bool> refuse{false};
  std::atomic<int> connects{0};
  cuttable links;
  std::mutex m;
  std::vector<std::shared_ptr<std::atomic<bool>>> frozen;  // one per link made
  // Every link made so far goes silent; later ones work.
  void freeze_all() {
    std::lock_guard lock(m);
    for (auto& f : frozen) f->store(true);
  }
  std::function<mv::result<std::unique_ptr<sp::link>>(const sp::net::endpoint&, int)> connector() {
    return [this](const sp::net::endpoint& to, int ms) -> mv::result<std::unique_ptr<sp::link>> {
      ++connects;
      if (refuse.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return mv::err(mv::status::io);
      }
      auto c = sp::net::connect(to, ms);
      if (!c) return mv::err(c.error());
      std::shared_ptr<sp::link> inner(std::move(*c));
      links.keep(inner);
      auto flag = std::make_shared<std::atomic<bool>>(false);
      {
        std::lock_guard lock(m);
        frozen.push_back(flag);
      }
      return std::unique_ptr<sp::link>(std::make_unique<bearer_link>(inner, sp::transport::wifi, &rate, flag));
    };
  }
};

// A receiver's side of a rejoin, on its own thread: the hello, then the live
// channel it names.
mv::expected accept_on(sp::channel& server, std::shared_ptr<sp::link> l) {
  auto h = sp::channel::read_hello(*l, clock_type::now() + std::chrono::seconds(5));
  if (!h) return mv::err(h.error());
  if (!server.names_this(*h)) {
    l->shutdown();
    return mv::err(mv::status::not_found);
  }
  return server.accept_rejoin(std::move(l), *h);
}

// A rejoin of `client` to `server` over a fresh loopback pair.
mv::expected rejoin_pair(sp::channel& client, sp::channel& server, std::shared_ptr<sp::link>* client_socket = nullptr) {
  auto pair = try_loopback_pair();
  REQUIRE(pair);
  if (client_socket) *client_socket = pair->first;
  mv::expected accepted = mv::err(mv::status::internal);
  std::thread t([&] { accepted = accept_on(server, pair->second); });
  auto r = client.rejoin(pair->first);
  t.join();
  if (!r) return r;
  return accepted;
}

// A channel pair whose first link the test can cut.
struct session_pair {
  std::unique_ptr<sp::channel> a, b;  // a: the client (sender)
  std::shared_ptr<sp::link> socket;   // a's end of the first link
};
session_pair open_session(std::uint32_t version = sp::kProtocolVersion) {
  auto pair = try_loopback_pair();
  REQUIRE(pair);
  session_pair s;
  s.socket = pair->first;
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&] { server = sp::channel::open(pair->second, false, "bbbbbbbbbbbbbbbb", "Bea"); });
  sp::channel::options o;
  o.version = version;
  auto client = sp::channel::open(pair->first, true, "aaaaaaaaaaaaaaaa", "Ann", o);
  t.join();
  REQUIRE(client);
  REQUIRE(server);
  s.a = std::move(*client);
  s.b = std::move(*server);
  return s;
}

std::vector<std::uint8_t> numbered(std::uint32_t i, std::size_t size = 64 * 1024) { return pattern(size, i + 1); }

// Sends `count` numbered messages from `from` on a thread; the caller reads.
struct producer {
  std::atomic<std::uint32_t> sent{0};
  std::atomic<bool> failed{false};
  std::thread t;
  producer(sp::channel& from, std::uint32_t count, std::size_t size = 64 * 1024) {
    t = std::thread([&, count, size] {
      for (std::uint32_t i = 0; i < count; ++i) {
        if (!from.send(sp::msg::data, numbered(i, size))) {
          failed.store(true);
          return;
        }
        sent.store(i + 1);
      }
    });
  }
  ~producer() {
    if (t.joinable()) t.join();
  }
};

}  // namespace

// ---- the session, two channels ------------------------------------------------------------------

TEST_CASE("sharepoint session: a dropped link rejoins on another and every message arrives once, in order",
          "[sharepoint]") {
  session_pair s = open_session();
  REQUIRE(s.a->resumable());
  constexpr std::uint32_t kCount = 160;
  producer out(*s.a, kCount);
  sp::message m;
  std::uint32_t got = 0;
  bool cut = false;
  std::thread rejoiner;
  while (got < kCount) {
    if (!cut && got == 40) {
      // The first link fails under the transfer (Wi-Fi off, out of range).
      s.socket->shutdown();
      cut = true;
      rejoiner = std::thread([&] { CHECK(rejoin_pair(*s.a, *s.b)); });
    }
    REQUIRE(s.b->receive(m, 15000));
    REQUIRE(m.type == sp::msg::data);
    INFO("message " << got);
    REQUIRE(m.body == numbered(got));
    ++got;
  }
  rejoiner.join();
  CHECK_FALSE(out.failed.load());
  CHECK(s.a->rejoins() == 1);
  CHECK(s.b->rejoins() == 1);
  // And the other way, on the new link.
  REQUIRE(s.b->send(sp::msg::file_done, std::string_view("{\"i\":0,\"ok\":true}")));
  REQUIRE(s.a->receive(m, 5000));
  CHECK(m.type == sp::msg::file_done);
}

TEST_CASE("sharepoint session: a faster link is opened alongside and the switch loses and repeats nothing",
          "[sharepoint]") {
  session_pair s = open_session();
  constexpr std::uint32_t kCount = 160;
  producer out(*s.a, kCount);
  sp::message m;
  std::thread rejoiner;
  for (std::uint32_t got = 0; got < kCount; ++got) {
    if (got == 30) rejoiner = std::thread([&] { CHECK(rejoin_pair(*s.a, *s.b)); });  // make before break
    REQUIRE(s.b->receive(m, 15000));
    INFO("message " << got);
    REQUIRE(m.body == numbered(got));
  }
  rejoiner.join();
  CHECK(s.a->rejoins() == 1);
  CHECK(s.a->link_up());
}

TEST_CASE("sharepoint session: a rejoin without the session's secret, or a replayed one, is refused",
          "[sharepoint]") {
  session_pair s = open_session();
  session_pair other = open_session();
  // Another session's client knocks: its rejoin names nothing here.
  {
    auto pair = try_loopback_pair();
    REQUIRE(pair);
    mv::expected accepted = mv::err(mv::status::internal);
    std::thread t([&] { accepted = accept_on(*s.b, pair->second); });
    CHECK_FALSE(other.a->rejoin(pair->first));
    t.join();
    REQUIRE_FALSE(accepted);
    CHECK(accepted.error() == mv::status::not_found);
  }
  // A recording of a real rejoin, played to the receiver again later: its tag
  // is right and its first message opens, but it has no keys, so the session
  // stays where it is.
  class recorder final : public sp::link {
   public:
    explicit recorder(std::shared_ptr<sp::link> in) : in_(std::move(in)) {}
    mv::expected write_all(std::span<const std::uint8_t> b) override {
      {
        std::lock_guard lock(m);
        bytes.insert(bytes.end(), b.begin(), b.end());
      }
      return in_->write_all(b);
    }
    mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override { return in_->read_some(into, ms); }
    void shutdown() noexcept override { in_->shutdown(); }
    sp::transport via() const noexcept override { return in_->via(); }
    std::mutex m;
    std::vector<std::uint8_t> bytes;

   private:
    std::shared_ptr<sp::link> in_;
  };
  std::shared_ptr<recorder> rec;
  {
    auto pair = try_loopback_pair();
    REQUIRE(pair);
    rec = std::make_shared<recorder>(pair->first);
    mv::expected accepted = mv::err(mv::status::internal);
    std::thread t([&] { accepted = accept_on(*s.b, pair->second); });
    REQUIRE(s.a->rejoin(rec));
    t.join();
    REQUIRE(accepted);
  }
  std::vector<std::uint8_t> replay;
  {
    std::lock_guard lock(rec->m);
    replay = rec->bytes;
  }
  {
    auto pair = try_loopback_pair();
    REQUIRE(pair);
    mv::expected accepted = mv::err(mv::status::internal);
    std::thread t([&] { accepted = accept_on(*s.b, pair->second); });
    REQUIRE(pair->first->write_all(replay));
    t.join();
    REQUIRE_FALSE(accepted);
    CHECK(accepted.error() == mv::status::corrupt);
  }
  CHECK(s.b->rejoins() == 1);
  REQUIRE(s.a->send(sp::msg::offer, std::string_view("{\"still\":true}")));
  sp::message m;
  REQUIRE(s.b->receive(m, 5000));
  CHECK(m.text() == "{\"still\":true}");
}

TEST_CASE("sharepoint session: three rejoins in 10 s end the session", "[sharepoint]") {
  session_pair s = open_session();
  CHECK(rejoin_pair(*s.a, *s.b));
  CHECK(rejoin_pair(*s.a, *s.b));
  const auto third = rejoin_pair(*s.a, *s.b);
  REQUIRE_FALSE(third);
  CHECK(s.a->closed());
  CHECK(eventually([&] { return s.b->closed(); }, 25));
}

TEST_CASE("sharepoint session: 20 s without a link ends the session on both sides", "[sharepoint]") {
  session_pair s = open_session();
  s.socket->shutdown();
  const auto t0 = clock_type::now();
  sp::message m;
  const auto r = s.b->receive(m);  // no deadline of its own: the session's
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
  REQUIRE_FALSE(r);
  CHECK(waited >= sp::kRejoinMs - 1500);
  CHECK(waited <= sp::kRejoinMs + 3000);
  CHECK(s.b->closed());
  CHECK(eventually([&] { return s.a->closed(); }, 5));
}

TEST_CASE("sharepoint session: a protocol 1 client and a protocol 2 server speak protocol 1", "[sharepoint]") {
  session_pair s = open_session(sp::kProtocol1);
  CHECK(s.a->version() == sp::kProtocol1);
  CHECK(s.b->version() == sp::kProtocol1);
  CHECK(s.a->code() == s.b->code());
  CHECK_FALSE(s.a->resumable());
  REQUIRE(s.a->send(sp::msg::offer, std::string_view("{}")));
  sp::message m;
  REQUIRE(s.b->receive(m, 5000));
  CHECK(m.type == sp::msg::offer);
  // No rejoin on protocol 1: the link is the channel.
  auto pair = try_loopback_pair();
  REQUIRE(pair);
  CHECK(s.a->rejoin(pair->first).error() == mv::status::unsupported_format);
}

TEST_CASE("sharepoint session: the beacon and Bluetooth INFO say protocol 2 where protocol 1 still reads them",
          "[sharepoint]") {
  sp::beacon b{"0123456789abcdef", "Ann", 4000, sp::kProtocolVersion};
  const std::string wire = sp::encode_beacon(b);
  CHECK(wire.find("\"v\":1") != std::string::npos);  // what a protocol 1 client checks
  CHECK(wire.find("\"p\":2") != std::string::npos);
  auto back = sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()});
  REQUIRE(back);
  CHECK(back->protocol == 2u);
  // A beacon from before #472 has no "p": protocol 1.
  const std::string old = "MVSP1{\"v\":1,\"id\":\"0123456789abcdef\",\"name\":\"Bea\",\"port\":4000}";
  auto older = sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(old.data()), old.size()});
  REQUIRE(older);
  CHECK(older->protocol == 1u);
  // A newer one is spoken to in ours.
  CHECK(sp::protocol_of(9) == sp::kProtocolVersion);
}

// ---- two engines, two bearers --------------------------------------------------------------------

namespace {

struct node {
  fake_host host;
  sp::engine eng;
  node(const fs::path& received, sp::engine::options o) : host(received), eng(host, std::move(o)) {
    eng.screen_opened("");
  }
};

sp::engine::options node_options(const std::string& name, bt_world& bt) {
  sp::engine::options o;
  o.my_name = name;
  o.loopback_only = true;
  o.make_bluetooth = [&bt] { return std::unique_ptr<sp::bluetooth>(std::make_unique<world_bluetooth>(bt)); };
  return o;
}

// Bea's beacon, repeated into Ann's discovery while `on` (Bea on Ann's network).
struct beacon_pump {
  std::atomic<bool> on{false};
  std::atomic<bool> stop{false};
  std::thread t;
  beacon_pump(std::atomic<inject_discovery*>& into, sp::beacon b) {
    t = std::thread([&into, b, this] {
      while (!stop.load()) {
        // `ready` publishes `seen`, which the engine's control thread set.
        inject_discovery* d = into.load();
        if (on.load() && d && d->ready.load()) d->seen(b, "127.0.0.1");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    });
  }
  ~beacon_pump() {
    stop.store(true);
    t.join();
  }
};

std::uint16_t port_on(bus& b) {
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) port = m.self.port;
    return port != 0;
  }));
  return port;
}

fs::path big_file(const fs::path& dir, std::uint64_t bytes) {
  fs::create_directories(dir);
  const fs::path p = dir / "clip.mov";
  std::ofstream f(p, std::ios::binary);
  const auto chunk = pattern(1024 * 1024, 42);
  for (std::uint64_t at = 0; at < bytes; at += chunk.size()) {
    f.write(reinterpret_cast<const char*>(chunk.data()),
            static_cast<std::streamsize>(std::min<std::uint64_t>(chunk.size(), bytes - at)));
  }
  return p;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  std::vector<char> ba(1 << 20), bb(1 << 20);
  for (;;) {
    fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
    fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
    if (fa.gcount() != fb.gcount()) return false;
    if (fa.gcount() == 0) return true;
    if (std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(fa.gcount())) != 0) return false;
  }
}

bool build_tree_has(const sp::view& v, const std::string& text) { return sp::build_tree(v).find(text) != std::string::npos; }

// Waits for Bea to have `bytes`; on a timeout says where both ends stood.
bool bytes_arrive(node& ann, node& bea, std::uint64_t bytes) {
  if (eventually([&] { return bea.eng.snapshot().in_bytes >= bytes; }, 40)) return true;
  const auto a = ann.eng.snapshot();
  const auto b = bea.eng.snapshot();
  std::fprintf(stderr, "Ann: out %d via %s reconnecting %d sent %llu; Bea: in %d via %s got %llu\n",
               static_cast<int>(a.out), a.out_via.c_str(), a.out_reconnecting ? 1 : 0,
               static_cast<unsigned long long>(a.out_bytes), static_cast<int>(b.in), b.in_via.c_str(),
               static_cast<unsigned long long>(b.in_bytes));
  return false;
}

// Types the code on Ann that Bea shows, and accepts on Bea.
void confirm_and_accept(node& ann, node& bea) {
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::confirming; }));
  std::string code;
  REQUIRE(eventually([&] {
    code = bea.eng.snapshot().in_code;
    return !code.empty();
  }));
  ann.eng.on_event("out_code_entry", 3, "\"" + code + "\"");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::sending; }));
}

// Bea's received bytes over time: the ring may pause, never go back.
struct progress_watch {
  std::atomic<bool> stop{false};
  std::atomic<bool> went_back{false};
  std::atomic<bool> saw_reconnecting{false};
  std::thread t;
  progress_watch(node& ann, node& bea) {
    t = std::thread([&, this] {
      std::uint64_t last = 0;
      while (!stop.load()) {
        const auto v = bea.eng.snapshot();
        if (v.in_bytes < last) went_back.store(true);
        last = v.in_bytes;
        if (v.in_reconnecting || ann.eng.snapshot().out_reconnecting) saw_reconnecting.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
  ~progress_watch() {
    stop.store(true);
    t.join();
  }
};

}  // namespace

TEST_CASE("sharepoint session: a big set starts on Bluetooth and moves to Wi-Fi when the peer is on it",
          "[sharepoint]") {
  scratch_dir s("sharepoint_handover_bt_wifi");
  bt_world bt;
  bt.rate = 2 * 1024 * 1024;  // "Bluetooth": slow enough that the move is what finishes it
  bus b;
  wifi_world wifi;
  // Bea: on her network (the bus) and in Bluetooth range.
  sp::engine::options bo = node_options("Bea", bt);
  bo.make_discovery = [&b] { return std::make_unique<bus_discovery>(b); };
  node bea(s / "bea_received", bo);
  const std::uint16_t port = port_on(b);
  // Ann: hears Bea only on Bluetooth until her beacon is let through.
  std::atomic<inject_discovery*> ann_wifi{nullptr};
  sp::engine::options ao = node_options("Ann", bt);
  ao.make_discovery = [&ann_wifi] {
    auto d = std::make_unique<inject_discovery>();
    ann_wifi.store(d.get());
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  ao.connect_wifi = wifi.connector();
  node ann(s / "ann_received", ao);
  beacon_pump pump(ann_wifi, sp::beacon{bea.eng.id(), "Bea", port, sp::kProtocolVersion});

  // 40 MiB: over protocol 1's 32 MB Bluetooth limit.
  const fs::path src = big_file(s / "out", 40ull * 1024 * 1024);
  ann.eng.set_files({utf8(src)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty() && v.peers[0].via == "Bluetooth";
  }));
  CHECK(ann.eng.snapshot().peers[0].resumable);
  ann.eng.on_event("peers", 4, "[\"" + bea.eng.id() + "\"]");
  CHECK_FALSE(sp::needs_wifi(ann.eng.snapshot()));  // no waiting for Wi-Fi first
  ann.eng.on_event("peers", 5, "\"" + bea.eng.id() + "\"");
  confirm_and_accept(ann, bea);
  CHECK(ann.eng.snapshot().out_via == "Bluetooth");
  CHECK(ann.eng.snapshot().out_protocol == 2u);
  progress_watch watch(ann, bea);
  REQUIRE(bytes_arrive(ann, bea, 2 * 1024 * 1024));

  // Bea joins Ann's network: within 3 s the send is on Wi-Fi.
  pump.on.store(true);
  const auto t0 = clock_type::now();
  REQUIRE(eventually([&] { return ann.eng.snapshot().out_via == "Wi-Fi"; }, 10));
  const auto moved_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
  CHECK(moved_ms <= 3000);
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  CHECK(bea.eng.snapshot().in_via == "Wi-Fi");
  CHECK_FALSE(watch.went_back.load());
  CHECK(same_bytes(src, s / "bea_received" / "clip.mov"));
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"clip.mov"});  // once
  CHECK_FALSE(any_temporary(s / "bea_received"));                              // nothing partial
  std::printf("{\"case\":\"bluetooth_to_wifi\",\"moved_ms\":%lld}\n", static_cast<long long>(moved_ms));
}

TEST_CASE("sharepoint session: Wi-Fi drops to Bluetooth, which goes out of range and comes back",
          "[sharepoint]") {
  scratch_dir s("sharepoint_handover_wifi_bt");
  bt_world bt;
  bus b;
  wifi_world wifi;
  wifi.rate = 6 * 1024 * 1024;  // slow enough to cut part-way
  bt.rate = 0;
  sp::engine::options bo = node_options("Bea", bt);
  bo.make_discovery = [&b] { return std::make_unique<bus_discovery>(b); };
  node bea(s / "bea_received", bo);
  const std::uint16_t port = port_on(b);
  std::atomic<inject_discovery*> ann_wifi{nullptr};
  sp::engine::options ao = node_options("Ann", bt);
  ao.make_discovery = [&ann_wifi] {
    auto d = std::make_unique<inject_discovery>();
    ann_wifi.store(d.get());
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  ao.connect_wifi = wifi.connector();
  node ann(s / "ann_received", ao);
  beacon_pump pump(ann_wifi, sp::beacon{bea.eng.id(), "Bea", port, sp::kProtocolVersion});
  pump.on.store(true);

  const fs::path src = big_file(s / "out", 48ull * 1024 * 1024);
  ann.eng.set_files({utf8(src)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty() && v.peers[0].wifi;
  }));
  ann.eng.on_event("peers", 5, "\"" + bea.eng.id() + "\"");
  confirm_and_accept(ann, bea);
  CHECK(ann.eng.snapshot().out_via == "Wi-Fi");
  progress_watch watch(ann, bea);
  REQUIRE(bytes_arrive(ann, bea, 4 * 1024 * 1024));

  // Wi-Fi goes: no more beacons, no connects, the link cut. Bluetooth takes it.
  pump.on.store(false);
  wifi.refuse.store(true);
  wifi.links.cut_all();
  auto t0 = clock_type::now();
  const int bt_before = bt.connects.load();
  const int wifi_before = wifi.connects.load();
  // When each side saw the cut, and what the mover tried: printed if slow.
  long long ann_saw = -1, bea_saw = -1, first_bt = -1, bea_on_bt = -1;
  REQUIRE(eventually(
      [&] {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
        const auto a = ann.eng.snapshot();
        if (ann_saw < 0 && a.out_reconnecting) ann_saw = ms;
        if (bea_saw < 0 && bea.eng.snapshot().in_reconnecting) bea_saw = ms;
        if (first_bt < 0 && bt.connects.load() > bt_before) first_bt = ms;
        if (bea_on_bt < 0 && bea.eng.snapshot().in_via == "Bluetooth") bea_on_bt = ms;
        return a.out_via == "Bluetooth";
      },
      15));
  const auto to_bt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
  INFO("Ann saw the cut at " << ann_saw << " ms, Bea at " << bea_saw << " ms; first Bluetooth connect at "
                             << first_bt << " ms, taking " << bt.last_connect_ms.load() << " ms; Bea on Bluetooth at "
                             << bea_on_bt << " ms; " << bt.connects.load() - bt_before << " Bluetooth and "
                             << wifi.connects.load() - wifi_before << " Wi-Fi connects; " << bea.eng.snapshot().in_bytes
                             << " bytes in at the move");
  // A cut that a side sees at once moves in milliseconds; one that shows only
  // as silence takes the suspect timer, then a race (slow CI runners: both).
  CHECK(to_bt_ms <= sp::kSuspectMs + 3000);
  const std::uint64_t at_cut = bea.eng.snapshot().in_bytes;

  // Out of Bluetooth range for 5 s, then back: it resumes.
  bt.reachable.store(false);
  bt.links.cut_all();
  std::this_thread::sleep_for(std::chrono::seconds(5));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::sending);  // waiting, not failed
  CHECK(ann.eng.snapshot().out_reconnecting);
  CHECK(build_tree_has(ann.eng.snapshot(), "Reconnecting to Bea"));
  bt.reachable.store(true);
  t0 = clock_type::now();
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  CHECK(bea.eng.snapshot().in_bytes >= at_cut);
  CHECK(watch.saw_reconnecting.load());
  CHECK_FALSE(watch.went_back.load());
  CHECK(same_bytes(src, s / "bea_received" / "clip.mov"));
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"clip.mov"});
  std::printf("{\"case\":\"wifi_to_bluetooth\",\"to_bluetooth_ms\":%lld}\n", static_cast<long long>(to_bt_ms));
}

TEST_CASE("sharepoint session: a peer that announces protocol 1 is sent to in protocol 1", "[sharepoint]") {
  scratch_dir s("sharepoint_protocol1_peer");
  bus b;
  b.force_protocol.store(sp::kProtocol1);  // both look like peers from before #472
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(300 * 1024, 5));
  ann.pick({f});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(ann.eng.snapshot().out_protocol == 1u);
  CHECK(bea.eng.snapshot().in_protocol == 1u);
  CHECK(fs::file_size(s / "bea_received" / "a.jpg") == 300u * 1024);
}

TEST_CASE("sharepoint session: a link that goes silent is suspect at 3 s and the send moves to a new one",
          "[sharepoint]") {
  scratch_dir s("sharepoint_silent_link");
  bt_world bt;  // nobody uses it: Wi-Fi only
  bus b;
  wifi_world wifi;
  wifi.rate = 8 * 1024 * 1024;
  sp::engine::options bo = node_options("Bea", bt);
  bo.use_bluetooth = false;
  bo.make_discovery = [&b] { return std::make_unique<bus_discovery>(b); };
  node bea(s / "bea_received", bo);
  const std::uint16_t port = port_on(b);
  std::atomic<inject_discovery*> ann_wifi{nullptr};
  sp::engine::options ao = node_options("Ann", bt);
  ao.use_bluetooth = false;
  ao.make_discovery = [&ann_wifi] {
    auto d = std::make_unique<inject_discovery>();
    ann_wifi.store(d.get());
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  ao.connect_wifi = wifi.connector();
  node ann(s / "ann_received", ao);
  beacon_pump pump(ann_wifi, sp::beacon{bea.eng.id(), "Bea", port, sp::kProtocolVersion});
  pump.on.store(true);

  const fs::path src = big_file(s / "out", 40ull * 1024 * 1024);
  ann.eng.set_files({utf8(src)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }));
  ann.eng.on_event("peers", 5, "\"" + bea.eng.id() + "\"");
  confirm_and_accept(ann, bea);
  progress_watch watch(ann, bea);
  REQUIRE(bytes_arrive(ann, bea, 4 * 1024 * 1024));
  const int before = wifi.connects.load();
  wifi.freeze_all();  // the access point stops passing anything; no error anywhere
  const auto t0 = clock_type::now();
  REQUIRE(eventually([&] { return wifi.connects.load() > before; }, 10));
  const auto suspect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
  // The timer counts from the last byte heard from the receiver (its acks
  // and pings), up to about a second before the freeze.
  CHECK(suspect_ms >= sp::kSuspectMs - 1000);
  CHECK(suspect_ms <= sp::kSuspectMs + 1500);
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  // "Reconnecting" lasts only from the suspect to the rejoin here (often under
  // the watcher's 20 ms poll); the out-of-range case checks it held.
  CHECK_FALSE(watch.went_back.load());
  CHECK(same_bytes(src, s / "bea_received" / "clip.mov"));
  CHECK_FALSE(any_temporary(s / "bea_received"));
  std::printf("{\"case\":\"silent_link\",\"suspect_ms\":%lld}\n", static_cast<long long>(suspect_ms));
}
