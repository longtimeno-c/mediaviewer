// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint (docs/design/28): the wire's rules, the sealed channel, and two
// or three engines sending to each other over loopback TCP, found through an
// in-memory discovery bus instead of the LAN beacon. The issue's verify, as
// far as one machine can run it: both sides show the same code, Accept
// delivers the files with matching bytes, Decline and a third client receive
// nothing, Cancel mid-way leaves nothing partial, a machine in the middle
// shows two different codes, and no sender path climbs out of the folder.
#include "catch_compat.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/screen.h"
#include "addons/sharepoint/channel.h"
#include "addons/sharepoint/engine.h"
#include "addons/sharepoint/net.h"
#include "addons/sharepoint/transfer.h"
#include "addons/sharepoint/wire.h"
#include "import_fixture.h"

using namespace mv::test;
namespace sp = mv::sharepoint;

namespace {

template <typename F>
bool eventually(F f, int seconds = 15) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < until) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return f();
}

// Discovery on one machine, in memory: every member hears every other one,
// three times a second, at 127.0.0.1.
struct bus {
  std::mutex m;
  struct member {
    sp::beacon self;
    sp::discovery::seen_fn seen;
  };
  std::map<int, member> members;
  int next = 0;
};

class bus_discovery final : public sp::discovery {
 public:
  explicit bus_discovery(bus& b) : bus_(b) {}
  ~bus_discovery() override { stop(); }
  bool start(const sp::beacon& self, seen_fn seen, tick_fn tick) override {
    {
      std::lock_guard lock(bus_.m);
      key_ = bus_.next++;
      bus_.members[key_] = {self, std::move(seen)};
    }
    stop_.store(false);
    thread_ = std::thread([this, tick = std::move(tick)] {
      while (!stop_.load()) {
        std::vector<std::pair<sp::beacon, sp::discovery::seen_fn>> calls;
        {
          std::lock_guard lock(bus_.m);
          const auto me = bus_.members.find(key_);
          if (me == bus_.members.end()) break;
          for (const auto& [k, other] : bus_.members) {
            if (k != key_) calls.emplace_back(me->second.self, other.seen);
          }
        }
        for (auto& [b, seen] : calls) seen(b, "127.0.0.1");
        if (tick) tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
    return true;
  }
  void stop() noexcept override {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(bus_.m);
    bus_.members.erase(key_);
  }

 private:
  bus& bus_;
  int key_ = -1;
  std::atomic<bool> stop_{true};
  std::thread thread_;
};

class fake_host final : public sp::engine_host {
 public:
  explicit fake_host(fs::path received) : received_(std::move(received)) {}
  void post_tree(const std::string& tree) override {
    std::lock_guard lock(m);
    last_tree = tree;
    ++posts;
  }
  void show_screen(bool show) override {
    std::lock_guard lock(m);
    shown += show ? 1 : 0;
  }
  std::string default_destination() override { return utf8(received_); }
  std::optional<std::uint64_t> free_bytes(const std::string&) override { return room; }
  bool should_yield() override { return slow.load(); }
  std::vector<std::uint32_t> pair_partners(const std::vector<std::string>& names) override {
    ++pair_calls;
    // A slow folder (a large selection, a share): the reader is held here.
    while (hold_pairs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // RAW + JPEG by stem, as io::pairing does for the cases tested here.
    std::vector<std::uint32_t> out(names.size(), UINT32_MAX);
    for (std::size_t i = 0; i < names.size(); ++i) {
      for (std::size_t j = 0; j < names.size(); ++j) {
        if (i == j) continue;
        const auto stem = [](const std::string& n) { return n.substr(0, n.rfind('.')); };
        const auto ext = [](const std::string& n) { return n.substr(n.rfind('.') + 1); };
        if (stem(names[i]) == stem(names[j]) && ((ext(names[i]) == "CR2" && ext(names[j]) == "JPG") ||
                                                 (ext(names[i]) == "JPG" && ext(names[j]) == "CR2"))) {
          out[i] = static_cast<std::uint32_t>(j);
        }
      }
    }
    return out;
  }
  // Shown with a failure (ctest prints the output): where a transfer ended.
  void log(int, const char* ascii) override { std::fprintf(stderr, "[log] %s\n", ascii); }

  std::mutex m;
  std::string last_tree;
  int posts = 0;
  int shown = 0;
  std::optional<std::uint64_t> room;
  std::atomic<bool> slow{false};
  std::atomic<bool> hold_pairs{false};
  std::atomic<int> pair_calls{0};

 private:
  fs::path received_;
};

// One MediaViewer: its host, its engine, its window open.
struct client {
  fake_host host;
  sp::engine eng;
  client(bus& b, const fs::path& received, const std::string& name)
      : host(received), eng(host, options(b, name)) {
    eng.screen_opened("");
  }
  static sp::engine::options options(bus& b, const std::string& name) {
    sp::engine::options o;
    o.my_name = name;
    o.use_bluetooth = false;
    o.loopback_only = true;
    o.make_discovery = [&b] { return std::make_unique<bus_discovery>(b); };
    return o;
  }
  bool sees(const client& other) const {
    const auto v = eng.snapshot();
    return std::any_of(v.peers.begin(), v.peers.end(), [&](const auto& p) { return p.id == other.eng.id(); });
  }
  void pick(const std::vector<fs::path>& files) {
    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(utf8(f));
    eng.set_files(paths);
    REQUIRE(eventually([&] {
      const auto v = eng.snapshot();
      return !v.reading_folder && v.file_count >= files.size();
    }));
  }
  // Picks `other` and sends; with `confirm`, says the codes match as soon
  // as both screens show one (what the person at this end checks).
  void send_to(const client& other, bool confirm = true) {
    REQUIRE(eventually([&] { return sees(other); }));
    eng.on_event("peers", 4, "[\"" + other.eng.id() + "\"]");
    eng.on_event("send", 1, "");
    if (!confirm) return;
    REQUIRE(eventually([&] {
      const auto o = eng.snapshot().out;
      return o == sp::view::out_phase::confirming || o == sp::view::out_phase::ended;
    }));
    eng.on_event("out_confirm", 1, "");
  }
  void save_into(const fs::path& dir) { eng.on_event("destination", 2, "\"" + json_path(dir) + "\""); }
  std::string tree() {
    std::lock_guard lock(host.m);
    return host.last_tree;
  }
};

std::vector<std::string> files_in(const fs::path& dir) {
  std::vector<std::string> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    // Relative paths with "/" on every platform, as the sender's names travel.
    std::string rel = utf8(fs::relative(e.path(), dir));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    out.push_back(std::move(rel));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool any_temporary(const fs::path& dir) {
  const auto all = files_in(dir);
  return std::any_of(all.begin(), all.end(), [](const std::string& n) { return n.find(".mvtmp") != std::string::npos; });
}

// A link over loopback: one end connected, the other accepted.
std::pair<std::unique_ptr<sp::link>, std::unique_ptr<sp::link>> loopback_pair() {
  auto l = sp::net::listen(true);
  REQUIRE(l);
  std::unique_ptr<sp::link> server;
  std::thread t([&] {
    auto a = (*l)->accept(5000);
    if (a) server = std::move(*a);
  });
  auto c = sp::net::connect({"127.0.0.1", (*l)->port()}, 5000);
  t.join();
  REQUIRE(c);
  REQUIRE(server);
  return {std::move(*c), std::move(server)};
}

// Both ends of a channel, opened at once.
std::pair<std::unique_ptr<sp::channel>, std::unique_ptr<sp::channel>> channel_pair() {
  auto [a, b] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, link = std::move(b)]() mutable { server = sp::channel::open(std::move(link), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  auto opened = sp::channel::open(std::move(a), true, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  REQUIRE(opened);
  REQUIRE(server);
  return {std::move(*opened), std::move(*server)};
}

// A link that passes bytes through and can flip one, at a byte count.
class tamper_link final : public sp::link {
 public:
  tamper_link(std::unique_ptr<sp::link> inner, std::size_t flip_at) : inner_(std::move(inner)), flip_at_(flip_at) {}
  mv::expected write_all(std::span<const std::uint8_t> bytes) override {
    std::vector<std::uint8_t> copy(bytes.begin(), bytes.end());
    for (std::size_t i = 0; i < copy.size(); ++i) {
      if (written_ + i == flip_at_) copy[i] ^= 0x40;
    }
    written_ += copy.size();
    return inner_->write_all(copy);
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  sp::transport via() const noexcept override { return inner_->via(); }

 private:
  std::unique_ptr<sp::link> inner_;
  std::size_t flip_at_;
  std::size_t written_ = 0;
};

}  // namespace

// ---- the wire -------------------------------------------------------------------------------

TEST_CASE("sharepoint: a sender's path never leaves the receiver's folder", "[sharepoint]") {
  for (const char* ok : {"IMG_1.JPG", "Trip/IMG_1.JPG", "Trip/Day 2/clip.mov", "caf\xC3\xA9.heic", ".hidden"}) {
    INFO(ok);
    CHECK(sp::safe_relative(ok));
  }
  CHECK(sp::safe_relative(std::string(sp::kMaxPartBytes, 'a')));
  CHECK_FALSE(sp::safe_relative(std::string(sp::kMaxPartBytes + 1, 'a')));
  for (const char* bad : {"", "/etc/passwd", "../x.jpg", "a/../../b", "a/./b", "a//b", "a/", "C:x.jpg", "a\\b.jpg",
                          "CON", "con.txt", "Trip/NUL.jpg", "COM1", "lpt9.log", "a.", "a ", "x?y", "x*y", "a\x01"
                          "b",
                          "\xFF\xFE"}) {
    INFO(bad);
    CHECK_FALSE(sp::safe_relative(bad));
  }
  CHECK_FALSE(sp::safe_relative(std::string(1100, 'a')));
}

TEST_CASE("sharepoint: an offer is refused whole for one bad path or wrong sums", "[sharepoint]") {
  CHECK(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"T/b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"../b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":6,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":3,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":1,"bytes":-1,"files":[{"rel":"a.jpg","size":-1}]})"));
  CHECK_FALSE(sp::parse_offer("not json"));
  // Sizes that would wrap the total, and one past the cap.
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":1,"files":[{"rel":"a.mov","size":100000000000},)"
                              R"({"rel":"b.mov","size":9223372036854775807}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":1,"bytes":1125899906842625,"files":[{"rel":"a.mov","size":1125899906842625}]})"));
}

TEST_CASE("sharepoint: the beacon carries an id, a name and a port, nothing else", "[sharepoint]") {
  sp::beacon b{"0123456789abcdef", "Tristan\xE2\x80\x99s MacBook", 50123};
  const std::string text = sp::encode_beacon(b);
  CHECK(text.rfind("MVSP1{", 0) == 0);
  auto back = sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
  REQUIRE(back);
  CHECK(back->id == b.id);
  CHECK(back->name == b.name);
  CHECK(back->port == 50123);
  for (const std::string& bad : {std::string("MVSP1{}"), std::string("MVSP1{\"v\":2,\"id\":\"0123456789abcdef\",\"name\":\"x\",\"port\":1}"),
                                std::string("MVSP1{\"v\":1,\"id\":\"../\",\"name\":\"x\",\"port\":1}"),
                                std::string("MVSP1{\"v\":1,\"id\":\"0123456789abcdef\",\"name\":\"x\",\"port\":0}"),
                                std::string("hello")}) {
    CHECK_FALSE(sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}));
  }
  CHECK(sp::clean_name(std::string(200, 'x')).size() == sp::kMaxNameBytes);
  CHECK(sp::clean_name("") == "MediaViewer");
  CHECK(sp::clean_name("a\nb\tc") == "abc");
}

TEST_CASE("sharepoint: the code is six digits from both keys, the client's first", "[sharepoint]") {
  std::array<std::uint8_t, 32> a{}, b{};
  a.fill(1);
  b.fill(2);
  const std::string ab = sp::pairing_code(a, b);
  CHECK(ab.size() == 7);
  CHECK(ab[3] == ' ');
  CHECK(sp::pairing_code(a, b) == ab);
  CHECK(sp::pairing_code(b, a) != ab);
}

// ---- the channel ------------------------------------------------------------------------------

TEST_CASE("sharepoint: both ends of a channel show the same code and each other's name", "[sharepoint]") {
  auto [a, b] = channel_pair();
  CHECK(a->code() == b->code());
  CHECK(a->peer_name() == "Bea");
  CHECK(b->peer_name() == "Ann");
  CHECK(a->peer_id() == "bbbbbbbbbbbbbbbb");
  REQUIRE(a->send(sp::msg::offer, std::string_view("{\"x\":1}")));
  sp::message m;
  REQUIRE(b->receive(m, 2000));
  CHECK(m.type == sp::msg::offer);
  CHECK(m.text() == "{\"x\":1}");
  // A big message (a chunk), the other way.
  std::vector<std::uint8_t> chunk = pattern(sp::kChunkBytes, 7);
  REQUIRE(b->send(sp::msg::data, chunk));
  REQUIRE(a->receive(m, 2000));
  CHECK(m.type == sp::msg::data);
  CHECK(m.body == chunk);
}

TEST_CASE("sharepoint: a machine in the middle shows each side a different code", "[sharepoint]") {
  // Ann -> Mallory -> Bea: Mallory answers Ann as a server and calls Bea as a
  // client, so it can read both streams; the codes differ, and the people
  // comparing them cancel.
  auto [ann_link, mal_in] = loopback_pair();
  auto [mal_out, bea_link] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> ann = mv::err(mv::status::internal), bea = mv::err(mv::status::internal),
                                           mal_a = mv::err(mv::status::internal),
                                           mal_b = mv::err(mv::status::internal);
  std::thread t1([&, l = std::move(ann_link)]() mutable { ann = sp::channel::open(std::move(l), true, "aaaaaaaaaaaaaaaa", "Ann"); });
  std::thread t2([&, l = std::move(mal_in)]() mutable { mal_a = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  std::thread t3([&, l = std::move(mal_out)]() mutable { mal_b = sp::channel::open(std::move(l), true, "aaaaaaaaaaaaaaaa", "Ann"); });
  std::thread t4([&, l = std::move(bea_link)]() mutable { bea = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  t1.join();
  t2.join();
  t3.join();
  t4.join();
  REQUIRE(ann);
  REQUIRE(bea);
  CHECK((*ann)->code() != (*bea)->code());  // a 1-in-a-million coincidence aside
}

TEST_CASE("sharepoint: an altered byte ends the channel", "[sharepoint]") {
  auto [a, b] = loopback_pair();
  // Past the hello (37) and the stream header (24): inside the first sealed message.
  auto tampered = std::make_unique<tamper_link>(std::move(a), 37 + 24 + 10);
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, l = std::move(b)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  auto opened = sp::channel::open(std::move(tampered), true, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  (void)opened;
  CHECK_FALSE(server);  // its first sealed message (the name) did not open
  CHECK(server.error() == mv::status::corrupt);
}

TEST_CASE("sharepoint: a key that does not match its commitment is refused", "[sharepoint]") {
  // A client that commits to one key and reveals another (what a machine in
  // the middle would need, to choose its key after seeing the server's).
  auto [a, b] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, l = std::move(b)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  std::uint8_t hello[37] = {'M', 'V', 'S', 'P', 1};
  for (int i = 5; i < 37; ++i) hello[i] = static_cast<std::uint8_t>(i);  // a commitment to nothing
  REQUIRE(a->write_all(hello));
  std::uint8_t theirs[37];
  REQUIRE(sp::read_exact(*a, theirs, 5000));
  std::uint8_t other_key[32] = {9};
  REQUIRE(a->write_all(other_key));
  t.join();
  REQUIRE_FALSE(server);
  CHECK(server.error() == mv::status::corrupt);
}

TEST_CASE("sharepoint: pings keep a link alive but never stretch a deadline", "[sharepoint]") {
  auto [a, b] = channel_pair();
  // b's pinger sends at about 2 s; a's receive with 2.5 s still ends then.
  const auto t0 = std::chrono::steady_clock::now();
  sp::message m;
  auto r = a->receive(m, 2500);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::timeout);
  CHECK(ms < 3500);
  // And with no deadline the link is still up afterwards.
  REQUIRE(b->send(sp::msg::offer, std::string_view("{}")));
  REQUIRE(a->receive(m, 2000));
  CHECK(m.type == sp::msg::offer);
}

TEST_CASE("sharepoint: another protocol version is refused at the hello", "[sharepoint]") {
  auto [a, b] = loopback_pair();
  std::uint8_t hello[37] = {'M', 'V', 'S', 'P', 9};
  REQUIRE(a->write_all(hello));
  auto server = sp::channel::open(std::move(b), false, "bbbbbbbbbbbbbbbb", "Bea");
  REQUIRE_FALSE(server);
  CHECK(server.error() == mv::status::unsupported_format);
}

// ---- the window -------------------------------------------------------------------------------

TEST_CASE("sharepoint: every state of the window is a tree the hosts accept", "[sharepoint]") {
  sp::view v;
  v.my_name = "Studio";
  std::vector<sp::view> states;
  states.push_back(v);
  v.file_names = {"IMG_1.JPG", "IMG_1.CR2"};
  v.file_count = 2;
  v.file_bytes = 30'000'000;
  v.peers = {{"0123456789abcdef", "Laptop", "Wi-Fi"}, {"fedcba9876543210", "Desk", "Bluetooth"}};
  v.selected_peer = "0123456789abcdef";
  v.wifi_up = true;
  states.push_back(v);
  for (auto phase : {sp::view::out_phase::waiting_wifi, sp::view::out_phase::connecting,
                     sp::view::out_phase::confirming, sp::view::out_phase::waiting,
                     sp::view::out_phase::sending,
                     sp::view::out_phase::done, sp::view::out_phase::ended}) {
    v.out = phase;
    v.out_peer = "Laptop";
    v.out_code = "123 456";
    v.out_note = "Sent 2 files to Laptop.";
    v.out_total = 30'000'000;
    v.out_bytes = 10'000'000;
    v.out_count = 2;
    states.push_back(v);
  }
  for (auto phase : {sp::view::in_phase::connecting, sp::view::in_phase::asking, sp::view::in_phase::receiving,
                     sp::view::in_phase::done,
                     sp::view::in_phase::ended}) {
    v.in = phase;
    v.in_peer = "Laptop";
    v.in_code = "654 321";
    v.in_via = "Wi-Fi";
    v.in_names = {"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg"};
    v.in_count = 5;
    v.in_total = 5000;
    v.in_note = "Received 5 files.";
    v.destination = "/Users/x/Pictures/MediaViewer/Received";
    states.push_back(v);
  }
  for (const auto& s : states) {
    const std::string tree = sp::build_tree(s);
    mv::addon::screen::tree t;
    INFO(tree);
    CHECK(t.set(tree) == mv::addon::screen::error::none);
  }
  // The code is on the screen while an offer waits.
  v.in = sp::view::in_phase::asking;
  CHECK(sp::build_tree(v).find("Code 654 321") != std::string::npos);
}

TEST_CASE("sharepoint: a set too big for Bluetooth says to use Wi-Fi before Send", "[sharepoint]") {
  sp::view v;
  v.my_name = "Studio";
  v.wifi_up = true;
  v.file_names = {"clip.mov"};
  v.file_count = 1;
  v.file_bytes = 340'000'000;
  v.peers = {{"0123456789abcdef", "Laptop", "Bluetooth", false, true}};
  v.selected_peer = "0123456789abcdef";
  CHECK(sp::needs_wifi(v));
  std::string tree = sp::build_tree(v);
  INFO(tree);
  CHECK(tree.find("Send to Laptop over Wi-Fi") != std::string::npos);
  CHECK(tree.find("Laptop is only in Bluetooth range") != std::string::npos);
  CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
  // No network here: the person turns on Wi-Fi on this computer.
  v.wifi_up = false;
  tree = sp::build_tree(v);
  CHECK(tree.find("turn on Wi-Fi here") != std::string::npos);
  // Small enough for Bluetooth, or the peer on Wi-Fi too: a plain Send.
  v.file_bytes = 1'000'000;
  CHECK_FALSE(sp::needs_wifi(v));
  v.file_bytes = 340'000'000;
  v.peers[0].wifi = true;
  CHECK_FALSE(sp::needs_wifi(v));
  CHECK(sp::build_tree(v).find("over Wi-Fi") == std::string::npos);
}

// ---- two and three clients ------------------------------------------------------------------------

TEST_CASE("sharepoint: Accept delivers the files with their bytes, keeping both on a name clash",
          "[sharepoint]") {
  scratch_dir s("sharepoint_send");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path dest = s / "chosen";
  fs::create_directories(dest);
  write_text(dest / "small.txt", "already here");  // a name the receiver already has

  const auto big = pattern(3 * sp::kChunkBytes + 1234, 11);
  write_bytes(s / "out" / "big.mov", big);
  write_text(s / "out" / "small.txt", "hello");
  write_bytes(s / "out" / "empty.jpg", {});
  ann.pick({s / "out" / "big.mov", s / "out" / "small.txt", s / "out" / "empty.jpg"});
  bea.save_into(dest);

  // Both screens show the code before anything about the files leaves: the
  // receiver knows only who is connecting until the sender's person confirms.
  ann.send_to(bea, false);
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::confirming; }));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  CHECK(bea.eng.snapshot().in_code == ann.eng.snapshot().out_code);
  CHECK(bea.eng.snapshot().in_count == 0);
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting; }));
  const auto asked = bea.eng.snapshot();
  CHECK(asked.in_code == ann.eng.snapshot().out_code);
  CHECK(asked.in_peer == "Ann");
  CHECK(asked.in_count == 3);
  CHECK(files_in(dest) == std::vector<std::string>{"small.txt"});  // nothing yet
  CHECK(bea.tree().find("Code " + asked.in_code) != std::string::npos);

  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  INFO(bea.eng.snapshot().in_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  CHECK(files_in(dest) == std::vector<std::string>{"big.mov", "empty.jpg", "small (2).txt", "small.txt"});
  CHECK(read_bytes(dest / "big.mov") == big);
  CHECK(read_bytes(dest / "small (2).txt") == std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
  const std::string kept = "already here";
  CHECK(read_bytes(dest / "small.txt") == std::vector<std::uint8_t>(kept.begin(), kept.end()));
  CHECK_FALSE(any_temporary(dest));
  // The originals are untouched (rule 5).
  CHECK(read_bytes(s / "out" / "big.mov") == big);
}

TEST_CASE("sharepoint: a new set to send never waits for the one still being read", "[sharepoint]") {
  // Issue #442: the share icon on Windows stalled the UI while an earlier
  // selection was still being read (set_files joined the reader on the
  // caller's thread). The caller hands the set over and returns.
  scratch_dir s("sharepoint_handoff");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  write_text(s / "one" / "a.jpg", "a");
  write_text(s / "two" / "b.jpg", "b");
  ann.host.hold_pairs.store(true);
  ann.eng.set_files({utf8(s / "one" / "a.jpg")});
  REQUIRE(eventually([&] { return ann.host.pair_calls.load() >= 1; }));  // the reader is held inside the first set
  // Under the old join this call would wait for the release below.
  std::thread release([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ann.host.hold_pairs.store(false);
  });
  const auto t0 = std::chrono::steady_clock::now();
  ann.eng.set_files({utf8(s / "two" / "b.jpg")});
  ann.eng.on_event("clear", 1, "");
  ann.eng.set_files({utf8(s / "two" / "b.jpg")});
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 100);
  CHECK(ann.eng.snapshot().reading_folder);
  release.join();
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1;
  }));
  CHECK(ann.eng.snapshot().file_names == std::vector<std::string>{"b.jpg"});  // the newest set, not the first
  CHECK(ann.host.pair_calls.load() == 2);  // the replaced middle set was never read
}

