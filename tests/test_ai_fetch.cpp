// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #482 T3: the model downloader (src/addons/ai/fetch.h) against a fake
// transport: resume after a cut, wait out an outage by itself, a 404 is final,
// a file that misses its hash is fetched again and never kept, a server that
// ignores Range, stop, and prune. Plus SHA-256's FIPS vectors, and for
// Enhance (docs/plans/enhance.md, E1) the per-file cap and adoption of a file
// already at its pinned path.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <string>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/ai/sha256.h"

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A UTF-8 path as a filesystem path (one string: never iterators of two).
fs::path native(const std::string& u) { return fs::path(std::u8string(u.begin(), u.end())); }

std::string hex_of(const std::vector<std::uint8_t>& bytes) {
  mv::ai::sha256 h;
  h.update(bytes);
  return h.finish_hex();
}

// Serves files by URL; `script` decides what each request does.
struct fake_net final : mv::ai::http_transport {
  struct call {
    std::string url;
    std::uint64_t from;
  };
  std::map<std::string, std::vector<std::uint8_t>> files;
  std::vector<call> calls;
  // Per request (0-based): the status to answer and how many body bytes to
  // send before cutting; a negative cut sends everything.
  std::function<std::pair<int, long long>(std::size_t n, std::uint64_t from)> script =
      [](std::size_t, std::uint64_t from) { return std::pair<int, long long>{from ? 206 : 200, -1}; };
  bool ignore_range = false;

  mv::ai::http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int)>& head,
                         const std::function<bool(std::span<const std::uint8_t>)>& body) override {
    const std::size_t n = calls.size();
    calls.push_back({url, from});
    auto [code, cut] = script(n, from);
    mv::ai::http_reply r;
    if (code == 0) {
      r.error = "offline";
      return r;
    }
    if (ignore_range && code == 206) code = 200;
    r.status = code;
    if (!head(code) || (code != 200 && code != 206)) return r;
    const auto& data = files.at(url);
    const std::uint64_t start = code == 206 ? from : 0;
    std::uint64_t end = data.size();
    if (cut >= 0) end = std::min<std::uint64_t>(end, start + static_cast<std::uint64_t>(cut));
    for (std::uint64_t at = start; at < end; at += 7) {
      const std::uint64_t k = std::min<std::uint64_t>(7, end - at);
      if (!body(std::span<const std::uint8_t>(data.data() + at, k))) break;
    }
    return r;
  }
};

