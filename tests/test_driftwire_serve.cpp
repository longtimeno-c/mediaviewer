// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire serve` over loopback (docs/design/28 "The NAS service"): the
// M2b verify line as far as loopback can show it. A paired client lists,
// range-reads and seeks in a large file; a client without the access key gets
// no reply and costs no ML-KEM work; a revoked device is refused; a path
// outside the named folders is refused. Filter with "[driftwire-serve]";
// nothing here touches the network beyond 127.0.0.1.
#include <sodium.h>

#include "driftwire_fixture.h"

#include "addons/driftwire/known.h"
#include "addons/driftwire/noise.h"
#include "addons/driftwire/serve.h"
#include "addons/driftwire/serve_client.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#endif

namespace serve = mv::driftwire::serve;
namespace noise = mv::driftwire::noise;
namespace known = mv::driftwire::known;
namespace fs = std::filesystem;

namespace {

// How long to wait for the service to catch up: a sanitizer build on a loaded runner is far slower.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MV_SERVE_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MV_SERVE_TEST_SANITIZED 1
#endif
#if defined(MV_SERVE_TEST_SANITIZED)
constexpr int kSlowSeconds = 60;
#else
constexpr int kSlowSeconds = 15;
#endif

const std::string kRootA = "a1a1a1a1a1a1a1a1";
const std::string kRootB = "b2b2b2b2b2b2b2b2";

using key32 = std::array<std::uint8_t, 32>;

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

void write_file(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Bytes that say where they are in the file, so a misplaced range fails.
std::string marker(std::uint64_t at, std::size_t n) {
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<char>(((at + i) * 131) >> 4));
  return out;
}

struct service_fixture {
  scratch_dir dir{"driftwire_serve"};
  fs::path data = dir.root() / "data";
  fs::path library = dir.root() / "library";
  fs::path other = dir.root() / "other";
  fs::path outside = dir.root() / "outside";
  std::unique_ptr<sp::secrets::store> store;
  key32 access{};
  key32 identity_public{};
  key32 device = random_key();
  std::unique_ptr<serve::server> srv;

  int per_source_pending = 2;
  int max_pending = 16;
  int max_sessions = 8;
  std::function<std::unique_ptr<sp::net::listener>()> make_listener;

  explicit service_fixture(int per_minute = 10000, int pending_per_source = 2, int pending_all = 16, int sessions = 8,
                           std::function<std::unique_ptr<sp::net::listener>()> listener_maker = {})
      : per_source_pending(pending_per_source),
        max_pending(pending_all),
        max_sessions(sessions),
        make_listener(std::move(listener_maker)) {
    fs::create_directories(data);
    fs::create_directories(library);
    fs::create_directories(other);
    fs::create_directories(outside);
    REQUIRE(sp::secrets::owner_only_folder(utf8(data / "secrets")));
    store = sp::secrets::file_store(utf8(data / "secrets"));
    REQUIRE(store);
    bool made = false;
    REQUIRE(serve::set_up(*store, identity_public, access, made));
    REQUIRE(made);
    REQUIRE(pair(device, "Phone"));
    start(per_minute);
  }

  bool pair(const key32& private_key, const std::string& name) {
    key32 pub;
    crypto_scalarmult_base(pub.data(), private_key.data());
    auto devices = known::load(*store);
    known::device d;
    d.name = name;
    d.key = pub;
    devices.push_back(d);
    return known::save(*store, devices);
  }
  void unpair(const key32& private_key) {
    key32 pub;
    crypto_scalarmult_base(pub.data(), private_key.data());
    auto devices = known::load(*store);
    std::erase_if(devices, [&](const known::device& d) { return d.key == pub; });
    REQUIRE(known::save(*store, devices));
  }

  void start(int per_minute) {
    serve::server::options o;
    o.cfg.port = 0;  // the OS picks: the tests only
    o.cfg.name = "Test NAS";
    o.cfg.roots = {{kRootA, "Photos", utf8(library)}, {kRootB, "Catalogue", utf8(other)}};
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = per_minute;
    o.per_source_pending = per_source_pending;
    o.per_block_pending = per_source_pending * 2;
    o.max_pending = max_pending;
    o.max_sessions = max_sessions;
    o.make_listener = make_listener;
    auto started = serve::server::start(o);
    REQUIRE(started);
    srv = std::move(*started);
  }

  std::shared_ptr<sp::link> connect() const {
    auto l = sp::net::connect({"::1", srv->port()}, 3000);
    REQUIRE(l);
    return std::shared_ptr<sp::link>(std::move(*l));
  }

  serve::client::options client_options(std::optional<key32> device_key = {}) const {
    serve::client::options o;
    o.access_key = access;
    o.device_key = device_key.value_or(device);
    o.service_key = identity_public;
    o.id = "aaaaaaaaaaaaaaaa";
    o.name = "Phone";
    return o;
  }

  std::unique_ptr<serve::client> open_client() const {
    auto c = serve::client::open(connect(), client_options());
    REQUIRE(c);
    return std::move(*c);
  }
};

// Reads a stream to its end; the bytes, in order.
std::string read_all(serve::client::read_stream& s) {
  std::string out;
  for (;;) {
    auto n = s.next(10000);
    REQUIRE(n);
    if (!*n) return out;
    out.append(reinterpret_cast<const char*>((*n)->bytes.data()), (*n)->bytes.size());
  }
}

// A raw hello for the tests: what a client puts on the link first.
std::vector<std::uint8_t> library_hello(const key32& psk) {
  const std::string prologue_text = "MediaViewer Driftwire library";
  std::vector<std::uint8_t> prologue(prologue_text.begin(), prologue_text.end());
  const std::uint8_t tail[] = {'M', 'V', 'S', 'P', 2, 'L'};
  prologue.insert(prologue.end(), std::begin(tail), std::end(tail));
  noise::handshake hs(noise::xx_hfs_psk0(), true, prologue, random_key(), psk);
  std::vector<std::uint8_t> m;
  REQUIRE(hs.write_message({}, m));
  std::vector<std::uint8_t> out = {'M', 'V', 'S', 'P', 2, 'L', static_cast<std::uint8_t>(m.size()),
                                   static_cast<std::uint8_t>(m.size() >> 8)};
  out.insert(out.end(), m.begin(), m.end());
  return out;
}

// Bytes back on `l` within `ms` (0 means it closed or stayed silent).
std::size_t bytes_back(sp::link& l, int ms) {
  std::array<std::uint8_t, 4096> buf;
  std::size_t total = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    auto n = l.read_some(buf, 100);
    if (!n) {
      if (n.error() == mv::status::timeout) continue;
      return total;
    }
    if (*n == 0) return total;
    total += *n;
  }
  return total;
}

}  // namespace