TEST_CASE("sharepoint: a pair half and a sidecar ride along; a folder keeps its shape", "[sharepoint]") {
  scratch_dir s("sharepoint_pairs");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  write_text(s / "card" / "IMG_1.JPG", "jpeg");
  write_text(s / "card" / "IMG_1.CR2", "raw");
  write_text(s / "card" / "IMG_1.xmp", "sidecar");
  write_text(s / "card" / "IMG_2.JPG", "other");
  ann.eng.set_files({utf8(s / "card" / "IMG_1.JPG")});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 3;
  }));
  auto names = ann.eng.snapshot().file_names;
  std::sort(names.begin(), names.end());
  CHECK(names == std::vector<std::string>{"IMG_1.CR2", "IMG_1.JPG", "IMG_1.xmp"});

  write_text(s / "Trip" / "a.jpg", "a");
  write_text(s / "Trip" / "Day 2" / "b.jpg", "b");
  write_text(s / "Trip" / "@eaDir" / "a.jpg@SynoEAStream", "nas");
  write_text(s / "Trip" / "Thumbs.db", "win");
  write_text(s / "Trip" / "x.jpg.mvtmp", "left over");
  ann.eng.on_event("send_folder", 2, "\"" + json_path(s / "Trip") + "\"");
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 2;
  }));
  bea.save_into(s / "bea_received");
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");  // the default folder is made on Accept
  bea.eng.wait_idle();
  ann.eng.wait_idle();
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"Trip/Day 2/b.jpg", "Trip/a.jpg"});
}

