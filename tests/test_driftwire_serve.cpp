// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire serve` over loopback (docs/design/28 "The NAS service"): the
// M2b verify line as far as loopback can show it. A paired client lists,
// range-reads and seeks in a large file; a client without the access key gets
// no reply and costs no ML-KEM work; a revoked device is refused; a path
// outside the named folders is refused. Filter with "[driftwire-serve]";
// nothing here touches the network beyond 127.0.0.1.
#include <sodium.h>

#include <condition_variable>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "driftwire_fixture.h"

#include "wire/grants.h"
#include "wire/known.h"
#include "wire/noise.h"
#include "wire/serve.h"
#include "wire/serve_backup.h"
#include "wire/serve_client.h"
#include "wire/udp_hub.h"
#include "io/content_hash.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
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
const std::string kRootC = "c3c3c3c3c3c3c3c3";  // a backup folder (phone backup, 1.1)
const std::string kRootD = "d4d4d4d4d4d4d4d4";  // the real Catalogue (a folder's .mediaviewer)

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
  fs::path backup = dir.root() / "backup";
  fs::path catalogue = dir.root() / "shared" / ".mediaviewer";
  bool copy_moves = false;  // options::copy_moves
  std::function<void()> during_copy;                       // options::during_copy
  std::vector<serve::root_config> extra_roots;             // more named folders
  std::function<std::int64_t()> now_unix;                  // options::now_unix
  std::function<std::chrono::steady_clock::time_point()> now_steady;
  std::uint64_t backup_limit = 64ull << 20;
  std::uint64_t backup_reserve = 0;
  std::atomic<std::uint64_t> free_bytes{1ull << 40};
  std::function<bool(const key32&)> may_write;  // empty: the service's own rule
  std::unique_ptr<sp::secrets::store> store;
  key32 access{};
  key32 identity_public{};
  key32 device = random_key();
  std::unique_ptr<serve::server> srv;

  int per_source_pending = 2;
  int max_pending = 16;
  int max_sessions = 8;
  bool udp = true;  // options::udp
  int max_per_device = 8;
  std::function<std::unique_ptr<sp::net::listener>()> make_listener;
  std::function<std::string()> storage;  // options::storage (the host's drives keeper)
  std::shared_ptr<serve::convert::service> converter;  // options::converter

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
    fs::create_directories(backup);
    fs::create_directories(catalogue);
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
    serve::root_config b{kRootC, "Phone backup", utf8(backup)};
    b.backup = true;
    b.limit = backup_limit;
    b.reserve = backup_reserve;
    o.cfg.roots.push_back(b);
    o.cfg.roots.push_back({kRootD, "Catalogue", utf8(catalogue)});
    o.copy_moves = copy_moves;
    o.during_copy = during_copy;
    o.now_unix = now_unix;
    o.now_steady = now_steady;
    for (const auto& r : extra_roots) o.cfg.roots.push_back(r);
    o.free_bytes = [this](const std::string&) { return free_bytes.load(); };
    o.may_write = may_write;
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = per_minute;
    o.per_source_pending = per_source_pending;
    o.per_block_pending = per_source_pending * 2;
    o.max_pending = max_pending;
    o.max_sessions = max_sessions;
    o.udp = udp;
    o.max_per_device = max_per_device;
    o.make_listener = make_listener;
    o.storage = storage;
    o.converter = converter;
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
    o.ops = serve::kAllOps | serve::kWriteOp | serve::kChangeOps;
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
  CHECK(c->caps().ops == (serve::kAllOps | serve::kWriteOp | serve::kChangeOps));
  CHECK(c->caps().chunk <= serve::kInteractiveChunk);

  auto roots = c->roots();
  REQUIRE(roots);
  REQUIRE(roots->size() == 4);
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

// A phone's session over UDP (options::udp): what link_session::try_udp opens.
std::unique_ptr<serve::client> open_over_udp(service_fixture& s, sp::udp_hub& hub, std::uint16_t port,
                                             std::optional<key32> device = {}, serve::refusal* why = nullptr) {
  std::shared_ptr<sp::link> l(hub.stream({"127.0.0.1", port}, randombytes_random()));
  auto c = serve::client::open(l, s.client_options(device), why);
  return c ? std::move(*c) : nullptr;
}

TEST_CASE("serve udp: a session over a UDP stream, on the port the caps name, reads a file whole",
          "[driftwire-serve]") {
  service_fixture s;
  const std::size_t size = 3 * 1024 * 1024 + 123;
  write_file(s.library / "clip.bin", marker(0, size));
  const auto port = s.open_client()->caps().udp;  // said over TCP
  REQUIRE(port == s.srv->port());
  auto hub = sp::udp_hub::open(0, true);
  REQUIRE(hub);
  auto c = open_over_udp(s, **hub, port);
  REQUIRE(c);
  CHECK(c->caps().udp == port);
  auto r = c->read_range(kRootA, "clip.bin", 0, size, serve::kMaxStreamCredit, true);
  REQUIRE(r);
  CHECK(read_all(**r) == marker(0, size));
  CHECK((*r)->code() == serve::end_code::ok);
  CHECK(s.srv->stats().udp == 1);
}

TEST_CASE("serve udp: a read over a lossy UDP stream arrives whole", "[driftwire-serve]") {
  service_fixture s;
  const std::size_t size = 1024 * 1024;
  write_file(s.library / "lossy.bin", marker(7, size));
  auto hub = sp::udp_hub::open(0, true);
  REQUIRE(hub);
  (*hub)->test_loss(30);  // 3 % of what the phone sends: the handshake, requests, acks
  auto c = open_over_udp(s, **hub, s.srv->port());
  REQUIRE(c);
  auto r = c->read_range(kRootA, "lossy.bin", 0, size, serve::kMaxStreamCredit, true);
  REQUIRE(r);
  CHECK(read_all(**r) == marker(7, size));
}

TEST_CASE("serve udp: over UDP a stranger is refused as over TCP, and a service with it off offers none",
          "[driftwire-serve]") {
  service_fixture s;
  auto hub = sp::udp_hub::open(0, true);
  REQUIRE(hub);
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(open_over_udp(s, **hub, s.srv->port(), random_key(), &why));
  CHECK(why == serve::refusal::not_paired);
  CHECK(s.srv->stats().udp == 0);

  s.srv.reset();
  s.udp = false;
  s.start(10000);
  CHECK(s.open_client()->caps().udp == 0);
}