TEST_CASE("serve: a paired client lists, range-reads and seeks in a large file", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.library / "Trip" / "a.jpg", marker(0, 90000));
  write_file(s.library / "b.txt", "hello");
  write_file(s.library / ".hidden", "not listed");
  fs::create_directories(s.library / "Empty");
  // Sparse, with content planted past every 32-bit boundary.
  const fs::path big = s.library / "big.bin";
  const std::uint64_t big_size = 5'000'000'000ull;
  {
    std::ofstream f(big, std::ios::binary);
    f.seekp(0);
    const std::string at0 = marker(0, 70000);
    f.write(at0.data(), static_cast<std::streamsize>(at0.size()));
    const std::uint64_t far = 4'200'000'123ull;
    f.seekp(static_cast<std::streamoff>(far));
    const std::string atfar = marker(far, 200000);
    f.write(atfar.data(), static_cast<std::streamsize>(atfar.size()));
  }
  fs::resize_file(big, big_size);

  auto c = s.open_client();
  CHECK(c->caps().ops == serve::kAllOps);
  CHECK(c->caps().chunk <= serve::kInteractiveChunk);

  auto roots = c->roots();
  REQUIRE(roots);
  REQUIRE(roots->size() == 2);
  CHECK(roots->at(0).id == kRootA);
  CHECK(roots->at(0).label == "Photos");

  auto listing = c->list(kRootA, "");
  REQUIRE(listing);
  std::map<std::string, serve::list_entry> by_name;
  for (const auto& e : *listing) by_name[e.name] = e;
  CHECK(by_name.count("Trip") == 1);
  CHECK(by_name.count("big.bin") == 1);
  CHECK(by_name.count(".hidden") == 0);
  CHECK(by_name["Trip"].dir);
  CHECK(by_name["big.bin"].size == big_size);
  auto empty = c->list(kRootA, "Empty");
  REQUIRE(empty);
  CHECK(empty->empty());
  auto inside = c->list(kRootA, "Trip");
  REQUIRE(inside);
  REQUIRE(inside->size() == 1);
  CHECK(inside->at(0).name == "a.jpg");

  auto st = c->stat(kRootA, "big.bin");
  REQUIRE(st);
  CHECK(st->size == big_size);
  CHECK_FALSE(st->dir);

  // A small range in the middle of a small file.
  auto small = c->read_range(kRootA, "Trip/a.jpg", 1000, 2000);
  REQUIRE(small);
  CHECK(read_all(**small) == marker(1000, 2000));
  CHECK((*small)->code() == serve::end_code::ok);

  // The start of the big file, then a seek past 4 GiB.
  auto head = c->read_range(kRootA, "big.bin", 0, 60000);
  REQUIRE(head);
  CHECK(read_all(**head) == marker(0, 60000));
  const std::uint64_t far = 4'200'000'123ull;
  auto streaming = c->read_range(kRootA, "big.bin", 4'100'000'000ull, 500000);
  REQUIRE(streaming);
  auto first = (*streaming)->next(10000);
  REQUIRE(first);
  REQUIRE(first->has_value());
  CHECK((*first)->offset == 4'100'000'000ull);
  (*streaming)->cancel();
  auto seek = c->read_range(kRootA, "big.bin", far, 180000);
  REQUIRE(seek);
  CHECK(read_all(**seek) == marker(far, 180000));
  // A read past the end is empty and ends cleanly.
  auto past = c->read_range(kRootA, "big.bin", big_size + 100, 100);
  REQUIRE(past);
  CHECK(read_all(**past).empty());
  CHECK((*past)->code() == serve::end_code::ok);

  // A read of what is not there.
  auto gone = c->read_range(kRootA, "nope.bin", 0, 10);
  REQUIRE(gone);
  CHECK(read_all(**gone).empty());
  CHECK((*gone)->code() == serve::end_code::not_found);
  auto gone_stat = c->stat(kRootA, "nope.bin");
  CHECK_FALSE(gone_stat);

  c->close();
  CHECK(s.srv->stats().sessions == 1);
}

TEST_CASE("serve: without the access key there is no reply and no ML-KEM work", "[driftwire-serve]") {
  service_fixture s;
  REQUIRE(sodium_init() >= 0);
  const std::uint64_t kem_before = noise::kem_encapsulations();
  // A real-looking client with the wrong key.
  {
    const key32 wrong = random_key();
    auto l = s.connect();
    REQUIRE(l->write_all(library_hello(wrong)));
    CHECK(bytes_back(*l, 1500) == 0);
  }
  // Garbage, an internet hello, a rejoin, a port scanner's silence.
  for (const std::vector<std::uint8_t>& probe :
       {std::vector<std::uint8_t>{'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P'},
        std::vector<std::uint8_t>(64, 0x00),
        std::vector<std::uint8_t>{'M', 'V', 'S', 'P', 2, 'I'},
        std::vector<std::uint8_t>{'M', 'V', 'S', 'P', 2, 'R'},
        std::vector<std::uint8_t>{'M', 'V', 'S', 'P', 2, 'L', 0, 0}}) {
    auto l = s.connect();
    REQUIRE(l->write_all(probe));
    CHECK(bytes_back(*l, 400) == 0);
  }
  CHECK(noise::kem_encapsulations() == kem_before);
  CHECK(eventually([&] { return s.srv->stats().no_key >= 1; }));
  CHECK(eventually([&] { return s.srv->stats().bad_hello >= 2; }));
  CHECK(s.srv->stats().sessions == 0);
}

TEST_CASE("serve: a recorded first message is not answered twice", "[driftwire-serve]") {
  service_fixture s;
  REQUIRE(sodium_init() >= 0);
  const std::vector<std::uint8_t> hello = library_hello(s.access);
  const std::uint64_t before = noise::kem_encapsulations();
  {
    auto l = s.connect();
    REQUIRE(l->write_all(hello));
    CHECK(bytes_back(*l, 1500) > 0);  // the real key: the service answers
  }
  CHECK(noise::kem_encapsulations() == before + 1);
  // The same bytes again, as someone who recorded them.
  const std::uint64_t mid = noise::kem_encapsulations();
  {
    auto l = s.connect();
    REQUIRE(l->write_all(hello));
    CHECK(bytes_back(*l, 800) == 0);
  }
  CHECK(noise::kem_encapsulations() == mid);
  CHECK(eventually([&] { return s.srv->stats().replayed == 1; }));
}

TEST_CASE("serve: one address is limited, and goes no further", "[driftwire-serve]") {
  service_fixture s(/*per_minute=*/2);
  {
    auto a = s.connect();
    auto b = s.connect();
    auto c = s.connect();
    CHECK(eventually([&] { return s.srv->stats().over_limit >= 1; }));
    // Past the limit it is closed with nothing said, whatever it sends.
    (void)c->write_all(library_hello(s.access));
    CHECK(bytes_back(*c, 400) == 0);
  }
  CHECK(s.srv->stats().sessions == 0);
}

TEST_CASE("serve: one address cannot hold every handshake slot", "[driftwire-serve]") {
  service_fixture s;
  // Connected, saying nothing: the first two hold their slots for the hello's time.
  auto a = s.connect();
  auto b = s.connect();
  CHECK(eventually([&] { return s.srv->stats().accepted >= 2; }));
  auto c = s.connect();
  CHECK(eventually([&] { return s.srv->stats().over_limit >= 1; }));
  CHECK(bytes_back(*c, 300) == 0);
}

namespace {

// Two loopback doors, so a test has two real source addresses without
// binding anything but loopback: ::1 for the phone, 127.0.0.1 for the flood.
class two_doors final : public sp::net::listener {
 public:
  // `v6_as`, `v4_as`: the address each door's connections say they come from.
  two_doors(std::unique_ptr<sp::net::listener> v6, std::unique_ptr<sp::net::listener> v4,
            std::optional<sp::net::address> v6_as = {}, std::optional<sp::net::address> v4_as = {})
      : v6_(std::move(v6)), v4_(std::move(v4)) {
    v6_port_ = v6_->port();
    v4_port_ = v4_->port();
    t_[0] = std::thread([this, v6_as] { pump(*v6_, v6_as); });
    t_[1] = std::thread([this, v4_as] { pump(*v4_, v4_as); });
  }
  ~two_doors() override {
    shutdown();
    for (auto& t : t_) t.join();
  }
  std::uint16_t port() const noexcept override { return v6_port_; }
  std::uint16_t v4_port() const noexcept { return v4_port_; }
  mv::result<std::unique_ptr<sp::link>> accept(int timeout_ms) override {
    sp::net::address ignored;
    return accept_from(timeout_ms, ignored);
  }
  mv::result<std::unique_ptr<sp::link>> accept_from(int timeout_ms, sp::net::address& from) override {
    std::unique_lock lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return down_ || !queue_.empty(); });
    if (down_) return mv::err(mv::status::cancelled);
    if (queue_.empty()) return mv::err(mv::status::timeout);
    auto item = std::move(queue_.front());
    queue_.pop_front();
    from = item.second;
    return std::move(item.first);
  }
  void shutdown() noexcept override {
    {
      std::lock_guard lock(m_);
      down_ = true;
    }
    cv_.notify_all();
    v6_->shutdown();
    v4_->shutdown();
  }

 private:
  void pump(sp::net::listener& door, const std::optional<sp::net::address>& as) {
    for (;;) {
      sp::net::address from;
      auto l = door.accept_from(100, from);
      if (!l) {
        if (l.error() == mv::status::cancelled) return;
        continue;
      }
      std::lock_guard lock(m_);
      queue_.emplace_back(std::move(*l), as.value_or(from));
      cv_.notify_all();
    }
  }
  std::unique_ptr<sp::net::listener> v6_, v4_;
  std::uint16_t v6_port_ = 0, v4_port_ = 0;
  std::mutex m_;
  std::condition_variable cv_;
  bool down_ = false;
  std::deque<std::pair<std::unique_ptr<sp::link>, sp::net::address>> queue_;
  std::thread t_[2];
};

}  // namespace