TEST_CASE("sharepoint: Decline, closing the window, and a third client receive nothing", "[sharepoint]") {
  scratch_dir s("sharepoint_decline");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  client cal(b, s / "cal_received", "Cal");
  fs::create_directories(s / "bea_received");
  fs::create_directories(s / "cal_received");
  write_text(s / "out" / "secret.jpg", "only for Bea");
  ann.pick({s / "out" / "secret.jpg"});
  REQUIRE(eventually([&] { return cal.sees(ann) && cal.sees(bea); }));

  // Decline.
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Bea declined.");
  CHECK(files_in(s / "bea_received").empty());

  // Closing the window while it asks declines too.
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.screen_closed();
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Bea declined.");
  CHECK(files_in(s / "bea_received").empty());

  // Cal heard everyone and received nothing.
  CHECK(files_in(s / "cal_received").empty());
  CHECK(cal.eng.snapshot().in == sp::view::in_phase::none);
}

TEST_CASE("sharepoint: a receiver already asking answers a second sender busy", "[sharepoint]") {
  scratch_dir s("sharepoint_busy");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  client cal(b, s / "cal_received", "Cal");
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  cal.pick({s / "out" / "a.jpg"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  cal.send_to(bea);
  cal.eng.wait_idle();
  CHECK(cal.eng.snapshot().out_note.find("receiving from someone else") != std::string::npos);
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
}

TEST_CASE("sharepoint: Cancel mid-way keeps what finished and leaves nothing partial", "[sharepoint]") {
  scratch_dir s("sharepoint_cancel");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path dest = s / "dest";
  fs::create_directories(dest);
  write_text(s / "out" / "a_first.jpg", "first");
  const auto big = pattern(16 * sp::kChunkBytes, 3);
  write_bytes(s / "out" / "b_big.mov", big);
  ann.pick({s / "out" / "a_first.jpg", s / "out" / "b_big.mov"});
  bea.save_into(dest);

  SECTION("the receiver cancels") {
    bea.host.slow.store(true);  // every chunk waits on the "viewer": the copy is slow
    ann.send_to(bea);
    REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
    bea.eng.on_event("accept", 1, "");
    REQUIRE(eventually([&] { return bea.eng.snapshot().in_bytes > sp::kChunkBytes; }));
    bea.eng.on_event("in_cancel", 1, "");
  }
  SECTION("the sender cancels") {
    ann.host.slow.store(true);
    ann.send_to(bea);
    REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
    bea.eng.on_event("accept", 1, "");
    REQUIRE(eventually([&] { return ann.eng.snapshot().out_bytes > sp::kChunkBytes; }));
    ann.eng.on_event("out_cancel", 1, "");
  }
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(bea.eng.snapshot().in_note);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::ended);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(files_in(dest) == std::vector<std::string>{"a_first.jpg"});  // whole, verified
  CHECK_FALSE(any_temporary(dest));
}

TEST_CASE("sharepoint: no room on the receiver sends nothing", "[sharepoint]") {
  scratch_dir s("sharepoint_room");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  fs::create_directories(s / "bea_received");
  bea.host.room = 1000;
  write_bytes(s / "out" / "a.mov", pattern(5000, 1));
  ann.pick({s / "out" / "a.mov"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note.find("does not have room") != std::string::npos);
  CHECK(files_in(s / "bea_received").empty());
}

TEST_CASE("sharepoint: an offer to a closed window with receiving on opens it", "[sharepoint]") {
  scratch_dir s("sharepoint_receiving");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  bea.eng.on_event("receiving", 2, "true");
  bea.eng.screen_closed();
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  {
    std::lock_guard lock(bea.host.m);
    CHECK(bea.host.shown == 1);
  }
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
}

TEST_CASE("sharepoint: with the window closed and receiving off there is no presence", "[sharepoint]") {
  scratch_dir s("sharepoint_idle");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  REQUIRE(eventually([&] { return ann.sees(bea); }));
  bea.eng.screen_closed();
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    return b.members.size() == 1;  // Bea left the bus: no beacon, no listener
  }));
  CHECK(eventually([&] { return !bea.eng.snapshot().wifi_up; }));
  REQUIRE(eventually([&] { return !ann.sees(bea); }, 10));
}