TEST_CASE("serve udp: a flood of UDP opens never stops a TCP session being admitted", "[driftwire-serve]") {
  // A UDP open's source is whatever its datagram says: none of TCP's budget may go to it.
  service_fixture s(10000, /*pending_per_source=*/100, /*pending_all=*/4, 8);
  auto hub = sp::udp_hub::open(0, true);
  REQUIRE(hub);
  constexpr int kOpens = 12;
  for (int i = 0; i < kOpens; ++i) {
    // A stream's first data packet (seq 0) and nothing more: a session that never says its hello.
    std::uint8_t d[11] = {sp::kStream, 1, 0, 0, 0, static_cast<std::uint8_t>(i + 1), 0, 0, 0, 0, 0x55};
    REQUIRE((*hub)->send(d, {"127.0.0.1", s.srv->port()}));
  }
  REQUIRE(eventually([&] { return s.srv->stats().accepted >= kOpens; }));
  CHECK(s.srv->stats().over_limit >= kOpens - 4);  // past UDP's own few, turned away
  // Straight after, while every one of them is still in its hello.
  auto c = serve::client::open(s.connect(), s.client_options());
  REQUIRE(c);
  CHECK((*c)->caps().udp == s.srv->port());
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

TEST_CASE("serve: one device's sessions stop at its own limit, and another device still gets in",
          "[driftwire-serve]") {
  service_fixture s;
  s.srv.reset();
  s.max_sessions = 6;
  s.max_per_device = 3;
  s.start(10000);
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second phone"));

  std::vector<std::unique_ptr<serve::client>> first;
  for (int i = 0; i < 3; ++i) {
    auto c = serve::client::open(s.connect(), s.client_options());
    REQUIRE(c);
    first.push_back(std::move(*c));
  }
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(serve::client::open(s.connect(), s.client_options(), &why));
  CHECK(why == serve::refusal::busy);

  // Three of six in use, all the first phone's: the second still has its own.
  std::vector<std::unique_ptr<serve::client>> other;
  for (int i = 0; i < 3; ++i) {
    auto c = serve::client::open(s.connect(), s.client_options(second));
    REQUIRE(c);
    other.push_back(std::move(*c));
  }
  why = serve::refusal::none;
  CHECK_FALSE(serve::client::open(s.connect(), s.client_options(second), &why));
  CHECK(why == serve::refusal::busy);

  // A session that ends gives its device's slot back.
  first.front()->close();
  first.erase(first.begin());
  std::unique_ptr<serve::client> again;
  for (int i = 0; i < 200 && !again; ++i) {  // the service sees the close a moment later
    why = serve::refusal::none;
    if (auto c = serve::client::open(s.connect(), s.client_options(), &why)) again = std::move(*c);
    else REQUIRE(why == serve::refusal::busy);
  }
  CHECK(again);
}

TEST_CASE("serve: a connection turned away as busy gives its place back before it is told", "[driftwire-serve]") {
  service_fixture s;
  s.srv.reset();
  s.max_sessions = 2;
  s.max_per_device = 1;
  s.start(10000);
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second phone"));
  auto first = serve::client::open(s.connect(), s.client_options());
  REQUIRE(first);
  // The first phone again, over its limit: through the handshake, then it never reads its
  // refusal, so the service waits (kCancelWaitMs) to say it.
  sp::channel_options co;
  co.access_key = s.access;
  co.static_key = s.device;
  auto refused = sp::channel::open_library(s.connect(), "aaaaaaaaaaaaaaaa", "Phone", co);
  REQUIRE(refused);
  // Meanwhile the second phone takes the one place left: the refused one is not holding it.
  auto other = serve::client::open(s.connect(), s.client_options(second));
  CHECK(other);
}

TEST_CASE("serve: by default a phone copying over all its lanes leaves room for another phone",
          "[driftwire-serve]") {
  service_fixture s;
  s.srv.reset();
  s.max_sessions = serve::server::options{}.max_sessions;
  s.max_per_device = serve::server::options{}.max_per_device;
  s.start(10000);
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second phone"));
  std::vector<std::unique_ptr<serve::client>> kept;
  for (int i = 0; i < 8; ++i) {  // more lanes than a copy opens today (sync_config::lanes)
    auto c = serve::client::open(s.connect(), s.client_options());
    REQUIRE(c);
    kept.push_back(std::move(*c));
  }
  auto other = serve::client::open(s.connect(), s.client_options(second));
  CHECK(other);
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

TEST_CASE("serve: a named folder whose drive is gone is listed offline, and comes back", "[driftwire-serve]") {
  service_fixture s;
  write_file(s.other / "a.txt", "on the drive");
  auto c = s.open_client();
  const auto offline = [&](const std::string& id) {
    auto roots = c->roots();
    REQUIRE(roots);
    for (const auto& r : *roots) {
      if (r.id == id) return r.offline;
    }
    FAIL("no such root");
    return false;
  };
  CHECK_FALSE(offline(kRootA));
  CHECK_FALSE(offline(kRootB));

  // The drive unplugged: its folder is not there. The session goes on.
  const fs::path away = s.dir.root() / "unplugged";
  fs::rename(s.other, away);
  CHECK(offline(kRootB));
  CHECK_FALSE(offline(kRootA));
  serve::end_code code = serve::end_code::ok;
  CHECK_FALSE(c->list(kRootB, "", &code));
  CHECK(code == serve::end_code::not_found);
  code = serve::end_code::ok;
  CHECK_FALSE(c->stat(kRootB, "a.txt", &code));
  CHECK(code == serve::end_code::not_found);
  auto gone = c->read_range(kRootB, "a.txt", 0, 100);
  REQUIRE(gone);
  CHECK(read_all(**gone).empty());
  CHECK((*gone)->code() == serve::end_code::not_found);
  CHECK_FALSE(c->closed());

  // Plugged back in: the same session reads it again.
  fs::rename(away, s.other);
  CHECK_FALSE(offline(kRootB));
  auto back = c->read_range(kRootB, "a.txt", 0, 100);
  REQUIRE(back);
  CHECK(read_all(**back) == "on the drive");
}

TEST_CASE("serve: the roots list says offline only when it is", "[driftwire-serve]") {
  std::vector<serve::root_info> roots(2);
  roots[0].id = "a1a1a1a1a1a1a1a1";
  roots[0].label = "Photos";
  roots[1].id = "b2b2b2b2b2b2b2b2";
  roots[1].label = "Drive";
  roots[1].offline = true;
  const std::string text = serve::roots_json(roots);
  // A root that is there reads exactly as before, for an older phone.
  CHECK(text.find("\"offline\"") == text.rfind("\"offline\""));
  auto back = serve::parse_roots(text);
  REQUIRE(back);
  REQUIRE(back->size() == 2);
  CHECK_FALSE((*back)[0].offline);
  CHECK((*back)[1].offline);
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

// ---- phone backup (docs/plans/mobile.md §10b, M11) -----------------------------------------

namespace {

std::string hash_of(std::string_view bytes) {
  return mv::io::hash_bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()))
      .hex();
}

serve::client::read_fn reader_of(const std::string& bytes) {
  return [&bytes](std::uint64_t at, std::span<std::uint8_t> into) -> mv::result<std::size_t> {
    if (at >= bytes.size()) return std::size_t{0};
    const std::size_t n = std::min<std::size_t>(into.size(), bytes.size() - static_cast<std::size_t>(at));
    std::memcpy(into.data(), bytes.data() + at, n);
    return n;
  };
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Every unfinished copy anywhere under `root`.
int parts_under(const fs::path& root) {
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (e.path().filename().string().ends_with(".mvpart")) ++n;
  }
  return n;
}

key32 public_of(const key32& private_key) {
  key32 pub;
  crypto_scalarmult_base(pub.data(), private_key.data());
  return pub;
}

}  // namespace

TEST_CASE("serve backup: a file is written whole, checked, and never replaces another", "[driftwire-serve]") {
  service_fixture s;
  auto c = s.open_client();
  REQUIRE((c->caps().ops & serve::kWriteOp) != 0);

  auto roots = c->roots();
  REQUIRE(roots);
  const auto it = std::find_if(roots->begin(), roots->end(), [](const serve::root_info& r) { return r.id == kRootC; });
  REQUIRE(it != roots->end());
  CHECK(it->backup);
  CHECK(it->writable);
  CHECK_FALSE(roots->front().backup);

  const std::string photo = marker(0, 3 * 1024 * 1024 + 17);  // past one window and one chunk
  serve::client::write_refused why;
  std::uint64_t progressed = 0;
  auto done = c->write(kRootC, "2026/09/IMG_0412.HEIC", photo.size(), hash_of(photo), 0, reader_of(photo),
                       [&](std::uint64_t n) { progressed = n; }, nullptr, &why);
  REQUIRE(done);
  CHECK(done->name == "2026/09/IMG_0412.HEIC");
  CHECK_FALSE(done->existing);
  CHECK(progressed == photo.size());
  CHECK(slurp(s.backup / "2026" / "09" / "IMG_0412.HEIC") == photo);
  CHECK(parts_under(s.backup) == 0);

  // The same file again: nothing is sent, and it says where it is.
  done = c->write(kRootC, "2026/09/IMG_0412.HEIC", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(done->existing);
  CHECK(done->name == "2026/09/IMG_0412.HEIC");

  // A different photo with that name, the same size or not: numbered, and the first untouched.
  const std::string other = marker(7, photo.size());
  done = c->write(kRootC, "2026/09/IMG_0412.HEIC", other.size(), hash_of(other), 0, reader_of(other), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(done->name == "2026/09/IMG_0412 (2).HEIC");
  CHECK(slurp(s.backup / "2026" / "09" / "IMG_0412.HEIC") == photo);
  CHECK(slurp(s.backup / "2026" / "09" / "IMG_0412 (2).HEIC") == other);
  // And that one again is found under its number.
  done = c->write(kRootC, "2026/09/IMG_0412.HEIC", other.size(), hash_of(other), 0, reader_of(other), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(done->existing);
  CHECK(done->name == "2026/09/IMG_0412 (2).HEIC");

  // An empty file is a file.
  done = c->write(kRootC, "empty.txt", 0, hash_of(""), 0, reader_of(std::string()), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(fs::is_regular_file(s.backup / "empty.txt"));
  CHECK(parts_under(s.backup) == 0);
}

TEST_CASE("serve backup: bytes that are not what was promised never become the file", "[driftwire-serve]") {
  service_fixture s;
  auto c = s.open_client();
  const std::string photo = marker(3, 300000);
  serve::client::write_refused why;
  auto done = c->write(kRootC, "bad.jpg", photo.size(), hash_of("something else"), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(done.error() == mv::status::corrupt);
  CHECK(why.why == serve::kWhyHash);
  CHECK_FALSE(fs::exists(s.backup / "bad.jpg"));
  CHECK(parts_under(s.backup) == 0);
  // The session goes on.
  done = c->write(kRootC, "bad.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE(done);
}

TEST_CASE("serve backup: a write left half way goes on from where it stopped", "[driftwire-serve]") {
  service_fixture s;
  const std::string clip = marker(11, 6 * 1024 * 1024 + 5);
  std::uint64_t stopped_at = 0;
  {
    auto c = s.open_client();
    std::atomic<bool> stop{false};
    serve::client::write_refused why;
    auto done = c->write(kRootC, "2026/09/IMG_0413.MOV", clip.size(), hash_of(clip), 0, reader_of(clip),
                         [&](std::uint64_t n) {
                           if (n >= 2 * 1024 * 1024) stop = true;
                         },
                         &stop, &why);
    REQUIRE_FALSE(done);
    CHECK(done.error() == mv::status::cancelled);
  }  // and the app was killed: the session ends with it
  CHECK_FALSE(fs::exists(s.backup / "2026" / "09" / "IMG_0413.MOV"));
  CHECK(eventually([&] { return parts_under(s.backup) == 1; }));

  auto c = s.open_client();
  serve::client::write_refused why;
  // From the start: told where its copy ends.
  auto done = c->write(kRootC, "2026/09/IMG_0413.MOV", clip.size(), hash_of(clip), 0, reader_of(clip), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyOffset);
  stopped_at = why.have;
  CHECK(stopped_at >= 2 * 1024 * 1024);
  CHECK(stopped_at < clip.size());
  done = c->write(kRootC, "2026/09/IMG_0413.MOV", clip.size(), hash_of(clip), stopped_at, reader_of(clip), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(slurp(s.backup / "2026" / "09" / "IMG_0413.MOV") == clip);
  CHECK(parts_under(s.backup) == 0);

  // Another device's unfinished copy of the same name is its own: never touched.
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second phone"));
  auto c2 = serve::client::open(s.connect(), s.client_options(second));
  REQUIRE(c2);
  std::atomic<bool> stop{false};
  done = (*c2)->write(kRootC, "x.mov", clip.size(), hash_of(clip), 0, reader_of(clip),
                      [&](std::uint64_t n) {
                        if (n >= 1024 * 1024) stop = true;
                      },
                      &stop, &why);
  REQUIRE_FALSE(done);
  done = c->write(kRootC, "x.mov", clip.size(), hash_of(clip), 0, reader_of(clip), {}, nullptr, &why);
  REQUIRE(done);  // this device starts its own from 0
  CHECK(parts_under(s.backup) == 1);  // the other's is still there
}

TEST_CASE("serve backup: the storage limit and the free-space reserve refuse a write", "[driftwire-serve]") {
  service_fixture s;
  s.srv.reset();
  s.backup_limit = 1024 * 1024;
  s.backup_reserve = 512 * 1024;
  write_file(s.backup / "old" / "already.jpg", marker(1, 300 * 1024));  // counted against the limit
  s.start(10000);
  auto c = s.open_client();

  auto space = c->stat(kRootC, "");
  REQUIRE(space);
  CHECK(space->has_space);
  CHECK(space->limit == 1024 * 1024);
  CHECK(space->used >= 300 * 1024);

  serve::client::write_refused why;
  const std::string big = marker(2, 800 * 1024);
  auto done = c->write(kRootC, "big.jpg", big.size(), hash_of(big), 0, reader_of(big), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(done.error() == mv::status::permission_denied);
  CHECK(why.why == serve::kWhyLimit);
  CHECK_FALSE(fs::exists(s.backup / "big.jpg"));
  CHECK(parts_under(s.backup) == 0);

  const std::string small = marker(3, 200 * 1024);
  done = c->write(kRootC, "small.jpg", small.size(), hash_of(small), 0, reader_of(small), {}, nullptr, &why);
  REQUIRE(done);

  // The NAS itself nearly full: the reserve stays free.
  s.free_bytes = 600 * 1024;
  const std::string tiny = marker(4, 150 * 1024);
  done = c->write(kRootC, "tiny.jpg", tiny.size(), hash_of(tiny), 0, reader_of(tiny), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyFull);
  space = c->stat(kRootC, "");
  REQUIRE(space);
  CHECK(space->room == 600 * 1024 - 512 * 1024);
}

TEST_CASE("serve backup: a folder with no limit takes more than any limit, and the reserve still refuses", "[driftwire-serve]") {
  service_fixture s;
  s.srv.reset();
  s.backup_limit = serve::kNoLimit;
  s.backup_reserve = 512 * 1024;
  s.free_bytes = 8 * 1024 * 1024;
  s.start(10000);
  auto c = s.open_client();

  auto space = c->stat(kRootC, "");
  REQUIRE(space);
  CHECK(serve::no_limit(space->limit));
  CHECK(space->room == 8 * 1024 * 1024 - 512 * 1024);  // only the reserve bounds it

  serve::client::write_refused why;
  for (int i = 0; i < 3; ++i) {  // 6 MB, past what a 1 MB limit would have taken
    const std::string clip = marker(20 + i, 2 * 1024 * 1024);
    REQUIRE(c->write(kRootC, "clip" + std::to_string(i) + ".mov", clip.size(), hash_of(clip), 0, reader_of(clip), {}, nullptr, &why));
  }
  // The volume itself nearly full: refused for the reserve, never for a limit.
  s.free_bytes = 1024 * 1024;
  const std::string more = marker(30, 600 * 1024);
  auto done = c->write(kRootC, "more.mov", more.size(), hash_of(more), 0, reader_of(more), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyFull);
  CHECK_FALSE(fs::exists(s.backup / "more.mov"));

  // Kept as none in serve.json, and none to a reader of it.
  serve::config cfg;
  cfg.port = 4000;
  serve::root_config r{kRootC, "Phone backup", "/volume1/backup"};
  r.backup = true;
  r.limit = serve::kNoLimit;
  r.reserve = 1ull << 30;
  cfg.roots = {r};
  auto back = serve::parse_config(serve::config_json(cfg));
  REQUIRE(back);
  CHECK(serve::no_limit(back->roots[0].limit));
}

TEST_CASE("serve backup: only a device that may write, and the meta folder only in a backup folder", "[driftwire-serve]") {
  service_fixture s;
  auto c = s.open_client();
  serve::client::write_refused why;
  const std::string photo = marker(5, 1000);
  // Since kChangeOps a "change" device adds to every folder it sees, not only backup folders.
  auto done = c->write(kRootA, "IMG.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE(done);
  CHECK(slurp(s.library / "IMG.jpg") == photo);
  // Never the Catalogue, and the backup meta folder only under a backup folder.
  done = c->write(kRootD, "IMG.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyNotAllowed);
  CHECK_FALSE(fs::exists(s.catalogue / "IMG.jpg"));
  const std::string me_id = serve::device_folder_id(public_of(s.device));
  done = c->write(kRootA, me_id + "/ledger.json", 2, hash_of("{}"), 0, reader_of(std::string("{}")), {}, nullptr, &why, true);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyNotAllowed);
  CHECK_FALSE(fs::exists(s.library / serve::kBackupMetaFolder));
  // Outside the folder: refused before any file is looked at.
  done = c->write(kRootC, "../escape.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK_FALSE(fs::exists(s.dir.root() / "escape.jpg"));
  done = c->write(kRootC, ".hidden.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  c.reset();

  // A client that did not ask for writes is never given them.
  auto o = s.client_options();
  o.ops = serve::kAllOps;
  auto reader = serve::client::open(s.connect(), o);
  REQUIRE(reader);
  CHECK(((*reader)->caps().ops & serve::kWriteOp) == 0);
  auto r = (*reader)->roots();
  REQUIRE(r);
  CHECK(std::none_of(r->begin(), r->end(), [](const serve::root_info& i) { return i.writable; }));
  reader->reset();

  // A device the grants say may not write sees the folder, not writable, and is refused.
  s.srv.reset();
  const key32 me = public_of(s.device);
  s.may_write = [me](const key32& k) { return k != me; };
  s.start(10000);
  c = s.open_client();
  r = c->roots();
  REQUIRE(r);
  const auto it = std::find_if(r->begin(), r->end(), [](const serve::root_info& i) { return i.id == kRootC; });
  REQUIRE(it != r->end());
  CHECK(it->backup);
  CHECK_FALSE(it->writable);
  done = c->write(kRootC, "IMG.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyNotAllowed);
  CHECK_FALSE(c->list(kRootC, "", nullptr, true));  // nor its ledgers
  // Nor an ordinary folder.
  done = c->write(kRootA, "other.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyNotAllowed);
  CHECK_FALSE(fs::exists(s.library / "other.jpg"));
}

TEST_CASE("serve backup: a device's ledger copy is the one file replaced, and only its own", "[driftwire-serve]") {
  service_fixture s;
  auto c = s.open_client();
  const std::string me = serve::device_folder_id(public_of(s.device));
  serve::client::write_refused why;
  const std::string v1 = R"({"library_id":"x","rows":[1]})";
  const std::string v2 = R"({"library_id":"x","rows":[1,2]})";
  auto done = c->write(kRootC, me + "/ledger.json", v1.size(), hash_of(v1), 0, reader_of(v1), {}, nullptr, &why, true);
  REQUIRE(done);
  done = c->write(kRootC, me + "/ledger.json", v2.size(), hash_of(v2), 0, reader_of(v2), {}, nullptr, &why, true);
  REQUIRE(done);
  CHECK(slurp(s.backup / serve::kBackupMetaFolder / me / "ledger.json") == v2);
  CHECK(parts_under(s.backup) == 0);

  // Not another device's, and nothing else in there.
  done = c->write(kRootC, "0123456789abcdef/ledger.json", v1.size(), hash_of(v1), 0, reader_of(v1), {}, nullptr, &why, true);
  REQUIRE_FALSE(done);
  CHECK(why.why == serve::kWhyNotAllowed);
  done = c->write(kRootC, me + "/other.json", v1.size(), hash_of(v1), 0, reader_of(v1), {}, nullptr, &why, true);
  REQUIRE_FALSE(done);

  // A new phone finds it: listed and read under the meta folder.
  const key32 next = random_key();
  REQUIRE(s.pair(next, "New phone"));
  auto c2 = serve::client::open(s.connect(), s.client_options(next));
  REQUIRE(c2);
  auto listing = (*c2)->list(kRootC, "", nullptr, true);
  REQUIRE(listing);
  REQUIRE(listing->size() == 1);
  CHECK(listing->front().name == me);
  auto stream = (*c2)->read_range(kRootC, me + "/ledger.json", 0, UINT64_MAX, 1024 * 1024, false, true);
  REQUIRE(stream);
  CHECK(read_all(**stream) == v2);
  // The meta folder is not a backup folder's ordinary content.
  auto plain = (*c2)->list(kRootC, "");
  REQUIRE(plain);
  CHECK(std::none_of(plain->begin(), plain->end(), [](const serve::list_entry& e) { return e.name.starts_with("."); }));
}

TEST_CASE("serve backup: the owner adds a backup folder and changes its limit", "[driftwire-serve]") {
  service_fixture s;
  // Through the wire messages only: the owner's admin verbs are tested with claiming elsewhere;
  // here the config they write must round-trip.
  serve::config c;
  c.port = 4000;
  c.name = "NAS";
  serve::root_config r{kRootC, "Phone backup", "/volume1/backup"};
  r.backup = true;
  r.limit = 500ull << 30;
  r.reserve = 1ull << 30;
  c.roots = {{kRootA, "Photos", "/volume1/photo"}, r};
  auto back = serve::parse_config(serve::config_json(c));
  REQUIRE(back);
  REQUIRE(back->roots.size() == 2);
  CHECK_FALSE(back->roots[0].backup);
  CHECK(back->roots[1].backup);
  CHECK(back->roots[1].limit == r.limit);
  CHECK(back->roots[1].reserve == r.reserve);
  // A config from before 1.1 has none.
  auto old = serve::parse_config(R"({"port":4000,"roots":[{"id":"a1a1a1a1a1a1a1a1","label":"P","path":"/v"}]})");
  REQUIRE(old);
  CHECK_FALSE(old->roots[0].backup);
}

TEST_CASE("serve backup: one writer per unfinished copy, long names, and an orphaned copy goes", "[driftwire-serve]") {
  service_fixture s;
  const std::string clip = marker(21, 2 * 1024 * 1024);
  // A write held half way (its reader waits), and the same device on a second session.
  std::atomic<bool> release{false}, started{false};
  auto c1 = s.open_client();
  serve::client::read_fn slow = [&](std::uint64_t at, std::span<std::uint8_t> into) -> mv::result<std::size_t> {
    if (at >= 512 * 1024) {
      started = true;
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return reader_of(clip)(at, into);
  };
  std::thread t([&] {
    serve::client::write_refused why;
    (void)c1->write(kRootC, "held.mov", clip.size(), hash_of(clip), 0, slow, {}, nullptr, &why);
  });
  REQUIRE(eventually([&] { return started.load(); }));
  auto c2 = s.open_client();
  serve::client::write_refused why;
  auto busy = c2->write(kRootC, "held.mov", clip.size(), hash_of(clip), 0, reader_of(clip), {}, nullptr, &why);
  REQUIRE_FALSE(busy);
  CHECK(why.why == serve::kWhyBusy);
  release = true;
  t.join();
  CHECK(slurp(s.backup / "held.mov") == clip);

  // A name too long to keep beside its unfinished copy is refused up front.
  const std::string long_name(serve::kMaxWriteName + 1, 'n');
  auto too_long = c2->write(kRootC, long_name, 10, hash_of("0123456789"), 0, reader_of(std::string("0123456789")), {}, nullptr, &why);
  REQUIRE_FALSE(too_long);
  CHECK(why.why == serve::kWhyName);

  // An unfinished copy of a file that is now there whole (another phone's, the same bytes): its own copy goes.
  std::atomic<bool> stop{false};
  auto left = c2->write(kRootC, "twin.mov", clip.size(), hash_of(clip), 0, reader_of(clip),
                        [&](std::uint64_t n) {
                          if (n >= 1024 * 1024) stop = true;
                        },
                        &stop, &why);
  REQUIRE_FALSE(left);
  REQUIRE(eventually([&] { return parts_under(s.backup) == 1; }));
  write_file(s.backup / "twin.mov", clip);
  auto found = c2->write(kRootC, "twin.mov", clip.size(), hash_of(clip), 0, reader_of(clip), {}, nullptr, &why);
  REQUIRE(found);
  CHECK(found->existing);
  CHECK(parts_under(s.backup) == 0);
}

TEST_CASE("serve backup: two phones placing one name at once keep both files", "[driftwire-serve]") {
  service_fixture s;
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second phone"));
  auto a = s.open_client();
  auto b = serve::client::open(s.connect(), s.client_options(second));
  REQUIRE(b);
  for (int round = 0; round < 10; ++round) {
    const std::string name = "same" + std::to_string(round) + ".jpg";
    const std::string one = marker(100 + round, 200 * 1024);
    const std::string two = marker(200 + round, 200 * 1024);
    mv::result<serve::client::write_done> ra = mv::err(mv::status::io), rb = mv::err(mv::status::io);
    std::thread ta([&] {
      serve::client::write_refused w;
      ra = a->write(kRootC, name, one.size(), hash_of(one), 0, reader_of(one), {}, nullptr, &w);
    });
    std::thread tb([&] {
      serve::client::write_refused w;
      rb = (*b)->write(kRootC, name, two.size(), hash_of(two), 0, reader_of(two), {}, nullptr, &w);
    });
    ta.join();
    tb.join();
    REQUIRE(ra);
    REQUIRE(rb);
    CHECK(ra->name != rb->name);
    CHECK(slurp(s.backup / ra->name) == one);
    CHECK(slurp(s.backup / rb->name) == two);
  }
}

TEST_CASE("serve: every session hears the server's drives' health in its caps", "[driftwire-serve][host-drives]") {
  service_fixture s;
  CHECK(s.open_client()->caps().storage.empty());  // no drives keeper: nothing said
  s.srv.reset();
  std::string word = "degraded";
  std::mutex m;
  s.storage = [&] {
    std::lock_guard g(m);
    return word;
  };
  s.start(10000);
  CHECK(s.open_client()->caps().storage == "degraded");
  {
    std::lock_guard g(m);
    word.clear();  // healthy again
  }
  CHECK(s.open_client()->caps().storage.empty());
  {
    std::lock_guard g(m);
    word = "not a word";  // never sent
  }
  CHECK(s.open_client()->caps().storage.empty());
}

TEST_CASE("serve: the devices with a session up now (the tray names them)", "[driftwire-serve][host-tray]") {
  // docs/plans/self-hosted.md "Tray": the host's menu names the phones connected.
  service_fixture s;
  CHECK(s.srv->connected_devices().empty());
  auto c = s.open_client();
  REQUIRE(c->roots());
  key32 pub;
  crypto_scalarmult_base(pub.data(), s.device.data());
  REQUIRE(eventually([&] { return s.srv->connected_devices().size() == 1; }));
  CHECK(s.srv->connected_devices().front() == pub);
  // Its file bytes, counted per device, with how and since when.
  write_file(s.library / "u.bin", marker(0, 50000));
  auto r = c->read_range(kRootA, "u.bin", 0, 50000);
  REQUIRE(r);
  CHECK(read_all(**r).size() == 50000);
  auto u = s.srv->usage();
  REQUIRE(u.size() == 1);
  CHECK(u[0].device == pub);
  CHECK(u[0].read == 50000);
  CHECK(u[0].written == 0);
  CHECK(u[0].connected);
  CHECK(u[0].since_unix > 0);
  CHECK(u[0].via != serve::server::usage_via::relay);
  c->close();
  CHECK(eventually([&] { return s.srv->connected_devices().empty(); }));
  // Ended, its bytes stay counted (once the session is reaped).
  CHECK(eventually([&] {
    const auto after = s.srv->usage();
    return after.size() == 1 && after[0].read == 50000 && !after[0].connected;
  }));
}

// ---- convert (docs/plans/phone-clip-convert.md) ------------------------------------------------

namespace {

namespace cv = serve::convert;

// A runner that converts nothing: the plan is 40 s in 4 s segments; a segment is its number and
// the source's first bytes. `gate`, when set, holds every run until opened.
struct fake_runner {
  std::atomic<int> plans{0}, segments{0};
  std::mutex m;
  std::condition_variable c;
  bool held = false;
  std::vector<std::uint32_t> order;  // segments, in the order they ran
  bool not_a_clip = false;
  std::atomic<bool> fail{false};

  void hold() {
    std::lock_guard g(m);
    held = true;
  }
  void open() {
    {
      std::lock_guard g(m);
      held = false;
    }
    c.notify_all();
  }
  void wait_open() {
    std::unique_lock g(m);
    c.wait(g, [&] { return !held; });
  }

  cv::runner make() {
    cv::runner r;
    r.plan = [this](const std::string& src, std::uint32_t seg_ms, const std::string& out) {
      wait_open();
      ++plans;
      if (not_a_clip) return cv::ran::not_a_clip;
      serve::convert_plan p{40000, seg_ms, (40000 + seg_ms - 1) / seg_ms, 1280, 720};
      std::ofstream(fs::path(std::u8string(reinterpret_cast<const char8_t*>(out.data()), out.size())), std::ios::binary)
          << serve::convert_plan_json(p);
      (void)src;
      return cv::ran::ok;
    };
    r.segment = [this](const std::string& src, std::uint32_t k, std::uint32_t, const std::string& out) {
      wait_open();
      ++segments;
      {
        std::lock_guard g(m);
        order.push_back(k);
      }
      if (fail) return cv::ran::failed;
      std::ifstream in(fs::path(std::u8string(reinterpret_cast<const char8_t*>(src.data()), src.size())), std::ios::binary);
      std::string head(8, '\0');
      in.read(head.data(), 8);
      head.resize(static_cast<std::size_t>(in.gcount()));
      std::ofstream(fs::path(std::u8string(reinterpret_cast<const char8_t*>(out.data()), out.size())), std::ios::binary)
          << "SEG" << k << ":" << head;
      return cv::ran::ok;
    };
    return r;
  }
};

struct waited_plan {
  std::mutex m;
  std::condition_variable c;
  std::optional<std::pair<cv::outcome, serve::convert_plan>> got;
  cv::service::plan_cb cb() {
    return [this](cv::outcome o, const serve::convert_plan& p) {
      std::lock_guard g(m);
      got = {o, p};
      c.notify_all();
    };
  }
  std::pair<cv::outcome, serve::convert_plan> wait() {
    std::unique_lock g(m);
    REQUIRE(c.wait_for(g, std::chrono::seconds(kSlowSeconds), [&] { return got.has_value(); }));
    return *got;
  }
};

struct waited_segment {
  std::mutex m;
  std::condition_variable c;
  std::optional<std::pair<cv::outcome, std::string>> got;
  cv::service::segment_cb cb() {
    return [this](cv::outcome o, const std::string& f) {
      std::lock_guard g(m);
      got = {o, f};
      c.notify_all();
    };
  }
  std::pair<cv::outcome, std::string> wait() {
    std::unique_lock g(m);
    REQUIRE(c.wait_for(g, std::chrono::seconds(kSlowSeconds), [&] { return got.has_value(); }));
    return *got;
  }
};

std::string file_text(const std::string& path) {
  std::ifstream f(fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Until the service has nothing running or queued (bounded).
void settle(cv::service& s, fake_runner& r, int segments) {
  for (int i = 0; i < kSlowSeconds * 100 && (r.segments.load() < segments || s.now().busy); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace

TEST_CASE("serve convert: the request and the plan on the wire", "[driftwire-serve][convert]") {
  serve::open_request r;
  r.id = 7;
  r.kind = serve::op::convert;
  r.root = kRootA;
  r.rel = "clips/a.avi";
  r.segment = 3;
  r.credit = 65536;
  bool unknown = true;
  auto back = serve::parse_open(serve::encode_open(r), unknown);
  REQUIRE(back);
  CHECK_FALSE(unknown);
  CHECK(back->kind == serve::op::convert);
  CHECK(back->segment == 3);
  CHECK(back->rel == "clips/a.avi");
  r.segment = -1;
  back = serve::parse_open(serve::encode_open(r), unknown);
  REQUIRE(back);
  CHECK(back->segment == -1);

  auto raw = [](std::string_view json) {
    std::vector<std::uint8_t> b{1, 0, 0, 0};
    b.insert(b.end(), json.begin(), json.end());
    return b;
  };
  CHECK_FALSE(serve::parse_open(raw(R"({"op":"convert","root":"a1a1a1a1a1a1a1a1","rel":"a.avi"})"), unknown));  // no seg
  CHECK_FALSE(serve::parse_open(raw(R"({"op":"convert","root":"a1a1a1a1a1a1a1a1","rel":"a.avi","seg":-2})"), unknown));
  CHECK_FALSE(serve::parse_open(raw(R"({"op":"convert","root":"a1a1a1a1a1a1a1a1","seg":0})"), unknown));  // no file
  CHECK(serve::parse_op("convert") == serve::op::convert);
  CHECK(serve::op_bit(serve::op::convert) == serve::kConvertOp);
  CHECK((serve::kAllOps & serve::kConvertOp) == 0);  // never granted with the reads

  serve::convert_plan p{10001, 4000, 3, 1920, 1080};
  const auto parsed = serve::parse_convert_plan(serve::convert_plan_json(p));
  REQUIRE(parsed);
  CHECK(parsed->duration_ms == 10001);
  CHECK(parsed->segments == 3);
  CHECK(parsed->width == 1920);
  CHECK_FALSE(serve::parse_convert_plan(R"({"duration_ms":10001,"segment_ms":4000,"segments":4,"width":1,"height":1})"));
  CHECK(serve::parse_convert_plan(R"({"duration_ms":8100,"segment_ms":4000,"segments":2,"width":1,"height":1})"));  // a 100 ms tail joined
  CHECK_FALSE(serve::parse_convert_plan(R"({"duration_ms":8100,"segment_ms":4000,"segments":1,"width":1,"height":1})"));
  CHECK_FALSE(serve::parse_convert_plan(R"({"duration_ms":0,"segment_ms":4000,"segments":0,"width":1,"height":1})"));
  CHECK_FALSE(serve::parse_convert_plan("[]"));
}

TEST_CASE("serve convert: a segment is made once, kept, and the next ones are made ahead", "[driftwire-serve][convert]") {
  scratch_dir dir{"serve_convert_cache"};
  write_file(dir.root() / "clip.avi", "RIFFclipbytes");
  fake_runner run;
  cv::service::options o;
  o.cache_dir = utf8(dir.root() / "cache");
  o.run = run.make();
  auto svc = cv::service::start(o);
  cv::source src{utf8(dir.root() / "clip.avi"), kRootA, "clip.avi", 13, 1000};

  waited_plan p1;
  svc->plan(src, p1.cb());
  const auto [po, plan] = p1.wait();
  REQUIRE(po == cv::outcome::ok);
  CHECK(plan.segments == 10);
  waited_plan p2;
  svc->plan(src, p2.cb());
  CHECK(p2.wait().first == cv::outcome::ok);
  CHECK(run.plans == 1);  // the second from the cache

  // Segment 0 alone (a poster): nothing made ahead.
  waited_segment s0;
  svc->segment(src, 0, s0.cb());
  const auto [so, file] = s0.wait();
  REQUIRE(so == cv::outcome::ok);
  CHECK(file_text(file) == "SEG0:RIFFclip");
  settle(*svc, run, 1);
  CHECK(run.segments == 1);
  // Segment 1 (playing): 2 and 3 follow by themselves, and are then hits.
  waited_segment s1;
  svc->segment(src, 1, s1.cb());
  CHECK(s1.wait().first == cv::outcome::ok);
  settle(*svc, run, 4);
  CHECK(run.segments == 4);
  waited_segment s3;
  svc->segment(src, 3, s3.cb());
  CHECK(s3.wait().first == cv::outcome::ok);
  settle(*svc, run, 6);  // 3 asked: 4 and 5 ahead
  CHECK(run.segments == 6);
  CHECK(svc->now().hits >= 2);
  // Nothing beside the original, and nothing named after it in the cache.
  CHECK(fs::exists(dir.root() / "clip.avi"));
  int beside = 0;
  for (const auto& e : fs::directory_iterator(dir.root())) beside += e.is_regular_file() ? 1 : 0;
  CHECK(beside == 1);
  for (const auto& e : fs::recursive_directory_iterator(dir.root() / "cache")) {
    CHECK(utf8(e.path().filename()).find("clip") == std::string::npos);
  }

  // The file changed (another mtime): converted again.
  cv::source changed = src;
  changed.mtime = 2000;
  CHECK(cv::cache_key(changed) != cv::cache_key(src));
  const int before = run.segments;
  waited_segment s0b;
  svc->segment(changed, 0, s0b.cb());
  CHECK(s0b.wait().first == cv::outcome::ok);
  CHECK(run.segments == before + 1);
}

TEST_CASE("serve convert: what a phone asks goes first, and a seek drops what was queued ahead", "[driftwire-serve][convert]") {
  scratch_dir dir{"serve_convert_order"};
  write_file(dir.root() / "a.mkv", "matroska");
  write_file(dir.root() / "b.mkv", "matroskb");
  fake_runner run;
  cv::service::options o;
  o.cache_dir = utf8(dir.root() / "cache");
  o.run = run.make();
  o.ahead = 2;
  auto svc = cv::service::start(o);
  cv::source src{utf8(dir.root() / "a.mkv"), kRootA, "a.mkv", 8, 1};
  waited_plan p;
  svc->plan(src, p.cb());
  REQUIRE(p.wait().first == cv::outcome::ok);  // 10 segments

  run.hold();
  waited_segment s1, s6;
  svc->segment(src, 1, s1.cb());  // runs (held); 2 and 3 queued ahead
  for (int i = 0; i < 1000 && !svc->now().busy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  svc->segment(src, 6, s6.cb());  // a seek: asked; 2 and 3 dropped, 7 and 8 ahead
  run.open();
  CHECK(s1.wait().first == cv::outcome::ok);
  CHECK(s6.wait().first == cv::outcome::ok);
  settle(*svc, run, 4);
  {
    std::lock_guard g(run.m);
    REQUIRE(run.order.size() == 4);
    CHECK(run.order == std::vector<std::uint32_t>{1, 6, 7, 8});
  }

  // Another clip: what was ahead for the first goes.
  cv::source other{utf8(dir.root() / "b.mkv"), kRootA, "b.mkv", 8, 1};
  waited_plan p2;
  svc->plan(other, p2.cb());
  REQUIRE(p2.wait().first == cv::outcome::ok);
  run.hold();
  waited_segment a2, b5;
  svc->segment(src, 2, a2.cb());  // runs (held); 3 and 4 of a ahead
  for (int i = 0; i < 1000 && !svc->now().busy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  svc->segment(other, 5, b5.cb());
  run.open();
  CHECK(a2.wait().first == cv::outcome::ok);
  CHECK(b5.wait().first == cv::outcome::ok);
  settle(*svc, run, 8);
  std::lock_guard g(run.m);
  REQUIRE(run.order.size() == 8);
  CHECK(std::vector<std::uint32_t>(run.order.begin() + 4, run.order.end()) == std::vector<std::uint32_t>{2, 5, 6, 7});
}

TEST_CASE("serve convert: the cache keeps to its bound, failures are not kept, purge empties it", "[driftwire-serve][convert]") {
  scratch_dir dir{"serve_convert_bound"};
  write_file(dir.root() / "a.mpg", "mpegbytes");
  write_file(dir.root() / "b.mpg", "otherbyt");
  fake_runner run;
  cv::service::options o;
  o.cache_dir = utf8(dir.root() / "cache");
  o.run = run.make();
  o.ahead = 0;
  o.max_bytes = 200;  // plans (~80 B each) and a few 13-byte segments
  auto svc = cv::service::start(o);
  cv::source a{utf8(dir.root() / "a.mpg"), kRootA, "a.mpg", 9, 1};
  for (std::uint32_t k = 0; k < 3; ++k) {
    for (int file = 0; file < 4; ++file) {
      cv::source s = a;
      s.mtime = file;  // four sources' worth of segments
      waited_segment w;
      svc->segment(s, k, w.cb());
      REQUIRE(w.wait().first == cv::outcome::ok);
    }
  }
  CHECK(svc->now().cached_bytes <= 200);

  run.fail = true;
  cv::source b{utf8(dir.root() / "b.mpg"), kRootA, "b.mpg", 8, 1};
  waited_segment bad;
  svc->segment(b, 0, bad.cb());
  CHECK(bad.wait().first == cv::outcome::failed);
  run.fail = false;
  waited_segment again;
  svc->segment(b, 0, again.cb());
  CHECK(again.wait().first == cv::outcome::ok);  // tried again, not remembered

  run.not_a_clip = true;
  cv::source c = b;
  c.rel = "c.mpg";
  waited_plan np;
  svc->plan(c, np.cb());
  CHECK(np.wait().first == cv::outcome::not_a_clip);
  const int plans = run.plans;
  waited_plan np2;
  svc->plan(c, np2.cb());
  CHECK(np2.wait().first == cv::outcome::not_a_clip);
  CHECK(run.plans == plans);  // remembered: not run again

  svc->purge();
  CHECK(svc->now().cached_bytes == 0);
  CHECK(fs::is_empty(dir.root() / "cache"));
}

TEST_CASE("serve convert: the op is offered only with a converter, and streams a segment", "[driftwire-serve][convert]") {
  service_fixture s;
  write_file(s.library / "clips" / "old.avi", "RIFFavidata");
  fs::create_directories(s.library / "clips" / "folder.avi");
  write_file(s.outside / "secret.avi", "nope");

  // No converter: not in the caps, and the client never asks.
  {
    auto opts = s.client_options();
    opts.ops |= serve::kConvertOp;
    auto c = serve::client::open(s.connect(), opts);
    REQUIRE(c);
    CHECK(((*c)->caps().ops & serve::kConvertOp) == 0);
    CHECK((*c)->convert_plan_of(kRootA, "clips/old.avi").error() == mv::status::unsupported_format);
  }

  fake_runner run;
  scratch_dir cache{"serve_convert_server_cache"};
  cv::service::options o;
  o.cache_dir = utf8(cache.root());
  o.run = run.make();
  s.srv->set_converter(cv::service::start(o));

  // A phone from before `convert` does not list it, and is not given it.
  CHECK((s.open_client()->caps().ops & serve::kConvertOp) == 0);

  auto opts = s.client_options();
  opts.ops |= serve::kConvertOp;
  auto c = serve::client::open(s.connect(), opts);
  REQUIRE(c);
  REQUIRE(((*c)->caps().ops & serve::kConvertOp) != 0);
  auto plan = (*c)->convert_plan_of(kRootA, "clips/old.avi");
  REQUIRE(plan);
  CHECK(plan->segments == 10);
  CHECK(plan->width == 1280);
  auto seg = (*c)->convert_segment(kRootA, "clips/old.avi", 1, 4096);
  REQUIRE(seg);
  CHECK(read_all(**seg) == "SEG1:RIFFavid");
  CHECK((*seg)->code() == serve::end_code::ok);

  // Outside the folders, a folder, and a link out are refused, never converted.
  const int before = run.segments;
  CHECK_FALSE((*c)->convert_plan_of(kRootA, "../outside/secret.avi"));
  CHECK((*c)->convert_plan_of(kRootA, "clips/folder.avi").error() == mv::status::permission_denied);
#if !defined(_WIN32)
  fs::create_symlink(s.outside / "secret.avi", s.library / "clips" / "link.avi");
  CHECK_FALSE((*c)->convert_plan_of(kRootA, "clips/link.avi"));
#endif
  CHECK((*c)->convert_plan_of(kRootA, "clips/missing.avi").error() == mv::status::not_found);

  // Turned off since the caps: said so.
  s.srv->set_converter(nullptr);
  std::string why;
  CHECK((*c)->convert_plan_of(kRootA, "clips/old.avi", &why).error() == mv::status::unsupported_format);
  CHECK(why == serve::kWhyConvertOff);
  CHECK(run.segments >= before);
}

TEST_CASE("serve convert: a stream cancelled while it converts ends, and the session goes on", "[driftwire-serve][convert]") {
  service_fixture s;
  write_file(s.library / "b.webm", "webmwebm");
  fake_runner run;
  scratch_dir cache{"serve_convert_cancel_cache"};
  cv::service::options o;
  o.cache_dir = utf8(cache.root());
  o.run = run.make();
  o.ahead = 0;
  s.srv->set_converter(cv::service::start(o));
  auto opts = s.client_options();
  opts.ops |= serve::kConvertOp;
  auto c = serve::client::open(s.connect(), opts);
  REQUIRE(c);
  REQUIRE((*c)->convert_plan_of(kRootA, "b.webm"));
  run.hold();
  auto seg = (*c)->convert_segment(kRootA, "b.webm", 0);
  REQUIRE(seg);
  (*seg)->cancel();
  run.open();
  // The session still answers, and the segment was kept for the next ask.
  auto again = (*c)->convert_segment(kRootA, "b.webm", 0);
  REQUIRE(again);
  CHECK(read_all(**again) == "SEG0:webmwebm");
  CHECK((*c)->stat(kRootA, "b.webm"));
}

// docs/plans/phone-clip-convert.md, measured: the real clip helper behind the real service over
// loopback. MV_CONVERT_BENCH_HELPER is MediaViewerClipJob, MV_CONVERT_BENCH_DIR a folder of clips.
// Per clip: the first frame cold (plan, then segment 0), a seek to the middle cold, and segment 0
// again from the cache. Hidden: it needs a hardware encoder and the files.
#include "addons/driftwire/cli/convert_run.h"
TEST_CASE("serve convert: first frame and seek times with the real helper", "[.convert-bench]") {
  const char* helper = std::getenv("MV_CONVERT_BENCH_HELPER");
  const char* clips = std::getenv("MV_CONVERT_BENCH_DIR");
  if (!helper || !clips) SKIP("set MV_CONVERT_BENCH_HELPER and MV_CONVERT_BENCH_DIR");
  service_fixture s;
  for (const auto& e : fs::directory_iterator(clips)) {
    if (e.is_regular_file()) fs::copy_file(e.path(), s.library / e.path().filename());
  }
  scratch_dir cache{"serve_convert_bench_cache"};
  auto conv = mv::driftwire::cli::start_converter(helper, utf8(cache.root()));
  REQUIRE(conv);
  s.srv->set_converter(conv);
  auto opts = s.client_options();
  opts.ops |= serve::kConvertOp;
  auto c = serve::client::open(s.connect(), opts);
  REQUIRE(c);
  using ms = std::chrono::duration<double, std::milli>;
  for (const auto& e : fs::directory_iterator(s.library)) {
    if (!e.is_regular_file()) continue;
    const std::string name = utf8(e.path().filename());
    const auto t0 = std::chrono::steady_clock::now();
    auto plan = (*c)->convert_plan_of(kRootA, name);
    if (!plan) continue;
    const auto t1 = std::chrono::steady_clock::now();
    auto seg0 = (*c)->convert_segment(kRootA, name, 0);
    REQUIRE(seg0);
    const auto bytes0 = read_all(**seg0).size();
    const auto t2 = std::chrono::steady_clock::now();
    const std::uint32_t mid = plan->segments / 2;
    auto segm = (*c)->convert_segment(kRootA, name, mid);
    REQUIRE(segm);
    (void)read_all(**segm);
    const auto t3 = std::chrono::steady_clock::now();
    auto again = (*c)->convert_segment(kRootA, name, 0);
    REQUIRE(again);
    (void)read_all(**again);
    const auto t4 = std::chrono::steady_clock::now();
    std::printf("{\"case\":\"convert_bench\",\"file\":\"%s\",\"plan_ms\":%.0f,\"first_frame_ms\":%.0f,\"seek_ms\":%.0f,"
                "\"cached_ms\":%.0f,\"seg0_bytes\":%zu,\"segments\":%u}\n",
                name.c_str(), ms(t1 - t0).count(), ms(t2 - t0).count(), ms(t3 - t2).count(), ms(t4 - t3).count(),
                bytes0, plan->segments);
  }
}

namespace {

// Bench only: the client's writes (its credit, its requests) reach the service
// `rtt_ms` late, so a stream's window has a round trip to cover, as on Wi-Fi.
class late_link final : public sp::link {
 public:
  late_link(std::shared_ptr<sp::link> inner, int rtt_ms) : inner_(std::move(inner)), rtt_(rtt_ms) {
    thread_ = std::thread([this] { run(); });
  }
  ~late_link() override {
    {
      std::lock_guard l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  mv::expected write_all(std::span<const std::uint8_t> bytes) override {
    std::lock_guard l(m_);
    q_.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(rtt_), {bytes.begin(), bytes.end()}});
    cv_.notify_all();
    return {};
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  sp::transport via() const noexcept override { return inner_->via(); }

 private:
  struct item {
    std::chrono::steady_clock::time_point due;
    std::vector<std::uint8_t> bytes;
  };
  void run() {
    std::unique_lock l(m_);
    for (;;) {
      cv_.wait(l, [&] { return stop_ || !q_.empty(); });
      if (stop_) return;
      const auto due = q_.front().due;
      if (cv_.wait_until(l, due, [&] { return stop_; })) return;
      item it = std::move(q_.front());
      q_.pop_front();
      l.unlock();
      (void)inner_->write_all(it.bytes);
      l.lock();
    }
  }
  std::shared_ptr<sp::link> inner_;
  int rtt_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<item> q_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace

TEST_CASE("serve bench: a whole-file read's speed by window and round trip", "[.serve-bench]") {
  service_fixture s;
  const std::uint64_t size = 256ull << 20;
  {
    std::ofstream f(s.library / "clip.mov", std::ios::binary);
    std::vector<char> block(1 << 20, 7);
    for (std::uint64_t i = 0; i < size; i += block.size()) f.write(block.data(), static_cast<std::streamsize>(block.size()));
  }
  for (const int rtt : {0, 5, 20, 50}) {
    for (const std::uint32_t window : {serve::kMaxStreamCredit / 8, serve::kMaxStreamCredit}) {
      auto c = serve::client::open(std::make_shared<late_link>(s.connect(), rtt), s.client_options());
      REQUIRE(c);
      const auto t0 = std::chrono::steady_clock::now();
      auto r = (*c)->read_range(kRootA, "clip.mov", 0, UINT64_MAX, window, true);
      REQUIRE(r);
      std::uint64_t got = 0;
      for (;;) {
        auto n = (*r)->next(10000);
        REQUIRE(n);
        if (!*n) break;
        got += (*n)->bytes.size();
      }
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      CHECK(got == size);
      std::printf("rtt %3d ms  window %5u KB  %7.1f MB/s\n", rtt, window / 1024, size / 1e6 / sec);
      (*c)->close();
    }
  }
}

TEST_CASE("serve: caps carry the service's addresses, each checked on its own", "[driftwire-serve]") {
  // A service from before addresses: none, and nothing else changes.
  const auto old = serve::parse_caps(serve::caps_json(1, serve::kAllOps));
  REQUIRE(old);
  CHECK(old->local.empty());
  CHECK(old->public_name.empty());
  CHECK(old->relay.empty());
  const auto c = serve::parse_caps(serve::caps_json(
      1, serve::kAllOps,
      R"("addresses":{"local":"192.168.1.20:7878","public":"home.duckdns.org:7878","relay":"[2001:db8::1]:7879"})"));
  REQUIRE(c);
  CHECK(c->local == "192.168.1.20:7878");
  CHECK(c->public_name == "home.duckdns.org:7878");
  CHECK(c->relay == "[2001:db8::1]:7879");
  // One that is not an address is dropped; the others stay, and the caps still parse.
  const auto odd = serve::parse_caps(serve::caps_json(
      1, serve::kAllOps, R"("addresses":{"local":"no port","public":"home.duckdns.org:7878","relay":5})"));
  REQUIRE(odd);
  CHECK(odd->local.empty());
  CHECK(odd->public_name == "home.duckdns.org:7878");
  CHECK(odd->relay.empty());
  const auto too_long = serve::parse_caps(serve::caps_json(
      1, serve::kAllOps, R"("addresses":{"public":")" + std::string(300, 'a') + R"(:1"})"));
  REQUIRE(too_long);
  CHECK(too_long->public_name.empty());
}

TEST_CASE("serve: serve.json keeps the addresses, and one from before has none", "[driftwire-serve]") {
  serve::config c;
  c.port = 7878;
  c.roots = {{"0123456789abcdef", "Photos", "/photos"}};
  c.local = "192.168.1.20:7878";
  c.public_name = "home.duckdns.org:7878";
  const auto back = serve::parse_config(serve::config_json(c));
  REQUIRE(back);
  CHECK(back->local == c.local);
  CHECK(back->public_name == c.public_name);
  const auto before = serve::parse_config(R"({"port":1,"roots":[{"id":"0123456789abcdef","label":"a","path":"/a"}]})");
  REQUIRE(before);
  CHECK(before->local.empty());
  CHECK(before->public_name.empty());
  CHECK(serve::config_json(before.value()).find("public") == std::string::npos);
}

TEST_CASE("serve: serve.json keeps the owner's phone lock, and caps carry it", "[driftwire-serve]") {
  serve::config c;
  c.port = 7878;
  c.roots = {{"0123456789abcdef", "Photos", "/photos"}};
  CHECK(serve::config_json(c).find("phone_lock") == std::string::npos);  // off: left out, as before it existed
  c.phone_lock = true;
  const auto back = serve::parse_config(serve::config_json(c));
  REQUIRE(back);
  CHECK(back->phone_lock);
  CHECK_FALSE(serve::parse_config(R"({"port":1,"roots":[{"id":"0123456789abcdef","label":"a","path":"/a"}],"phone_lock":1})"));
  const auto locked = serve::parse_caps(serve::caps_json(1, serve::kAllOps, R"("lock":true)"));
  REQUIRE(locked);
  CHECK(locked->lock);
  const auto old = serve::parse_caps(serve::caps_json(1, serve::kAllOps));
  REQUIRE(old);
  CHECK_FALSE(old->lock);  // a service from before it: no lock asked
  const auto odd = serve::parse_caps(serve::caps_json(1, serve::kAllOps, R"("lock":"yes")"));
  REQUIRE(odd);
  CHECK_FALSE(odd->lock);
}

// ---- changing the shared folders (serve_wire.h "changing", kChangeOps) --------------------

namespace {

using why_t = serve::client::change_why;

// A device on the grants list (a limited one): `backup` off is role "view".
void grant_device(service_fixture& s, const key32& private_key, bool backup,
                  std::optional<std::vector<std::string>> roots = std::nullopt) {
  std::vector<mv::driftwire::serve::grants::grant> list;
  REQUIRE(mv::driftwire::serve::grants::load(*s.store, list));
  mv::driftwire::serve::grants::grant g;
  g.name = "Limited";
  g.key = public_of(private_key);
  g.backup = backup;
  g.roots = std::move(roots);
  g.created = 1;
  list.push_back(g);
  REQUIRE(mv::driftwire::serve::grants::save(*s.store, list));
}

std::unique_ptr<serve::client> open_as(service_fixture& s, const key32& private_key) {
  auto c = serve::client::open(s.connect(), s.client_options(private_key));
  REQUIRE(c);
  return std::move(*c);
}

const serve::root_info* root_of(const std::vector<serve::root_info>& roots, const std::string& id) {
  const auto it = std::find_if(roots.begin(), roots.end(), [&](const serve::root_info& r) { return r.id == id; });
  return it == roots.end() ? nullptr : &*it;
}

bool listed(serve::client& c, const std::string& root, const std::string& rel, const std::string& name) {
  auto l = c.list(root, rel);
  REQUIRE(l);
  return std::any_of(l->begin(), l->end(), [&](const serve::list_entry& e) { return e.name == name; });
}

int leftovers_under(const fs::path& root) {
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    const std::string name = e.path().filename().string();
    if (name.ends_with(".mvpart") || name.ends_with(".mvmove")) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("serve change: a change device makes folders, adds, moves and deletes in a shared folder",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  auto c = s.open_client();
  CHECK(c->caps().role == "change");
  CHECK((c->caps().ops & serve::kChangeOps) == serve::kChangeOps);
  CHECK(c->changes_files());
  auto roots = c->roots();
  REQUIRE(roots);
  CHECK(root_of(*roots, kRootA)->writable);
  CHECK_FALSE(root_of(*roots, kRootA)->backup);
  CHECK(root_of(*roots, kRootC)->writable);
  CHECK_FALSE(root_of(*roots, kRootD)->writable);  // the Catalogue never

  serve::client::change_refused why;
  auto made = c->make_folder(kRootA, "Trips/2026", &why);
  REQUIRE(made);
  CHECK(made->rel == "Trips/2026");
  CHECK_FALSE(made->existing);
  CHECK(fs::is_directory(s.library / "Trips" / "2026"));
  made = c->make_folder(kRootA, "Trips/2026", &why);
  REQUIRE(made);
  CHECK(made->existing);

  // Add: into any writable folder, never replacing.
  const std::string one = marker(1, 70000), two = marker(2, 70000);
  auto added = c->add(kRootA, "Trips/2026/a.jpg", one.size(), hash_of(one), 0, reader_of(one), {}, nullptr, &why);
  REQUIRE(added);
  CHECK(added->rel == "Trips/2026/a.jpg");
  added = c->add(kRootA, "Trips/2026/a.jpg", two.size(), hash_of(two), 0, reader_of(two), {}, nullptr, &why);
  REQUIRE(added);
  CHECK(added->rel == "Trips/2026/a (2).jpg");
  CHECK(slurp(s.library / "Trips" / "2026" / "a.jpg") == one);
  // A file where a folder is asked for is not replaced either.
  CHECK_FALSE(c->make_folder(kRootA, "Trips/2026/a.jpg", &why));
  CHECK(why.why == why_t::exists);

  // Rename within the folder.
  auto moved = c->move(kRootA, "Trips/2026/a.jpg", kRootA, "Trips/2026/beach.jpg", &why);
  REQUIRE(moved);
  CHECK(moved->rel == "Trips/2026/beach.jpg");
  CHECK_FALSE(fs::exists(s.library / "Trips" / "2026" / "a.jpg"));
  CHECK(slurp(s.library / "Trips" / "2026" / "beach.jpg") == one);
  // A taken name is refused, and both files stay as they were.
  auto taken = c->move(kRootA, "Trips/2026/beach.jpg", kRootA, "Trips/2026/a (2).jpg", &why);
  REQUIRE_FALSE(taken);
  CHECK(taken.error() == mv::status::permission_denied);
  CHECK(why.why == why_t::exists);
  CHECK(why.word == serve::kWhyExists);
  CHECK(slurp(s.library / "Trips" / "2026" / "beach.jpg") == one);
  CHECK(slurp(s.library / "Trips" / "2026" / "a (2).jpg") == two);
  // A folder, into folders that are not there yet.
  moved = c->move(kRootA, "Trips/2026", kRootA, "Archive/Old/2026", &why);
  REQUIRE(moved);
  CHECK(slurp(s.library / "Archive" / "Old" / "2026" / "beach.jpg") == one);
  CHECK_FALSE(fs::exists(s.library / "Trips" / "2026"));
  // Gone from where it was: not found.
  CHECK_FALSE(c->move(kRootA, "Trips/2026", kRootA, "Elsewhere", &why));
  CHECK(why.why == why_t::not_found);

  // Delete: into the folder's own trash, whole, and no listing shows it.
  auto gone = c->remove(kRootA, "Archive/Old/2026/a (2).jpg", &why);
  REQUIRE(gone);
  CHECK(gone->rel.size() > 11);
  CHECK(gone->rel.substr(10) == "/Archive/Old/2026/a (2).jpg");  // "YYYY-MM-DD/" and where it was
  CHECK_FALSE(fs::exists(s.library / "Archive" / "Old" / "2026" / "a (2).jpg"));
  const fs::path trash = s.library / serve::kTrashFolder;
  CHECK(slurp(trash / fs::path(gone->rel)) == two);
  CHECK_FALSE(listed(*c, kRootA, "Archive/Old/2026", "a (2).jpg"));
  CHECK_FALSE(listed(*c, kRootA, "", serve::kTrashFolder));
  CHECK_FALSE(c->stat(kRootA, std::string(serve::kTrashFolder)));  // nor reachable by name
  // The same name deleted again the same day is numbered, never put over the first.
  added = c->add(kRootA, "Archive/Old/2026/a (2).jpg", one.size(), hash_of(one), 0, reader_of(one), {}, nullptr, &why);
  REQUIRE(added);
  auto again = c->remove(kRootA, "Archive/Old/2026/a (2).jpg", &why);
  REQUIRE(again);
  CHECK(again->rel != gone->rel);
  CHECK(slurp(trash / fs::path(gone->rel)) == two);
  CHECK(slurp(trash / fs::path(again->rel)) == one);
  // A folder goes whole.
  gone = c->remove(kRootA, "Archive", &why);
  REQUIRE(gone);
  CHECK_FALSE(fs::exists(s.library / "Archive"));
  CHECK(slurp(trash / fs::path(gone->rel) / "Old" / "2026" / "beach.jpg") == one);
  CHECK_FALSE(listed(*c, kRootA, "", "Archive"));
  CHECK(leftovers_under(s.library) == 0);
}

TEST_CASE("serve change: a view device and a scoped grant are refused outside what they may change",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  write_file(s.library / "keep.jpg", "keep");
  write_file(s.backup / "b.jpg", "backup");
  serve::client::change_refused why;
  {
    const key32 viewer = random_key();
    grant_device(s, viewer, false);
    auto c = open_as(s, viewer);
    CHECK(c->caps().role == "view");
    CHECK(c->changes_files());  // the service has them; the role says this device may not
    auto roots = c->roots();
    REQUIRE(roots);
    CHECK(std::none_of(roots->begin(), roots->end(), [](const serve::root_info& r) { return r.writable; }));
    CHECK_FALSE(c->make_folder(kRootA, "New", &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK(why.word == serve::kWhyNotAllowed);
    auto moved = c->move(kRootA, "keep.jpg", kRootA, "moved.jpg", &why);
    REQUIRE_FALSE(moved);
    CHECK(moved.error() == mv::status::permission_denied);
    CHECK(why.why == why_t::not_allowed);
    CHECK_FALSE(c->remove(kRootA, "keep.jpg", &why));
    CHECK(why.why == why_t::not_allowed);
    const std::string photo = marker(9, 1000);
    CHECK_FALSE(c->add(kRootA, "x.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK(fs::exists(s.library / "keep.jpg"));
    CHECK_FALSE(fs::exists(s.library / "New"));
    CHECK_FALSE(fs::exists(s.library / "x.jpg"));
    CHECK_FALSE(fs::exists(s.library / serve::kTrashFolder));
  }
  {
    // "change", but only Photos: the backup folder is neither source nor destination.
    const key32 scoped = random_key();
    grant_device(s, scoped, true, std::vector<std::string>{kRootA});
    auto c = open_as(s, scoped);
    CHECK(c->caps().role == "change");
    auto roots = c->roots();
    REQUIRE(roots);
    CHECK(root_of(*roots, kRootA)->writable);
    CHECK(root_of(*roots, kRootC) == nullptr);
    CHECK_FALSE(c->move(kRootA, "keep.jpg", kRootC, "keep.jpg", &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK_FALSE(c->move(kRootC, "b.jpg", kRootA, "b.jpg", &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK_FALSE(c->remove(kRootC, "b.jpg", &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK_FALSE(c->make_folder(kRootC, "New", &why));
    CHECK(why.why == why_t::not_allowed);
    // The Catalogue it may read is never changed.
    CHECK_FALSE(c->make_folder(kRootD, "New", &why));
    CHECK(why.why == why_t::not_allowed);
    CHECK(fs::exists(s.library / "keep.jpg"));
    CHECK(slurp(s.backup / "b.jpg") == "backup");
    CHECK_FALSE(fs::exists(s.backup / "keep.jpg"));
    // Inside its folder it may.
    REQUIRE(c->move(kRootA, "keep.jpg", kRootA, "kept.jpg", &why));
    CHECK(fs::exists(s.library / "kept.jpg"));
  }
}

// A path that came from Windows: "...\\.mediaviewer". Missed, the Catalogue was an ordinary
// folder a change device could empty (CI's first Windows run of this file).
TEST_CASE("serve change: a Catalogue named with a backslash is still the Catalogue", "[driftwire-serve][serve-change]") {
  service_fixture s;
  const std::string id = "f6f6f6f6f6f6f6f6";
  // One odd folder name on POSIX, a folder inside "other" on Windows: the same bytes either way.
  fs::path folder = s.dir.root() / "other";
  folder += std::string("\\") + serve::kCatalogueFolder;
  const std::string path = utf8(folder);
  write_file(folder / "catalogue.db", "db");
  s.extra_roots.push_back({id, "Catalogue", path});
  s.start(10000);
  auto c = s.open_client();
  auto roots = c->roots();
  REQUIRE(roots);
  REQUIRE(root_of(*roots, id));
  CHECK_FALSE(root_of(*roots, id)->writable);
  serve::client::change_refused why;
  CHECK_FALSE(c->remove(id, "catalogue.db", &why));
  CHECK(why.why == serve::client::change_why::not_allowed);
  CHECK_FALSE(c->make_folder(id, "New", &why));
  CHECK(slurp(folder / "catalogue.db") == "db");
}

TEST_CASE("serve change: the folder itself, hidden names, escapes and the Catalogue are refused",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  write_file(s.library / "a.jpg", "a");
  write_file(s.library / "Album" / "b.jpg", "b");
  write_file(s.catalogue / "catalogue.db", "db");
  write_file(s.outside / "secret.txt", "secret");
  write_file(s.backup / serve::kBackupMetaFolder / "x" / "ledger.json", "{}");
  auto c = s.open_client();
  serve::client::change_refused why;
  const auto refused_path = [&](mv::result<serve::client::change_done> r) {
    REQUIRE_FALSE(r);
    CHECK(r.error() == mv::status::permission_denied);
    CHECK(why.why == why_t::path);
  };
  // The folder itself.
  refused_path(c->remove(kRootA, "", &why));
  refused_path(c->move(kRootA, "", kRootC, "all", &why));
  refused_path(c->move(kRootA, "a.jpg", kRootC, "", &why));
  refused_path(c->make_folder(kRootA, "", &why));
  // Hidden and meta names: the trash, the backup meta folder, a part file.
  refused_path(c->remove(kRootA, serve::kTrashFolder, &why));
  refused_path(c->make_folder(kRootA, ".hidden", &why));
  refused_path(c->move(kRootA, "a.jpg", kRootA, ".a.jpg", &why));
  refused_path(c->remove(kRootC, std::string(serve::kBackupMetaFolder) + "/x/ledger.json", &why));
  refused_path(c->move(kRootC, serve::kBackupMetaFolder, kRootA, "meta", &why));
  refused_path(c->remove(kRootA, ".a.jpg.0123456789abcdef.mvpart", &why));
  // Escapes.
  refused_path(c->remove(kRootA, "../outside/secret.txt", &why));
  refused_path(c->move(kRootA, "a.jpg", kRootA, "../outside/a.jpg", &why));
  refused_path(c->move(kRootA, "../outside/secret.txt", kRootA, "s.txt", &why));
  refused_path(c->make_folder(kRootA, "/tmp/x", &why));
  refused_path(c->move(kRootA, "Album/./b.jpg", kRootA, "b.jpg", &why));
  // A folder into itself.
  refused_path(c->move(kRootA, "Album", kRootA, "Album/Inner", &why));
  refused_path(c->move(kRootA, "Album", kRootA, "Album", &why));
  // A name too long to keep.
  CHECK_FALSE(c->make_folder(kRootA, std::string(serve::kMaxWriteName + 1, 'n'), &why));
  CHECK(why.why == why_t::name);
  // The Catalogue, and a folder that is not one of this service's.
  CHECK_FALSE(c->remove(kRootD, "catalogue.db", &why));
  CHECK(why.why == why_t::not_allowed);
  CHECK_FALSE(c->move(kRootA, "a.jpg", kRootD, "a.jpg", &why));
  CHECK(why.why == why_t::not_allowed);
  CHECK_FALSE(c->move(kRootD, "catalogue.db", kRootA, "c.db", &why));
  CHECK(why.why == why_t::not_allowed);
  CHECK_FALSE(c->remove("e5e5e5e5e5e5e5e5", "a.jpg", &why));
  CHECK(why.why == why_t::not_allowed);
#if !defined(_WIN32)
  // A link inside the folder is never followed out of it, as source or destination.
  fs::create_directory_symlink(s.outside, s.library / "out");
  refused_path(c->remove(kRootA, "out/secret.txt", &why));
  refused_path(c->move(kRootA, "out/secret.txt", kRootA, "s.txt", &why));
  refused_path(c->move(kRootA, "a.jpg", kRootA, "out/a.jpg", &why));
  refused_path(c->make_folder(kRootA, "out/new", &why));
  CHECK_FALSE(fs::exists(s.outside / "a.jpg"));
  CHECK_FALSE(fs::exists(s.outside / "new"));
#endif
  CHECK(slurp(s.outside / "secret.txt") == "secret");
  CHECK(slurp(s.library / "a.jpg") == "a");
  CHECK(slurp(s.library / "Album" / "b.jpg") == "b");
  CHECK(slurp(s.catalogue / "catalogue.db") == "db");
  CHECK(fs::exists(s.backup / serve::kBackupMetaFolder / "x" / "ledger.json"));
  CHECK_FALSE(fs::exists(s.library / serve::kTrashFolder));
}

TEST_CASE("serve change: a move between folders, renamed or copied, never leaves a half file",
          "[driftwire-serve][serve-change]") {
  for (const bool copy : {false, true}) {
    CAPTURE(copy);
    service_fixture s;
    s.srv.reset();
    s.copy_moves = copy;  // as across volumes
    s.backup_limit = 1024 * 1024;
    s.start(10000);
    const std::string big = marker(4, 600 * 1024);
    write_file(s.library / "clip.mov", big);
    write_file(s.library / "Album" / "one.jpg", marker(5, 3000));
    write_file(s.library / "Album" / "Deeper" / "two.jpg", marker(6, 5000));
    auto c = s.open_client();
    serve::client::change_refused why;

    auto moved = c->move(kRootA, "clip.mov", kRootC, "Moved/clip.mov", &why);
    REQUIRE(moved);
    CHECK(slurp(s.backup / "Moved" / "clip.mov") == big);
    CHECK_FALSE(fs::exists(s.library / "clip.mov"));
    // It counts against the backup folder's limit now: another as big is past it, and stays.
    write_file(s.library / "clip2.mov", big);
    CHECK_FALSE(c->move(kRootA, "clip2.mov", kRootC, "clip2.mov", &why));
    CHECK(why.why == why_t::limit);
    CHECK(slurp(s.library / "clip2.mov") == big);
    CHECK_FALSE(fs::exists(s.backup / "clip2.mov"));

    // A folder with folders in it.
    moved = c->move(kRootA, "Album", kRootC, "Album", &why);
    REQUIRE(moved);
    CHECK(slurp(s.backup / "Album" / "one.jpg") == marker(5, 3000));
    CHECK(slurp(s.backup / "Album" / "Deeper" / "two.jpg") == marker(6, 5000));
    CHECK_FALSE(fs::exists(s.library / "Album"));
    // Back, onto a name that is taken: refused, and nothing on either side changes.
    write_file(s.library / "Album" / "mine.jpg", "mine");
    CHECK_FALSE(c->move(kRootC, "Album", kRootA, "Album", &why));
    CHECK(why.why == why_t::exists);
    CHECK(slurp(s.library / "Album" / "mine.jpg") == "mine");
    CHECK_FALSE(fs::exists(s.library / "Album" / "one.jpg"));
    CHECK(fs::exists(s.backup / "Album" / "Deeper" / "two.jpg"));
    CHECK(leftovers_under(s.library) == 0);
    CHECK(leftovers_under(s.backup) == 0);
  }
}

TEST_CASE("serve change: two devices moving one file at once never both get it", "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;  // a copy takes long enough for the second to meet the first
  s.start(10000);
  const std::string big = marker(8, 24 * 1024 * 1024);
  write_file(s.library / "big.mov", big);
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second"));
  auto c1 = s.open_client();
  auto c2 = open_as(s, second);
  serve::client::change_refused w1, w2;
  mv::result<serve::client::change_done> r1 = mv::err(mv::status::io), r2 = mv::err(mv::status::io);
  std::thread t1([&] { r1 = c1->move(kRootA, "big.mov", kRootC, "one.mov", &w1); });
  std::thread t2([&] { r2 = c2->move(kRootA, "big.mov", kRootC, "two.mov", &w2); });
  t1.join();
  t2.join();
  CHECK(static_cast<int>(r1.has_value()) + static_cast<int>(r2.has_value()) == 1);
  const auto& lost = r1 ? w2 : w1;
  CHECK((lost.why == why_t::busy || lost.why == why_t::not_found));
  CHECK(slurp(s.backup / (r1 ? "one.mov" : "two.mov")) == big);
  CHECK_FALSE(fs::exists(s.backup / (r1 ? "two.mov" : "one.mov")));
  CHECK_FALSE(fs::exists(s.library / "big.mov"));
  CHECK(leftovers_under(s.backup) == 0);
}

TEST_CASE("serve change: old clients and old services keep what they had", "[driftwire-serve][serve-change]") {
  // On the wire: the names, the move's destination, and what an old service's answers say.
  serve::open_request r;
  r.id = 7;
  r.kind = serve::op::move;
  r.root = kRootA;
  r.rel = "a.jpg";
  r.to_root = kRootC;
  r.to_rel = "b/a.jpg";
  bool unknown = true;
  auto back = serve::parse_open(serve::encode_open(r), unknown);
  REQUIRE(back);
  CHECK_FALSE(unknown);
  CHECK(back->kind == serve::op::move);
  CHECK(back->to_root == kRootC);
  CHECK(back->to_rel == "b/a.jpg");
  CHECK(std::string(serve::op_name(serve::op::remove)) == "delete");
  CHECK(serve::parse_op("delete") == serve::op::remove);
  const std::string no_to = R"({"op":"move","root":"a1a1a1a1a1a1a1a1","rel":"a.jpg"})";
  std::vector<std::uint8_t> frame = {1, 0, 0, 0};
  frame.insert(frame.end(), no_to.begin(), no_to.end());
  CHECK_FALSE(serve::parse_open(frame, unknown));
  // A service from before: no change op in its caps, and `writable` only on a backup folder.
  auto old_caps = serve::parse_caps(R"({"v":1,"ops":["roots","list","stat","read_range","write"],"chunk":65536,"bulk_chunk":65536})");
  REQUIRE(old_caps);
  CHECK((old_caps->ops & serve::kChangeOps) == 0);
  auto old_roots = serve::parse_roots(
      R"({"roots":[{"id":"a1a1a1a1a1a1a1a1","label":"Photos"},{"id":"c3c3c3c3c3c3c3c3","label":"B","backup":true,"writable":true}]})");
  REQUIRE(old_roots);
  CHECK_FALSE((*old_roots)[0].writable);
  CHECK((*old_roots)[1].writable);
  // And this service's roots, read the way a reader from before reads them: the same shape.
  std::vector<serve::root_info> mine = {{kRootA, "Photos", false, true}, {kRootC, "B", true, true}};
  const std::string text = serve::roots_json(mine);
  CHECK(text.find(R"("label":"Photos","writable":true)") != std::string::npos);
  CHECK(text.find(R"("backup":true,"writable":true)") != std::string::npos);

  // A client from before (a phone that backs up): no change op is agreed, and backup works.
  service_fixture s;
  write_file(s.library / "a.jpg", "a");
  auto o = s.client_options();
  o.ops = serve::kAllOps | serve::kWriteOp;
  auto old_client = serve::client::open(s.connect(), o);
  REQUIRE(old_client);
  auto& c = **old_client;
  CHECK((c.caps().ops & serve::kChangeOps) == 0);
  CHECK_FALSE(c.changes_files());
  serve::client::change_refused why;
  auto refused = c.remove(kRootA, "a.jpg", &why);
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::unsupported_format);
  CHECK(why.why == why_t::unsupported);
  CHECK(fs::exists(s.library / "a.jpg"));
  // Asked anyway, on the wire, the service answers that stream unsupported and goes on.
  sp::channel::options co;
  co.access_key = s.access;
  co.static_key = s.device;
  auto raw = sp::channel::open_library(s.connect(), "aaaaaaaaaaaaaaaa", "Old", co);
  REQUIRE(raw);
  REQUIRE((*raw)->send(sp::msg::serve_hello, std::string_view(R"({"v":1,"min":1,"ops":["list","write"]})")));
  sp::message m;
  REQUIRE((*raw)->receive(m, 3000));
  REQUIRE(m.type == sp::msg::serve_caps);
  const std::string del = R"({"op":"delete","root":"a1a1a1a1a1a1a1a1","rel":"a.jpg"})";
  std::vector<std::uint8_t> ask = {2, 0, 0, 0};
  ask.insert(ask.end(), del.begin(), del.end());
  REQUIRE((*raw)->send(sp::msg::stream_open, ask));
  REQUIRE((*raw)->receive(m, 3000));
  REQUIRE(m.type == sp::msg::stream_end);
  auto e = serve::parse_end(m.body);
  REQUIRE(e);
  CHECK(e->code == serve::end_code::unsupported);
  CHECK(fs::exists(s.library / "a.jpg"));
  const std::string photo = marker(3, 5000);
  serve::client::write_refused wr;
  auto done = c.write(kRootC, "IMG.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &wr);
  REQUIRE(done);
  CHECK(slurp(s.backup / "IMG.jpg") == photo);
}

// ---- changing the shared folders: what a review of the first cut found --------------------

namespace {

std::string utc_day() {
  using namespace std::chrono;
  const year_month_day d{floor<days>(system_clock::now())};
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                static_cast<unsigned>(d.day()));
  return buf;
}

#if !defined(_WIN32)
// The tests run as a person, whom a folder's mode binds (root is bound by nothing).
bool modes_bind() { return ::geteuid() != 0; }
#endif

}  // namespace

TEST_CASE("serve change: a folder never goes into itself, by a case variant or through a nested folder",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  const std::string kRootE = "e5e5e5e5e5e5e5e5";
  s.extra_roots.push_back({kRootE, "Inner", utf8(s.library / "Outer" / "Inner")});
  write_file(s.library / "Outer" / "Inner" / "in.jpg", "inner");
  write_file(s.library / "Trip" / "x.jpg", "x");
  s.start(10000);
  auto c = s.open_client();
  serve::client::change_refused why;
  if (fs::exists(s.library / "TRIP")) {  // a volume that folds case (APFS, NTFS by default)
    CHECK_FALSE(c->move(kRootA, "Trip", kRootA, "trip/Trip", &why));
    CHECK(why.why == why_t::path);
    CHECK_FALSE(c->move(kRootA, "Trip", kRootA, "TRIP/New/Trip", &why));
    CHECK(why.why == why_t::path);
    CHECK_FALSE(fs::exists(s.library / "Trip" / "New"));
    CHECK(slurp(s.library / "Trip" / "x.jpg") == "x");
    CHECK(leftovers_under(s.library) == 0);
  }
  // A folder that holds another named folder is neither moved nor trashed: it would carry it off.
  CHECK_FALSE(c->move(kRootA, "Outer", kRootA, "Moved", &why));
  CHECK(why.why == why_t::path);
  CHECK_FALSE(c->move(kRootA, "Outer", kRootC, "Outer", &why));
  CHECK(why.why == why_t::path);
  CHECK_FALSE(c->remove(kRootA, "Outer", &why));
  CHECK(why.why == why_t::path);
  // Nor into that nested folder, which is inside it.
  CHECK_FALSE(c->move(kRootA, "Outer", kRootE, "sub", &why));
  CHECK(why.why == why_t::path);
  CHECK(slurp(s.library / "Outer" / "Inner" / "in.jpg") == "inner");
  CHECK_FALSE(fs::exists(s.library / serve::kTrashFolder));
  // A file out of it is an ordinary move.
  REQUIRE(c->move(kRootE, "in.jpg", kRootA, "in.jpg", &why));
}

#if !defined(_WIN32)
TEST_CASE("serve change: a rename the disk refuses is an error, never a copy", "[driftwire-serve][serve-change]") {
  if (!modes_bind()) return;
  service_fixture s;
  write_file(s.library / "Locked" / "a.jpg", "a");
  fs::permissions(s.library / "Locked", fs::perms::owner_read | fs::perms::owner_exec);
  struct unlock {
    fs::path p;
    ~unlock() { fs::permissions(p, fs::perms::owner_all); }
  } restore{s.library / "Locked"};
  auto c = s.open_client();
  serve::client::change_refused why;
  auto moved = c->move(kRootA, "Locked/a.jpg", kRootA, "a.jpg", &why);
  REQUIRE_FALSE(moved);
  CHECK(why.why == why_t::failed);
  moved = c->move(kRootA, "Locked/a.jpg", kRootC, "a.jpg", &why);  // another folder, the same disk
  REQUIRE_FALSE(moved);
  CHECK(why.why == why_t::failed);
  CHECK(slurp(s.library / "Locked" / "a.jpg") == "a");
  CHECK_FALSE(fs::exists(s.library / "a.jpg"));
  CHECK_FALSE(fs::exists(s.backup / "a.jpg"));
  CHECK(leftovers_under(s.backup) == 0);
}

TEST_CASE("serve change: a copy that fails half way leaves the source whole and nothing behind",
          "[driftwire-serve][serve-change]") {
  if (!modes_bind()) return;
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  s.start(10000);
  write_file(s.library / "Album" / "1.jpg", "one");
  write_file(s.library / "Album" / "2.jpg", "two");
  write_file(s.library / "Album" / "3.jpg", "three");
  fs::permissions(s.library / "Album" / "2.jpg", fs::perms::none);
  struct unlock {
    fs::path p;
    ~unlock() { fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write); }
  } restore{s.library / "Album" / "2.jpg"};
  auto c = s.open_client();
  serve::client::change_refused why;
  REQUIRE_FALSE(c->move(kRootA, "Album", kRootC, "Album", &why));
  CHECK(why.why == why_t::failed);
  CHECK(slurp(s.library / "Album" / "1.jpg") == "one");
  CHECK(slurp(s.library / "Album" / "3.jpg") == "three");
  CHECK(fs::exists(s.library / "Album" / "2.jpg"));
  CHECK_FALSE(fs::exists(s.backup / "Album"));
  CHECK(leftovers_under(s.backup) == 0);
}
#endif

TEST_CASE("serve change: a device revoked during a long copy stops it, and keeps nothing of it",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  const key32 second = random_key();
  int files = 0;
  s.during_copy = [&] {
    if (++files == 2) {
      s.unpair(second);
      s.srv->recheck_devices();
    }
  };
  s.start(10000);
  REQUIRE(s.pair(second, "Second"));
  for (int i = 0; i < 4; ++i) write_file(s.library / "Album" / ("p" + std::to_string(i) + ".jpg"), marker(i, 40000));
  auto c = open_as(s, second);
  serve::client::change_refused why;
  REQUIRE_FALSE(c->move(kRootA, "Album", kRootC, "Album", &why));
  CHECK((why.why == why_t::not_allowed || why.why == why_t::failed));  // refused, or its session cut first
  CHECK(eventually([&] { return !fs::exists(s.backup / "Album") && leftovers_under(s.backup) == 0; }));
  for (int i = 0; i < 4; ++i) CHECK(slurp(s.library / "Album" / ("p" + std::to_string(i) + ".jpg")) == marker(i, 40000));
  CHECK(files == 2);  // no file after the revoke was copied
}

TEST_CASE("serve change: a backup folder's count follows what a delete and a move do", "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  int files = 0;
  s.during_copy = [&] {
    // Both of Kept's files change once its first (whichever it is) was copied: that one stays
    // where it was; the second is copied as it is now, and goes. (The 1st call is out.jpg's.)
    if (++files == 3) {
      write_file(s.backup / "Kept" / "a.jpg", marker(9, 4000));
      write_file(s.backup / "Kept" / "b.jpg", marker(10, 4000));
    }
  };
  write_file(s.backup / "gone.jpg", marker(1, 10000));
  write_file(s.backup / "out.jpg", marker(2, 20000));
  write_file(s.backup / "Kept" / "a.jpg", marker(3, 4000));
  write_file(s.backup / "Kept" / "b.jpg", marker(4, 4000));
  s.start(10000);
  auto c = s.open_client();
  auto used = [&] {
    auto st = c->stat(kRootC, "");
    REQUIRE(st);
    return st->used;
  };
  const std::uint64_t before = used();
  serve::client::change_refused why;
  // Trashed: still on that disk, in that folder, so it still counts until the owner empties it.
  REQUIRE(c->remove(kRootC, "gone.jpg", &why));
  CHECK(used() == before);
  REQUIRE(c->move(kRootC, "out.jpg", kRootA, "out.jpg", &why));
  CHECK(used() == before - 20000);
  // Only what was removed is taken off: the file that changed stays, and counts.
  REQUIRE(c->move(kRootC, "Kept", kRootA, "Kept", &why));
  CHECK(fs::exists(s.backup / "Kept" / "a.jpg") != fs::exists(s.backup / "Kept" / "b.jpg"));
  CHECK(used() == before - 20000 - 4000);

  // A folder written to before it was a backup folder is counted from its disk, once.
  serve::backup::book book;
  book.free_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
  serve::root_config plain{kRootA, "Photos", utf8(s.other)};
  write_file(s.other / "x.bin", marker(5, 5000));
  REQUIRE(book.reserve(plain, 5000) == serve::backup::book::verdict::ok);
  book.wrote(plain.id, 5000);
  plain.backup = true;
  plain.limit = 1 << 20;
  serve::stat_info info;
  book.space(plain, info);
  CHECK(info.used == 5000);
}

TEST_CASE("serve change: trash names run out, a grant runs out, the disk runs out", "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  std::atomic<std::int64_t> unix_now{std::time(nullptr)};
  std::atomic<std::int64_t> steady_ms{0};
  const auto t0 = std::chrono::steady_clock::now();
  s.now_unix = [&] { return unix_now.load(); };
  s.now_steady = [&] { return t0 + std::chrono::milliseconds(steady_ms.load()); };
  s.start(10000);
  serve::client::change_refused why;
  {
    // Every numbered trash name for the day taken: refused, and the file stays.
    write_file(s.library / "a.jpg", "a");
    const std::string day = utc_day();
    for (int n = 1; n <= serve::backup::kMaxNumbered; ++n) {
      write_file(s.library / serve::kTrashFolder / day / serve::backup::numbered("a.jpg", n), "old");
    }
    auto c = s.open_client();
    auto gone = c->remove(kRootA, "a.jpg", &why);
    if (utc_day() == day) {
      REQUIRE_FALSE(gone);
      CHECK(why.why == why_t::exists);
      CHECK(slurp(s.library / "a.jpg") == "a");
    }
  }
  {
    // A grant whose time is up changes nothing, though its session is still up.
    write_file(s.library / "b.jpg", "b");
    const key32 guest = random_key();
    std::vector<mv::driftwire::serve::grants::grant> list;
    REQUIRE(mv::driftwire::serve::grants::load(*s.store, list));
    mv::driftwire::serve::grants::grant g;
    g.name = "Guest";
    g.key = public_of(guest);
    g.expires = unix_now.load() + 100;
    g.created = 1;
    list.push_back(g);
    REQUIRE(mv::driftwire::serve::grants::save(*s.store, list));
    auto c = open_as(s, guest);
    CHECK(c->caps().role == "change");
    unix_now += 200;
    steady_ms += 2000;  // past the second a device's access is kept
    CHECK_FALSE(c->remove(kRootA, "b.jpg", &why));
    CHECK((why.why == why_t::not_allowed || why.why == why_t::failed));
    CHECK(slurp(s.library / "b.jpg") == "b");
  }
  {
    // An ordinary folder keeps its reserve free: a write or a copy that would eat it is refused;
    // a rename on the same disk needs no room.
    s.free_bytes = serve::kDefaultReserve + 1000;
    write_file(s.backup / "c.jpg", marker(6, 5000));
    auto c = s.open_client();
    const std::string photo = marker(7, 5000);
    CHECK_FALSE(c->add(kRootA, "new.jpg", photo.size(), hash_of(photo), 0, reader_of(photo), {}, nullptr, &why));
    CHECK(why.why == why_t::full);
    CHECK_FALSE(fs::exists(s.library / "new.jpg"));
    REQUIRE(c->move(kRootC, "c.jpg", kRootA, "c.jpg", &why));
    CHECK(fs::exists(s.library / "c.jpg"));
  }
}

TEST_CASE("serve change: the disk running out refuses a copy into an ordinary folder", "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  s.start(10000);
  s.free_bytes = serve::kDefaultReserve + 1000;
  write_file(s.backup / "c.jpg", marker(6, 5000));
  auto c = s.open_client();
  serve::client::change_refused why;
  CHECK_FALSE(c->move(kRootC, "c.jpg", kRootA, "c.jpg", &why));
  CHECK(why.why == why_t::full);
  CHECK(fs::exists(s.backup / "c.jpg"));
  CHECK_FALSE(fs::exists(s.library / "c.jpg"));
}

TEST_CASE("serve change: a folder with a write still open in it is busy, and a copy leaves part files",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  s.start(10000);
  const key32 second = random_key();
  REQUIRE(s.pair(second, "Second"));
  auto writer = s.open_client();
  auto mover = open_as(s, second);
  const std::string clip = marker(3, 6 * 1024 * 1024);
  serve::client::change_refused moved_why, removed_why, why;
  bool asked = false;
  auto added = writer->add(kRootA, "P/clip.mov", clip.size(), hash_of(clip), 0, reader_of(clip),
                           [&](std::uint64_t n) {
                             if (asked || n < 1024 * 1024) return;
                             asked = true;
                             (void)mover->move(kRootA, "P", kRootC, "P", &moved_why);
                             (void)mover->remove(kRootA, "P", &removed_why);
                           },
                           nullptr, &why);
  REQUIRE(asked);
  CHECK(moved_why.why == why_t::busy);
  CHECK(removed_why.why == why_t::busy);
  REQUIRE(added);
  CHECK(slurp(s.library / "P" / "clip.mov") == clip);
  // Another device's unfinished copy is not carried by a move's copy.
  write_file(s.library / "P" / ".x.jpg.0123456789abcdef.mvpart", "half");
  REQUIRE(mover->move(kRootA, "P", kRootC, "P", &why));
  CHECK(slurp(s.backup / "P" / "clip.mov") == clip);
  CHECK(leftovers_under(s.backup) == 0);
  CHECK(fs::exists(s.library / "P" / ".x.jpg.0123456789abcdef.mvpart"));
}

TEST_CASE("serve change: the file port says which folder is which, and when a file changed", "[driftwire-serve][serve-change]") {
  scratch_dir dir{"serve_identity"};
  write_file(dir.root() / "A" / "f.bin", "x");
  const auto a = mv::io::identify_entry(utf8(dir.root() / "A"));
  REQUIRE(a);
  CHECK(a->known());
  const auto f = mv::io::identify_entry(utf8(dir.root() / "A" / "f.bin"));
  REQUIRE(f);
  CHECK_FALSE(f.value() == a.value());
  CHECK(f->volume == a->volume);
  CHECK(mv::io::sync_directory(utf8(dir.root() / "A")));
#if !defined(_WIN32)
  fs::create_directory_symlink(dir.root() / "A", dir.root() / "L");
  const auto l = mv::io::identify_entry(utf8(dir.root() / "L"));
  REQUIRE_FALSE(l);
  CHECK(l.error() == mv::status::permission_denied);
#endif
  // stat_beneath's mtime_ns is the file's own (a rewrite within the second at the same size differs).
  const auto st = mv::io::stat_beneath(utf8(dir.root()), "A/f.bin");
  REQUIRE(st);
  CHECK(st->mtime_ns != 0);
  CHECK(st->mtime_ns / 1000000000 == st->mtime_unix);
}

TEST_CASE("serve change: a device made view-only during a long copy stops it at the next file",
          "[driftwire-serve][serve-change]") {
  service_fixture s;
  s.srv.reset();
  s.copy_moves = true;
  const key32 limited = random_key();
  int files = 0;
  s.during_copy = [&] {
    if (++files != 2) return;
    // Its role turned to "view" on the card: still paired, the session goes on.
    std::vector<mv::driftwire::serve::grants::grant> list;
    REQUIRE(mv::driftwire::serve::grants::load(*s.store, list));
    for (auto& g : list) g.backup = false;
    REQUIRE(mv::driftwire::serve::grants::save(*s.store, list));
    s.srv->recheck_devices();
  };
  s.start(10000);
  grant_device(s, limited, true, std::vector<std::string>{kRootA, kRootC});  // a grant always limits something
  for (int i = 0; i < 4; ++i) write_file(s.library / "Album" / ("p" + std::to_string(i) + ".jpg"), marker(i, 40000));
  auto c = open_as(s, limited);
  serve::client::change_refused why;
  REQUIRE_FALSE(c->move(kRootA, "Album", kRootC, "Album", &why));
  CHECK(why.why == why_t::not_allowed);
  CHECK_FALSE(c->closed());
  CHECK(files == 2);
  CHECK_FALSE(fs::exists(s.backup / "Album"));
  CHECK(leftovers_under(s.backup) == 0);
  for (int i = 0; i < 4; ++i) CHECK(slurp(s.library / "Album" / ("p" + std::to_string(i) + ".jpg")) == marker(i, 40000));
}