TEST_CASE("serve: one flooding address does not lock out a phone at another", "[driftwire-serve]") {
  auto v6 = sp::net::listen_internet(0, true);
  REQUIRE(v6);
  auto v4 = sp::net::listen(true);
  REQUIRE(v4);
  const std::uint16_t v4_port = (*v4)->port();
  auto doors = std::make_shared<std::unique_ptr<sp::net::listener>>(
      std::make_unique<two_doors>(std::move(*v6), std::move(*v4)));
  service_fixture s(/*per_minute=*/30, 2, 16, 8, [doors] { return std::move(*doors); });

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> flood_sent{0};
  std::thread flood([&] {
    // Far past the service's 200 a second, from one address.
    while (!stop.load()) {
      auto l = sp::net::connect({"127.0.0.1", v4_port}, 200);
      if (l) flood_sent.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the flood is under way
  int ok = 0;
  for (int i = 0; i < 24; ++i) {
    auto l = sp::net::connect({"::1", s.srv->port()}, 3000);
    if (!l) continue;
    auto c = serve::client::open(std::shared_ptr<sp::link>(std::move(*l)), s.client_options());
    if (c) {
      ok += (*c)->roots().has_value() ? 1 : 0;
    }
  }
  stop.store(true);
  flood.join();
  std::printf("{\"case\":\"serve_flood\",\"phone_opened\":%d,\"of\":24,\"flood_connections\":%llu}\n", ok,
              static_cast<unsigned long long>(flood_sent.load()));
  CHECK(ok == 24);
}

TEST_CASE("serve: one address over its limit does not spend its /48's budget", "[driftwire-serve]") {
  auto v6 = sp::net::listen_internet(0, true);
  REQUIRE(v6);
  auto v4 = sp::net::listen(true);
  REQUIRE(v4);
  const std::uint16_t v4_port = (*v4)->port();
  // Both doors' connections claim addresses in one /48, in different /64s.
  auto doors = std::make_shared<std::unique_ptr<sp::net::listener>>(
      std::make_unique<two_doors>(std::move(*v6), std::move(*v4), sp::net::parse_address("2001:db8:1:2::1"),
                                  sp::net::parse_address("2001:db8:1:1::1")));
  service_fixture s(/*per_minute=*/2, 2, 16, 8, [doors] { return std::move(*doors); });
  {
    // Count the connects that got through (a loaded or sanitized runner drops some) and hold
    // them open until the service has accepted every one; then close them.
    std::vector<std::unique_ptr<sp::link>> noisy;
    for (int i = 0; i < 40; ++i) {
      auto l = sp::net::connect({"127.0.0.1", v4_port}, 2000);
      if (l) noisy.push_back(std::move(*l));
    }
    INFO(noisy.size() << " of 40 connects got through");
    // Well past the /48's eight, so a noisy /64 that spent the /48's budget would lock it out.
    REQUIRE(noisy.size() >= 16);
    const auto sent = noisy.size();
    REQUIRE(eventually([&] { return s.srv->stats().accepted >= sent; }, kSlowSeconds));
  }
  // The noisy /64 has spent its own two, not the /48's eight.
  auto c = s.open_client();
  CHECK(c->roots());
}

TEST_CASE("serve: a stalled handshake makes way once it has had a second", "[driftwire-serve]") {
  service_fixture s(10000, /*pending_per_source=*/100, /*pending_all=*/4);
  // Four holders of the key that get the reply and never send message 3.
  std::vector<std::shared_ptr<sp::link>> stalled;
  for (int i = 0; i < 4; ++i) {
    auto l = s.connect();
    REQUIRE(l->write_all(library_hello(s.access)));
    std::array<std::uint8_t, 64> reply;
    auto n = l->read_some(reply, 2000);  // the service's message 2: its hello was whole
    REQUIRE(n);
    REQUIRE(*n > 0);
    stalled.push_back(std::move(l));
  }
  // Too soon: none of them is old enough, and the newcomer is the one turned away.
  auto early = s.connect();
  CHECK(eventually([&] { return s.srv->stats().over_limit >= 1; }, 2));
  CHECK(s.srv->stats().evicted == 0);
  early.reset();
  // Past a second, but inside the 2 s they would be given anyway.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  auto c = s.open_client();
  CHECK(c->roots());
  CHECK(s.srv->stats().evicted >= 1);
}

TEST_CASE("serve: a full set of silent connections makes way for a new one", "[driftwire-serve]") {
  service_fixture s(10000, /*pending_per_source=*/100, /*pending_all=*/4);
  std::vector<std::shared_ptr<sp::link>> silent;
  for (int i = 0; i < 4; ++i) silent.push_back(s.connect());
  REQUIRE(eventually([&] { return s.srv->stats().accepted >= 4; }));
  // A hello gets 500 ms before it can be evicted.
  std::this_thread::sleep_for(std::chrono::milliseconds(650));
  // A real client arrives last and still gets in.
  auto c = s.open_client();
  CHECK(c->roots());
  CHECK(s.srv->stats().evicted >= 1);
}

TEST_CASE("serve: a handshake that stalls after the reply gives its slot back soon", "[driftwire-serve]") {
  service_fixture s;
  auto l = s.connect();
  REQUIRE(l->write_all(library_hello(s.access)));
  const auto from = std::chrono::steady_clock::now();
  CHECK(bytes_back(*l, 6000) > 0);  // the reply, then nothing of ours until it gives up
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
  INFO(ms << " ms");
  CHECK(ms >= 1000);
  CHECK(ms <= 3500);
}

TEST_CASE("serve: a burst is admitted up to the session limit", "[driftwire-serve]") {
  service_fixture s(10000, 100, 16, 8);
  std::atomic<int> ok{0};
  std::vector<std::unique_ptr<serve::client>> kept(8);
  std::vector<std::thread> openers;
  for (std::size_t i = 0; i < 8; ++i) {
    openers.emplace_back([&, i] {
      auto l = sp::net::connect({"::1", s.srv->port()}, 3000);
      if (!l) return;
      auto c = serve::client::open(std::shared_ptr<sp::link>(std::move(*l)), s.client_options());
      if (c) {
        kept[i] = std::move(*c);
        ok.fetch_add(1);
      }
    });
  }
  for (auto& t : openers) t.join();
  CHECK(ok.load() == 8);
  serve::refusal why = serve::refusal::none;
  auto ninth = serve::client::open(s.connect(), s.client_options(), &why);
  CHECK_FALSE(ninth);
  CHECK(why == serve::refusal::busy);
}

TEST_CASE("serve: a wrong prefix takes as long to close as a right one", "[driftwire-serve]") {
  service_fixture s;
  const auto time_to_close = [&](std::vector<std::uint8_t> first) {
    auto l = s.connect();
    REQUIRE(l->write_all(first));
    const auto from = std::chrono::steady_clock::now();
    CHECK(bytes_back(*l, 4000) == 0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
  };
  const auto wrong = time_to_close({'X', 'Y', 'Z', 'Q', 2, 'L', 0, 0});
  const auto right = time_to_close({'M', 'V', 'S', 'P', 2, 'L', 0xE0, 0x04});
  INFO("wrong " << wrong << " ms, right " << right << " ms");
  CHECK(wrong >= 2500);
  CHECK(right >= 2500);
}

TEST_CASE("serve: a key that cannot be read is never replaced", "[driftwire-serve]") {
  scratch_dir d("driftwire_serve_keys");
  const fs::path folder = d.root() / "secrets";
  REQUIRE(sp::secrets::owner_only_folder(utf8(folder)));
  auto store = sp::secrets::file_store(utf8(folder));
  key32 pub{}, key{}, pub2{}, key2{};
  bool made = false;
  REQUIRE(serve::set_up(*store, pub, key, made));
  CHECK(made);
  // A second run changes nothing.
  REQUIRE(serve::set_up(*store, pub2, key2, made));
  CHECK_FALSE(made);
  CHECK(pub2 == pub);
  CHECK(key2 == key);
  serve::server::options o;
  o.cfg.port = 0;
  o.cfg.roots = {{kRootA, "P", utf8(d.root())}};
  o.store = store.get();
  o.loopback_only = true;
#if !defined(_WIN32)
  const fs::path identity = folder / "secret-6964656e74697479";  // hex of "identity"
  REQUIRE(fs::exists(identity));
  REQUIRE(::chmod(utf8(identity).c_str(), 0644) == 0);
  CHECK_FALSE(serve::set_up(*store, pub2, key2, made));
  auto refused = serve::server::start(o);
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::not_found);
  REQUIRE(::chmod(utf8(identity).c_str(), 0600) == 0);
  REQUIRE(serve::set_up(*store, pub2, key2, made));
  CHECK(pub2 == pub);
#endif
  // A store with nothing in it: the service will not start and makes nothing.
  const fs::path empty_folder = d.root() / "empty";
  REQUIRE(sp::secrets::owner_only_folder(utf8(empty_folder)));
  auto empty = sp::secrets::file_store(utf8(empty_folder));
  o.store = empty.get();
  auto none = serve::server::start(o);
  REQUIRE_FALSE(none);
  CHECK(none.error() == mv::status::not_found);
  CHECK_FALSE(empty->get(known::kIdentityName));
}

TEST_CASE("serve: an unpaired and a revoked device are refused", "[driftwire-serve]") {
  service_fixture s;
  // The access key, but a device that was never paired.
  const key32 stranger = random_key();
  serve::refusal why = serve::refusal::none;
  auto denied = serve::client::open(s.connect(), s.client_options(stranger), &why);
  REQUIRE_FALSE(denied);
  CHECK(denied.error() == mv::status::permission_denied);
  CHECK(why == serve::refusal::not_paired);
  CHECK(eventually([&] { return s.srv->stats().not_paired == 1; }));

  // Paired: in. Revoked while connected: cut within a recheck, told why.
  auto c = s.open_client();
  auto listing = c->list(kRootA, "");
  REQUIRE(listing);
  s.unpair(s.device);
  s.srv->recheck_devices();
  CHECK(eventually([&] { return c->closed(); }, 5));
  CHECK(c->ended_why() == "revoked");
  CHECK_FALSE(c->list(kRootA, ""));
  c.reset();

  // And it cannot come back.
  why = serve::refusal::none;
  auto again = serve::client::open(s.connect(), s.client_options(), &why);
  REQUIRE_FALSE(again);
  CHECK(why == serve::refusal::not_paired);
}

TEST_CASE("serve: a path outside the named folders is refused", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.outside / "secret.txt", "outside");
  write_file(s.library / "ok.txt", "inside");
  auto c = s.open_client();

  const auto refused = [&](const std::string& root, const std::string& rel) {
    auto read = c->read_range(root, rel, 0, 10);
    REQUIRE(read);
    const std::string got = read_all(**read);
    INFO(rel);
    CHECK(got.empty());
    CHECK((*read)->code() == serve::end_code::refused);
    serve::end_code code = serve::end_code::ok;
    CHECK_FALSE(c->stat(root, rel, &code));
    CHECK(code == serve::end_code::refused);
    code = serve::end_code::ok;
    CHECK_FALSE(c->list(root, rel, &code));
    CHECK(code == serve::end_code::refused);
  };
  refused(kRootA, "../outside/secret.txt");
  refused(kRootA, "../../outside/secret.txt");
  refused(kRootA, "/etc/passwd");
  refused(kRootA, "a/../../outside/secret.txt");
  refused(kRootA, ".hidden/secret");
  refused(kRootA, "C:/Windows/win.ini");
  refused(kRootA, std::string("a\0b", 3));
  refused("0000000000000000", "ok.txt");  // no such root
  refused("not-a-root-id", "ok.txt");

#if !defined(_WIN32)
  // Links out of a named folder, at the leaf and in the middle.
  fs::create_symlink(s.outside / "secret.txt", s.library / "link.txt");
  fs::create_symlink(s.outside, s.library / "away");
  refused(kRootA, "link.txt");
  refused(kRootA, "away/secret.txt");
  auto listing = c->list(kRootA, "");
  REQUIRE(listing);
  for (const auto& e : *listing) {
    CHECK(e.name != "link.txt");
    CHECK(e.name != "away");
  }
#endif
  auto ok = c->read_range(kRootA, "ok.txt", 0, 100);
  REQUIRE(ok);
  CHECK(read_all(**ok) == "inside");
}

#if !defined(_WIN32)
TEST_CASE("serve: a folder swapped for a link during a read never serves outside bytes", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.library / "sw" / "f.txt", "inside");
  write_file(s.outside / "f.txt", "OUTSIDE");
  const fs::path sw = s.library / "sw";
  const fs::path real = s.library / "sw_real";
  const fs::path lnk = s.library / "sw_link";
  fs::create_symlink(s.outside, lnk);
  std::atomic<bool> stop{false};
  std::thread swapper([&] {
    // A checked folder becomes a link to outside the root, and back.
    const std::string sw8 = utf8(sw), real8 = utf8(real), lnk8 = utf8(lnk);
    while (!stop.load()) {
      (void)::rename(sw8.c_str(), real8.c_str());
      (void)::rename(lnk8.c_str(), sw8.c_str());
      std::this_thread::yield();
      (void)::rename(sw8.c_str(), lnk8.c_str());
      (void)::rename(real8.c_str(), sw8.c_str());
      std::this_thread::yield();
    }
  });
  auto c = s.open_client();
  int outside_reads = 0, inside_reads = 0, listed_outside = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (std::chrono::steady_clock::now() < until) {
    auto read = c->read_range(kRootA, "sw/f.txt", 0, 16);
    REQUIRE(read);
    const std::string got = read_all(**read);
    outside_reads += got == "OUTSIDE" ? 1 : 0;
    inside_reads += got == "inside" ? 1 : 0;
    if (auto l = c->list(kRootA, "sw")) {
      for (const auto& e : *l) listed_outside += e.size == 7 ? 1 : 0;  // "OUTSIDE"
    }
  }
  stop.store(true);
  swapper.join();
  INFO("inside " << inside_reads);
  CHECK(outside_reads == 0);
  CHECK(listed_outside == 0);
  CHECK(inside_reads > 0);  // the race did not simply stop every read
}
#endif

TEST_CASE("serve: credit bounds what is sent; cancel ends a stream; chunks stay small", "[driftwire-serve]") {
  service_fixture s;
  const std::string body(4 * 1024 * 1024, 'x');
  write_file(s.library / "file.bin", body);

  // The protocol directly, so a test can count what arrives per credit.
  sp::channel::options o;
  o.access_key = s.access;
  o.static_key = s.device;
  auto opened = sp::channel::open_library(s.connect(), "aaaaaaaaaaaaaaaa", "Raw", o);
  REQUIRE(opened);
  auto& ch = **opened;
  CHECK(ch.library());
  REQUIRE(ch.send(sp::msg::serve_hello, serve::hello_json(serve::kAllOps)));
  sp::message m;
  REQUIRE(ch.receive(m, 3000));
  REQUIRE(m.type == sp::msg::serve_caps);
  const auto caps = serve::parse_caps(m.text());
  REQUIRE(caps);
  CHECK(caps->chunk == serve::kInteractiveChunk);

  serve::open_request r;
  r.id = 7;
  r.kind = serve::op::read_range;
  r.root = kRootA;
  r.rel = "file.bin";
  r.at = 0;
  r.length = body.size();
  r.credit = 128 * 1024;
  REQUIRE(ch.send(sp::msg::stream_open, serve::encode_open(r)));

  std::uint64_t received = 0;
  const auto drain = [&](std::uint64_t expect) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (received < expect && std::chrono::steady_clock::now() < until) {
      auto got = ch.receive(m, 500);
      if (!got) continue;
      if (m.type != sp::msg::stream_data) continue;
      const auto d = serve::parse_data(m.body);
      REQUIRE(d);
      CHECK(d->id == 7);
      CHECK(d->bytes.size() <= serve::kInteractiveChunk);
      received += d->bytes.size();
    }
    // Nothing more arrives past the credit, however long it waits.
    auto extra = ch.receive(m, 300);
    if (extra) CHECK(m.type != sp::msg::stream_data);
  };
  drain(128 * 1024);
  CHECK(received == 128 * 1024);
  REQUIRE(ch.send(sp::msg::stream_credit, serve::encode_credit(7, 64 * 1024)));
  drain(192 * 1024);
  CHECK(received == 192 * 1024);

  // Cancelled: no more bytes and no end; a new stream runs.
  REQUIRE(ch.send(sp::msg::stream_cancel, serve::encode_cancel(7)));
  r.id = 8;
  r.length = 1024;
  r.credit = 4096;
  r.at = 100;
  REQUIRE(ch.send(sp::msg::stream_open, serve::encode_open(r)));
  std::optional<serve::end_code> end;
  std::uint64_t second = 0;
  for (int i = 0; i < 50 && !end; ++i) {
    auto got = ch.receive(m, 1000);
    if (!got) continue;
    if (m.type == sp::msg::stream_data) {
      const auto d = serve::parse_data(m.body);
      REQUIRE(d);
      if (d->id == 8) second += d->bytes.size();
    } else if (m.type == sp::msg::stream_end) {
      const auto e = serve::parse_end(m.body);
      REQUIRE(e);
      CHECK(e->id == 8);  // never stream 7's
      end = e->code;
    }
  }
  CHECK(end == serve::end_code::ok);
  CHECK(second == 1024);
}