struct scratch {
  fs::path root = fs::temp_directory_path() / "mv_ai_fetch_test";
  scratch() {
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

std::vector<std::uint8_t> bytes(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31);
  return v;
}

mv::ai::remote_file file_of(fake_net& net, const std::string& name, std::vector<std::uint8_t> data) {
  mv::ai::remote_file f;
  f.url = "https://huggingface.co/x/resolve/" + std::string(40, 'a') + "/" + name;
  f.path = "models/m/" + name;
  f.sha256 = hex_of(data);
  f.size = data.size();
  net.files[f.url] = std::move(data);
  return f;
}

// Writes `data` at a UTF-8 path, making its folder: a file put there by hand.
void put(const std::string& path, const std::vector<std::uint8_t>& data) {
  fs::create_directories(native(path).parent_path());
  std::ofstream(native(path), std::ios::binary)
      .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<double> waits;
mv::ai::fetch_options quick() {
  mv::ai::fetch_options o;
  o.wait = [](double s, const std::atomic<bool>& stop) {
    waits.push_back(s);
    return !stop.load();
  };
  return o;
}

}  // namespace

TEST_CASE("SHA-256 matches FIPS 180-4", "[ai][fetch]") {
  CHECK(hex_of({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string abc = "abc";
  CHECK(hex_of({abc.begin(), abc.end()}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(hex_of({two.begin(), two.end()}) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  mv::ai::sha256 h;  // a million 'a's, fed unevenly
  const std::vector<std::uint8_t> a(1000, 'a');
  for (int i = 0; i < 1000; ++i) h.update(std::span<const std::uint8_t>(a.data(), i % 2 ? 1000 : 1000));
  CHECK(h.finish_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("the downloader fetches, verifies, and does not fetch twice", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(1000, 1));
  const auto b = file_of(net, "b.onnx", bytes(333, 2));
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  std::vector<mv::ai::fetch_progress> seen;
  const std::vector<mv::ai::remote_file> both{a, b};
  REQUIRE(f.run(both, stop, [&](const mv::ai::fetch_progress& p) { seen.push_back(p); }));
  CHECK(f.ready(a));
  CHECK(f.ready(b));
  CHECK(seen.back().state == mv::ai::fetch_state::done);
  CHECK(seen.back().done_bytes == 1333);
  CHECK(seen.back().files_done == 2);
  CHECK(net.calls.size() == 2);
  REQUIRE(f.run(both, stop, {}));
  CHECK(net.calls.size() == 2);  // ready: not asked again
  // A file changed on disk is not ready: fetched again.
  std::ofstream(native(f.local_path(b)), std::ios::binary | std::ios::app)
      << "x";
  CHECK_FALSE(f.ready(b));
}

TEST_CASE("an outage and a cut transfer resume by themselves", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(1000, 3));
  // Cut at 400 bytes, then offline twice, then a 503, then the rest.
  net.script = [](std::size_t n, std::uint64_t from) -> std::pair<int, long long> {
    if (n == 0) return {200, 400};
    if (n == 1 || n == 2) return {0, 0};
    if (n == 3) return {503, 0};
    return {from ? 206 : 200, -1};
  };
  waits.clear();
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  bool waited = false;
  const std::vector<mv::ai::remote_file> one{a};
  REQUIRE(f.run(one, stop, [&](const mv::ai::fetch_progress& p) {
    waited = waited || p.state == mv::ai::fetch_state::waiting;
  }));
  CHECK(waited);
  CHECK(f.ready(a));
  REQUIRE(net.calls.size() == 5);
  CHECK(net.calls[1].from == 400);  // asked for the rest, not the whole file
  CHECK(net.calls[4].from == 400);
  // Backs off from the first wait, doubling while nothing answers.
  REQUIRE(waits.size() == 4);
  CHECK(waits[0] == 2);
  CHECK(waits[1] == 4);
  CHECK(waits[2] == 8);
  CHECK(waits[3] == 16);
}

TEST_CASE("a part left by an earlier run is resumed", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto data = bytes(900, 4);
  const auto a = file_of(net, "a.onnx", data);
  mv::ai::fetcher f(utf8(s.root), net, quick());
  const std::string part = f.local_path(a) + ".part";
  fs::create_directories(fs::path(std::u8string(part.begin(), part.end())).parent_path());
  std::ofstream(fs::path(std::u8string(part.begin(), part.end())), std::ios::binary)
      .write(reinterpret_cast<const char*>(data.data()), 500);
  std::atomic<bool> stop{false};
  const std::vector<mv::ai::remote_file> one{a};
  REQUIRE(f.run(one, stop, {}));
  REQUIRE(net.calls.size() == 1);
  CHECK(net.calls[0].from == 500);
  CHECK(f.ready(a));
}

TEST_CASE("a server that ignores Range still ends with the right file", "[ai][fetch]") {
  scratch s;
  fake_net net;
  net.ignore_range = true;
  const auto a = file_of(net, "a.onnx", bytes(600, 5));
  net.script = [](std::size_t n, std::uint64_t from) -> std::pair<int, long long> {
    if (n == 0) return {200, 250};
    return {from ? 206 : 200, -1};
  };
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  const std::vector<mv::ai::remote_file> one{a};
  REQUIRE(f.run(one, stop, {}));
  CHECK(f.ready(a));
}

TEST_CASE("a pinned file that is gone is reported, not retried forever", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(100, 6));
  net.script = [](std::size_t, std::uint64_t) { return std::pair<int, long long>{404, 0}; };
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  mv::ai::fetch_state last{};
  const std::vector<mv::ai::remote_file> one{a};
  auto r = f.run(one, stop, [&](const mv::ai::fetch_progress& p) { last = p.state; });
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::not_found);
  CHECK(last == mv::ai::fetch_state::gone);
  CHECK(net.calls.size() == 1);
}

TEST_CASE("bytes that miss the hash are deleted and fetched again, never kept", "[ai][fetch]") {
  scratch s;
  fake_net net;
  auto a = file_of(net, "a.onnx", bytes(200, 7));
  net.files[a.url][50] ^= 0xFF;  // the mirror serves a wrong byte
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  const std::vector<mv::ai::remote_file> one{a};
  auto r = f.run(one, stop, {});
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::corrupt);
  CHECK(net.calls.size() == 3);  // hash_attempts
  CHECK_FALSE(f.ready(a));
  CHECK_FALSE(fs::exists(native(f.local_path(a))));
  // Fixed upstream: the next run gets it.
  net.files[a.url][50] ^= 0xFF;
  REQUIRE(f.run(one, stop, {}));
  CHECK(f.ready(a));
}

TEST_CASE("stop keeps the part; prune keeps only what is wanted", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(700, 8));
  const auto b = file_of(net, "b.onnx", bytes(70, 9));
  std::atomic<bool> stop{false};
  net.script = [&](std::size_t, std::uint64_t from) -> std::pair<int, long long> {
    stop = true;  // the user cancels mid-file
    return {from ? 206 : 200, 300};
  };
  mv::ai::fetcher f(utf8(s.root), net, quick());
  const std::vector<mv::ai::remote_file> one{a};
  auto r = f.run(one, stop, {});
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::cancelled);
  stop = false;
  net.script = [](std::size_t, std::uint64_t from) { return std::pair<int, long long>{from ? 206 : 200, -1}; };
  const std::vector<mv::ai::remote_file> both{a, b};
  REQUIRE(f.run(both, stop, {}));
  // Prune to b: a and its record go, b stays.
  const std::vector<mv::ai::remote_file> keep{b};
  f.prune(keep);
  CHECK_FALSE(f.ready(a));
  CHECK(f.ready(b));
}

TEST_CASE("a pin over max_file_bytes is refused before any request", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(100, 10));
  const auto b = file_of(net, "b.onnx", bytes(1000, 11));
  const fs::path root = s.root / "cache";  // not made yet
  auto o = quick();
  o.max_file_bytes = 999;
  mv::ai::fetcher f(utf8(root), net, o);
  std::atomic<bool> stop{false};
  bool reported = false;
  // The small file first: it is not fetched either, the list is refused whole.
  const std::vector<mv::ai::remote_file> both{a, b};
  auto r = f.run(both, stop, [&](const mv::ai::fetch_progress&) { reported = true; });
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::invalid_arg);
  CHECK(net.calls.empty());
  CHECK_FALSE(reported);
  CHECK_FALSE(fs::exists(root));
  CHECK(fs::is_empty(s.root));
  // The same list under a cap it fits: fetched.
  o.max_file_bytes = 1000;
  mv::ai::fetcher g(utf8(root), net, o);
  REQUIRE(g.run(both, stop, {}));
  CHECK(net.calls.size() == 2);
  CHECK(g.ready(a));
  CHECK(g.ready(b));
}