TEST_CASE("sharepoint: a peer that connects and stalls cannot hold the receiver on shutdown", "[sharepoint]") {
  scratch_dir s("sharepoint_stall");
  bus b;
  auto bea = std::make_unique<client>(b, s / "bea_received", "Bea");
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) port = m.self.port;
    return port != 0;
  }));
  // Connects, then says nothing: the handshake waits for it.
  auto stalled = sp::net::connect({"127.0.0.1", port}, 2000);
  REQUIRE(stalled);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto t0 = std::chrono::steady_clock::now();
  bea.reset();  // the add-on's shutdown
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 3000);  // not the handshake's 8 s
}

namespace {
// Beacons the test makes up, as a flood or a spoof would.
// The engine starts it on its control thread; `ready` publishes `seen` to the
// test's thread.
struct inject_discovery final : sp::discovery {
  seen_fn seen;
  std::atomic<bool> ready{false};
  bool start(const sp::beacon&, seen_fn s, tick_fn) override {
    seen = std::move(s);
    ready.store(true);
    return true;
  }
  void stop() noexcept override {}
};
}  // namespace

TEST_CASE("sharepoint: made-up beacons cannot flood the list or take a peer's address", "[sharepoint]") {
  scratch_dir s("sharepoint_flood");
  std::atomic<inject_discovery*> inject{nullptr};
  fake_host host(s / "received");
  sp::engine::options o;
  o.my_name = "Ann";
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.make_discovery = [&inject] {
    auto d = std::make_unique<inject_discovery>();
    inject = d.get();
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  sp::engine eng(host, o);
  eng.screen_opened("");
  REQUIRE(eventually([&] {
    const inject_discovery* d = inject.load();
    return d && d->ready.load();
  }));
  for (int i = 0; i < 500; ++i) {
    char id[17];
    std::snprintf(id, sizeof id, "%016x", 0x1000 + i);
    inject.load()->seen(sp::beacon{id, "Flood " + std::to_string(i), 40000}, "10.0.0.9");
  }
  REQUIRE(eventually([&] { return eng.snapshot().peers.size() == sp::kMaxPeers; }));
  CHECK(eng.snapshot().peers.size() == sp::kMaxPeers);
  // A fresh peer keeps its address: a second machine claiming its id is ignored.
  inject.load()->seen(sp::beacon{"0000000000001000", "Somebody else", 50000}, "10.0.0.66");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto v = eng.snapshot();
  const auto it = std::find_if(v.peers.begin(), v.peers.end(), [](const auto& p) { return p.id == "0000000000001000"; });
  REQUIRE(it != v.peers.end());
  CHECK(it->name == "Flood 0");
}

namespace {
// Reports one peer on "Bluetooth" a few times a second; it cannot connect.
struct fake_bluetooth final : sp::bluetooth {
  sp::bt_peer peer;
  std::atomic<bool> stop_{false};
  std::thread t;
  explicit fake_bluetooth(sp::bt_peer p) : peer(std::move(p)) {}
  ~fake_bluetooth() override { stop(); }
  bool start(const std::string&, const std::string&, peer_fn on_peer, link_fn, std::string&) override {
    stop_.store(false);
    t = std::thread([this, on_peer = std::move(on_peer)] {
      while (!stop_.load()) {
        on_peer(peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
    return true;
  }
  void stop() noexcept override {
    stop_.store(true);
    if (t.joinable()) t.join();
  }
  mv::result<std::unique_ptr<sp::link>> connect(const std::string&, int) override {
    return mv::err(mv::status::io);
  }
};

// Ann hears Bea only on Bluetooth until the test lets Bea's beacon through.
struct bt_only_sender {
  fake_host host;
  inject_discovery* inject = nullptr;
  sp::engine eng;
  bt_only_sender(const fs::path& received, const std::string& bea_id)
      : host(received), eng(host, options(bea_id)) {
    eng.screen_opened("");
  }
  sp::engine::options options(const std::string& bea_id) {
    sp::engine::options o;
    o.my_name = "Ann";
    o.loopback_only = true;
    o.make_discovery = [this] {
      auto d = std::make_unique<inject_discovery>();
      inject = d.get();
      return std::unique_ptr<sp::discovery>(std::move(d));
    };
    o.make_bluetooth = [bea_id] {
      return std::unique_ptr<sp::bluetooth>(std::make_unique<fake_bluetooth>(sp::bt_peer{"handle", bea_id, "Bea"}));
    };
    return o;
  }
};
}  // namespace

TEST_CASE("sharepoint: a big set to a Bluetooth-only peer waits for it on Wi-Fi, then sends", "[sharepoint]") {
  scratch_dir s("sharepoint_wait_wifi");
  bus b;
  client bea(b, s / "bea_received", "Bea");
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) port = m.self.port;
    return port != 0;
  }));
  bt_only_sender ann(s / "ann_received", bea.eng.id());
  // 33 MiB: over Bluetooth's 32.
  const fs::path big = s / "out" / "clip.mov";
  fs::create_directories(big.parent_path());
  { std::ofstream(big, std::ios::binary).put('x'); }
  fs::resize_file(big, 33ull * 1024 * 1024);
  ann.eng.set_files({utf8(big)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }));
  CHECK(ann.eng.snapshot().peers[0].via == "Bluetooth");
  ann.eng.on_event("peers", 4, "[\"" + bea.eng.id() + "\"]");
  CHECK(sp::needs_wifi(ann.eng.snapshot()));
  ann.eng.on_event("send", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi);  // nothing tried over Bluetooth
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);

  // Bea joins the network: her beacon arrives, and the send starts by itself.
  sp::beacon bea_beacon{bea.eng.id(), "Bea", port};
  ann.inject->seen(bea_beacon, "127.0.0.1");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::confirming; }));
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(fs::file_size(s / "bea_received" / "clip.mov") == 33ull * 1024 * 1024);
}

TEST_CASE("sharepoint: Cancel ends a send that waits for Wi-Fi", "[sharepoint]") {
  scratch_dir s("sharepoint_wait_cancel");
  bt_only_sender ann(s / "ann_received", "0123456789abcdef");
  const fs::path big = s / "out" / "clip.mov";
  fs::create_directories(big.parent_path());
  { std::ofstream(big, std::ios::binary).put('x'); }
  fs::resize_file(big, 40ull * 1024 * 1024);
  ann.eng.set_files({utf8(big)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }));
  ann.eng.on_event("peers", 4, "[\"0123456789abcdef\"]");
  ann.eng.on_event("send", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi; }));
  ann.eng.on_event("out_cancel", 1, "");
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(ann.eng.snapshot().out_note == "Cancelled. Nothing was sent.");
  // The peer appearing on Wi-Fi afterwards starts nothing.
  ann.inject->seen(sp::beacon{"0123456789abcdef", "Bea", 40000}, "127.0.0.1");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
}