TEST_CASE("serve: credit or a cancel that lands while a stream opens is not lost", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.library / "f.bin", std::string(200000, 'y'));
  sp::channel::options o;
  o.access_key = s.access;
  o.static_key = s.device;
  auto opened = sp::channel::open_library(s.connect(), "aaaaaaaaaaaaaaaa", "Raw", o);
  REQUIRE(opened);
  auto& ch = **opened;
  REQUIRE(ch.send(sp::msg::serve_hello, serve::hello_json(serve::kAllOps)));
  sp::message m;
  REQUIRE(ch.receive(m, 3000));
  serve::open_request r;
  r.kind = serve::op::read_range;
  r.root = kRootA;
  r.rel = "f.bin";
  r.length = 1000;
  // Far more opens and immediate cancels than the session has stream slots.
  for (std::uint32_t id = 1; id <= 400; ++id) {
    r.id = id;
    r.credit = 16;
    REQUIRE(ch.send(sp::msg::stream_open, serve::encode_open(r)));
    REQUIRE(ch.send(sp::msg::stream_cancel, serve::encode_cancel(id)));
  }
  // Opened with no credit, topped up at once: it must still run.
  r.id = 1000;
  r.credit = 0;
  REQUIRE(ch.send(sp::msg::stream_open, serve::encode_open(r)));
  REQUIRE(ch.send(sp::msg::stream_credit, serve::encode_credit(1000, 1000)));
  std::uint64_t got = 0;
  std::optional<serve::end_code> end;
  for (int i = 0; i < 3000 && !end; ++i) {
    if (!ch.receive(m, 3000)) break;
    if (m.type == sp::msg::stream_data) {
      const auto d = serve::parse_data(m.body);
      REQUIRE(d);
      if (d->id == 1000) got += d->bytes.size();
    } else if (m.type == sp::msg::stream_end) {
      const auto e = serve::parse_end(m.body);
      REQUIRE(e);
      if (e->id == 1000) end = e->code;
    }
  }
  CHECK(end == serve::end_code::ok);
  CHECK(got == 1000);
}