TEST_CASE("a file dropped at its pinned path is adopted by its hash", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto data = bytes(800, 12);
  const auto a = file_of(net, "a.onnx", data);
  mv::ai::fetcher f(utf8(s.root), net, quick());
  const std::string at = f.local_path(a);
  put(at, data);
  CHECK_FALSE(f.ready(a));  // no .ok yet
  const std::vector<mv::ai::remote_file> one{a};
  // Stopped: nothing is hashed or recorded.
  std::atomic<bool> stop{true};
  auto r = f.run(one, stop, {});
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::cancelled);
  CHECK_FALSE(fs::exists(native(at + ".ok")));
  stop = false;
  std::vector<mv::ai::fetch_progress> seen;
  REQUIRE(f.run(one, stop, [&](const mv::ai::fetch_progress& p) { seen.push_back(p); }));
  CHECK(net.calls.empty());
  CHECK(fs::exists(native(at + ".ok")));
  CHECK(f.ready(a));
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().state == mv::ai::fetch_state::done);
  CHECK(seen.back().files_done == 1);
  CHECK(seen.back().done_bytes == 800);
  // Ready from then on: the next run stats it.
  REQUIRE(f.run(one, stop, {}));
  CHECK(net.calls.empty());
}

TEST_CASE("wrong bytes of the right size at the pinned path are replaced by a download", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto data = bytes(500, 13);
  const auto a = file_of(net, "a.onnx", data);
  mv::ai::fetcher f(utf8(s.root), net, quick());
  auto wrong = data;
  wrong[100] ^= 0xFF;
  put(f.local_path(a), wrong);
  std::atomic<bool> stop{false};
  const std::vector<mv::ai::remote_file> one{a};
  REQUIRE(f.run(one, stop, {}));
  REQUIRE(net.calls.size() == 1);
  CHECK(net.calls[0].from == 0);
  CHECK(f.ready(a));
  const auto hash = mv::ai::sha256_file_hex(f.local_path(a));
  REQUIRE(hash);
  CHECK(*hash == a.sha256);
}

TEST_CASE("a verified file whose mtime changed is hashed again and kept", "[ai][fetch]") {
  scratch s;
  fake_net net;
  const auto a = file_of(net, "a.onnx", bytes(600, 14));
  mv::ai::fetcher f(utf8(s.root), net, quick());
  std::atomic<bool> stop{false};
  const std::vector<mv::ai::remote_file> one{a};
  REQUIRE(f.run(one, stop, {}));
  REQUIRE(net.calls.size() == 1);
  // Copied back from a backup: the same bytes, a day older.
  const fs::path at = native(f.local_path(a));
  fs::last_write_time(at, fs::last_write_time(at) - std::chrono::hours(24));
  CHECK_FALSE(f.ready(a));  // the .ok no longer agrees
  REQUIRE(f.run(one, stop, {}));
  CHECK(net.calls.size() == 1);  // kept, not fetched again
  CHECK(f.ready(a));
}