TEST_CASE("serve: capabilities are negotiated, and old channels never take these ids", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.library / "a.txt", "a");

  // A client that only knows list and stat: reads are not agreed.
  auto o = s.client_options();
  o.ops = serve::op_bit(serve::op::list) | serve::op_bit(serve::op::stat);
  auto narrow = serve::client::open(s.connect(), o);
  REQUIRE(narrow);
  CHECK((*narrow)->caps().ops == o.ops);
  auto l = (*narrow)->list(kRootA, "");
  REQUIRE(l);
  CHECK(l->size() == 1);
  auto read = (*narrow)->read_range(kRootA, "a.txt", 0, 1);
  REQUIRE(read);
  CHECK(read_all(**read).empty());
  CHECK((*read)->code() == serve::end_code::unsupported);
  serve::end_code code = serve::end_code::ok;
  CHECK_FALSE((*narrow)->roots(&code));
  CHECK(code == serve::end_code::unsupported);

  // A client from the future names an operation nobody has: its own stream
  // fails, the session goes on.
  sp::channel::options co;
  co.access_key = s.access;
  co.static_key = s.device;
  auto raw = sp::channel::open_library(s.connect(), "aaaaaaaaaaaaaaaa", "Future", co);
  REQUIRE(raw);
  REQUIRE((*raw)->send(sp::msg::serve_hello, std::string_view(R"({"v":99,"min":1,"ops":["list","teleport"]})")));
  sp::message m;
  REQUIRE((*raw)->receive(m, 3000));
  REQUIRE(m.type == sp::msg::serve_caps);
  auto caps = serve::parse_caps(m.text());
  REQUIRE(caps);
  CHECK(caps->version == serve::kVersion);
  CHECK(caps->ops == serve::op_bit(serve::op::list));
  std::vector<std::uint8_t> frame = {1, 0, 0, 0};
  const std::string req = R"({"op":"teleport"})";
  frame.insert(frame.end(), req.begin(), req.end());
  REQUIRE((*raw)->send(sp::msg::stream_open, frame));
  REQUIRE((*raw)->receive(m, 3000));
  REQUIRE(m.type == sp::msg::stream_end);
  auto e = serve::parse_end(m.body);
  REQUIRE(e);
  CHECK(e->code == serve::end_code::unsupported);

  // A serve message on a channel that is not a library channel: the peer
  // has not agreed to it and the session is not a library's, so it fails.
  auto listener = sp::net::listen(true);
  REQUIRE(listener);
  std::unique_ptr<sp::channel> server_side;
  std::thread t([&] {
    auto incoming = (*listener)->accept(5000);
    REQUIRE(incoming);
    std::shared_ptr<sp::link> in(std::move(*incoming));
    auto opened = sp::channel::open(in, false, "bbbbbbbbbbbbbbbb", "Bea");
    REQUIRE(opened);
    server_side = std::move(*opened);
  });
  auto out = sp::net::connect({"127.0.0.1", (*listener)->port()}, 3000);
  REQUIRE(out);
  std::shared_ptr<sp::link> out_shared(std::move(*out));
  auto client_side = sp::channel::open(out_shared, true, "aaaaaaaaaaaaaaaa", "Ann");
  REQUIRE(client_side);
  t.join();
  REQUIRE(server_side);
  CHECK_FALSE((*client_side)->library());
  REQUIRE((*client_side)->send(sp::msg::serve_hello, std::string_view("{}")));
  sp::message got;
  auto result = server_side->receive(got, 1000);
  CHECK_FALSE(result);
  server_side->shutdown();
  (*client_side)->shutdown();
}

TEST_CASE("serve: the file store is owner-only and the config is checked", "[driftwire-serve]") {
  scratch_dir s("driftwire_serve_store");
  const fs::path folder = s.root() / "secrets";
  REQUIRE(sp::secrets::owner_only_folder(utf8(folder)));
  auto store = sp::secrets::file_store(utf8(folder));
  REQUIRE(store);
  const std::vector<std::uint8_t> value = {1, 2, 3};
  REQUIRE(store->put("identity", value));
  CHECK(store->get("identity") == value);
#if !defined(_WIN32)
  {
    struct stat st{};
    REQUIRE(::stat(utf8(folder).c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    const fs::path file = folder / "secret-6964656e74697479";  // hex of "identity"
    REQUIRE(fs::exists(file));
    REQUIRE(::stat(utf8(file).c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    // A value someone else can read is not a secret and is not trusted.
    REQUIRE(::chmod(utf8(file).c_str(), 0644) == 0);
    CHECK_FALSE(store->get("identity"));
    REQUIRE(::chmod(utf8(file).c_str(), 0600) == 0);
    CHECK(store->get("identity") == value);
    // A link in the folder's place is not followed.
    fs::remove(file);
    fs::create_symlink("/etc/hosts", file);
    CHECK_FALSE(store->get("identity"));
  }
  // A folder others can read is refused.
  const fs::path open_folder = s.root() / "shared";
  fs::create_directories(open_folder);
  REQUIRE(::chmod(utf8(open_folder).c_str(), 0755) == 0);
  CHECK_FALSE(sp::secrets::owner_only_folder(utf8(open_folder)));
#endif

  CHECK(serve::parse_config(R"({"port":47700,"roots":[{"id":"aaaaaaaaaaaaaaaa","label":"P","path":"/volume1/photo"}]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":0,"roots":[{"id":"aaaaaaaaaaaaaaaa","label":"P","path":"/v"}]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":99999,"roots":[{"id":"aaaaaaaaaaaaaaaa","label":"P","path":"/v"}]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":47700,"roots":[]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":47700,"roots":[{"id":"nothex","label":"P","path":"/v"}]})"));
  CHECK_FALSE(serve::parse_config(R"({"port":47700,"roots":[{"id":"aaaaaaaaaaaaaaaa","label":"P","path":"rel/ative"}]})"));
  CHECK_FALSE(serve::parse_config(
      R"({"port":47700,"roots":[{"id":"aaaaaaaaaaaaaaaa","label":"P","path":"/v"},{"id":"aaaaaaaaaaaaaaaa","label":"Q","path":"/w"}]})"));
}

#if !defined(_WIN32)
// The program itself, with no terminal: stdin from /dev/null, its output piped.
TEST_CASE("serve: the program runs without a terminal, keeps its keys and stops on a signal", "[driftwire-serve]") {
  scratch_dir s("driftwire_serve_cli");
  const fs::path data = s.root() / "svc";
  fs::create_directories(data);
  const fs::path library = s.root() / "lib";
  write_file(library / "a.txt", "from the program");
  {
    std::ofstream f(data / "serve.json", std::ios::binary);
    f << R"({"port":0,"name":"CLI","roots":[{"id":"cccccccccccccccc","label":"Lib","path":")" << utf8(library) << R"("}]})";
  }
  const std::string base = std::string("'") + MV_DRIFTWIRE_CLI_TEST_PATH + "' serve --test-data '" + utf8(data) + "' ";
  const auto run_line = [&](const std::string& rest) {
    FILE* p = popen((base + rest + " </dev/null 2>&1").c_str(), "r");
    REQUIRE(p);
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, p)) out += buf;
    CHECK(pclose(p) == 0);
    return out;
  };
  // init writes owner-only keys and prints the key once.
  const std::string init = run_line("init --no-passphrase");
  auto hex_after = [](const std::string& text, const std::string& label) {
    const auto at = text.find(label);
    REQUIRE(at != std::string::npos);
    return text.substr(at + label.size(), 64);
  };
  const auto identity = sp::parse_key_hex(hex_after(init, "identity "));
  const auto access = sp::parse_key_hex(hex_after(init, "access-key "));
  REQUIRE(identity);
  REQUIRE(access);
  CHECK(run_line("init --no-passphrase").find("access-key") == std::string::npos);  // shown once
  // A device is paired from the command line.
  const key32 device = random_key();
  key32 device_public;
  crypto_scalarmult_base(device_public.data(), device.data());
  CHECK(run_line("add Phone " + sp::key_hex(device_public)).find("paired") != std::string::npos);
  CHECK(run_line("devices").find("Phone") != std::string::npos);

  // A config that someone else could change is refused.
  REQUIRE(::chmod(utf8(data / "serve.json").c_str(), 0666) == 0);
  {
    FILE* bad = popen((base + " </dev/null 2>&1").c_str(), "r");
    REQUIRE(bad);
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, bad)) out += buf;
    CHECK(pclose(bad) != 0);
    CHECK(out.find("writable by this user alone") != std::string::npos);
  }
  REQUIRE(::chmod(utf8(data / "serve.json").c_str(), 0644) == 0);

  // The service, read through a pipe; `exec` so the pid is the program's.
  FILE* p = popen(("sh -c 'echo $$; exec \"$0\" \"$@\"' '" + std::string(MV_DRIFTWIRE_CLI_TEST_PATH) +
                   "' serve --test-loopback --test-data '" + utf8(data) + "' </dev/null")
                      .c_str(),
                  "r");
  REQUIRE(p);
  char line[256];
  REQUIRE(fgets(line, sizeof line, p));
  const int pid = std::atoi(line);
  REQUIRE(pid > 0);
  // A failed check must not leave the child holding this test's pipes.
  struct stopper {
    int pid;
    ~stopper() { (void)::kill(pid, SIGKILL); }
  } stop_child{pid};
  REQUIRE(fgets(line, sizeof line, p));
  const std::string listening = line;
  CHECK(listening.find("listening on port") != std::string::npos);
  const auto port = static_cast<std::uint16_t>(std::atoi(listening.substr(listening.rfind(' ') + 1).c_str()));
  REQUIRE(port != 0);

  auto l = sp::net::connect({"::1", port}, 3000);
  REQUIRE(l);
  serve::client::options o;
  o.access_key = *access;
  o.device_key = device;
  o.service_key = *identity;
  o.id = "aaaaaaaaaaaaaaaa";
  auto c = serve::client::open(std::shared_ptr<sp::link>(std::move(*l)), o);
  REQUIRE(c);
  auto read = (*c)->read_range("cccccccccccccccc", "a.txt", 0, 100);
  REQUIRE(read);
  CHECK(read_all(**read) == "from the program");

  // Revoked from the command line, while running: the program's timer cuts it.
  CHECK(run_line("revoke Phone").find("revoked") != std::string::npos);
  CHECK(eventually([&] { return (*c)->closed(); }, 10));
  CHECK((*c)->ended_why() == "revoked");
  c->reset();

  REQUIRE(::kill(pid, SIGTERM) == 0);
  std::string rest;
  while (fgets(line, sizeof line, p)) rest += line;
  CHECK(rest.find("stopped") != std::string::npos);
  int status = pclose(p);
  CHECK(status == 0);
}
#endif
