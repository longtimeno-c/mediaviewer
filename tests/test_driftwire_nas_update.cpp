// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The NAS service updating itself (cli/nas_update.h, docs/plans/nas-setup.md
// §6): the signed offer, and staging that changes nothing unless the download
// is what was signed and runs. No network: the fetch and the self-test are the
// test's. Filter with "[nas-update]".
#include "catch_compat.h"

#include <sodium.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "addons/driftwire/cli/nas_update.h"
#include "core/json.h"

namespace nu = mv::driftwire::cli::nas_update;
namespace fs = std::filesystem;

namespace {

struct keypair {
  std::array<std::uint8_t, 32> pub{};
  std::array<std::uint8_t, 64> sec{};
  keypair() {
    REQUIRE(sodium_init() >= 0);
    crypto_sign_keypair(pub.data(), sec.data());
  }
};

std::string hex(const unsigned char* p, std::size_t n) {
  std::string out(2 * n + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), p, n);
  out.pop_back();
  return out;
}

std::string sha_of(const std::string& bytes) {
  unsigned char out[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(out, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  return hex(out, sizeof out);
}

std::string sign(const keypair& k, const nu::offer& o) {
  const std::string m = nu::signed_message(o);
  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(m.data()), m.size(), k.sec.data());
  return hex(sig, sizeof sig);
}

std::string latest(const nu::offer& o, const std::string& sig) {
  mv::json::writer w;
  w.begin_object().key("version").string(o.version).key("arch").begin_object();
  w.key(o.arch).begin_object()
      .key("url").string(o.url)
      .key("sha256").string(o.sha256)
      .key("size").integer(static_cast<std::int64_t>(o.size))
      .key("sig").string(sig)
      .end_object();
  w.end_object().end_object();
  return w.take();
}

struct scratch {
  fs::path root;
  scratch() {
    std::random_device rd;
    root = fs::temp_directory_path() / ("mv_nas_update_" + std::to_string(rd()));
    fs::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  std::string var() const { return (root / "var").string(); }
};

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

TEST_CASE("nas update: only an offer signed by the pinned key, and newer, is taken", "[nas-update]") {
  keypair k, other;
  nu::offer o{"1.4.0", "x86_64", "https://example.com/nas/driftwire-1.4.0-x86_64", sha_of("new"), 3};
  const std::string good = latest(o, sign(k, o));
  auto got = nu::verify_latest(good, "x86_64", k.pub, "1.3.9");
  REQUIRE(got);
  CHECK(got->url == o.url);
  CHECK(got->sha256 == o.sha256);
  CHECK(nu::verify_latest(good, "x86_64", k.pub, "1.4.0").error() == mv::status::not_found);  // not newer
  CHECK(nu::verify_latest(good, "aarch64", k.pub, "1.0.0").error() == mv::status::not_found);  // not for this arch
  CHECK(nu::verify_latest(good, "x86_64", other.pub, "1.0.0").error() == mv::status::permission_denied);
  // Any signed field changed: the signature no longer holds.
  nu::offer moved = o;
  moved.url = "https://example.com/other";
  CHECK(nu::verify_latest(latest(moved, sign(k, o)), "x86_64", k.pub, "1.0.0").error() == mv::status::permission_denied);
  nu::offer bigger = o;
  bigger.size = 4;
  CHECK(nu::verify_latest(latest(bigger, sign(k, o)), "x86_64", k.pub, "1.0.0").error() == mv::status::permission_denied);
  // Not an offer at all, or a URL that is not HTTPS.
  CHECK(nu::verify_latest("{}", "x86_64", k.pub, "1.0.0").error() == mv::status::corrupt);
  nu::offer plain = o;
  plain.url = "http://example.com/x";
  CHECK(nu::verify_latest(latest(plain, sign(k, plain)), "x86_64", k.pub, "1.0.0").error() == mv::status::corrupt);
}

TEST_CASE("nas update: a download that is not what was signed is never run or kept", "[nas-update]") {
  scratch s;
  const std::string bytes = "#!/bin/sh\necho new\n";
  nu::offer o{"1.4.0", "x86_64", "https://example.com/x", sha_of(bytes), bytes.size()};
  int tests = 0;
  nu::io io;
  io.self_test = [&](const std::string&) {
    ++tests;
    return true;
  };
  io.fetch = [&](const std::string&, const std::string& to) -> mv::expected {
    std::ofstream(to, std::ios::binary) << "#!/bin/sh\necho evil\n";  // same size, other bytes
    return {};
  };
  CHECK(nu::install(o, s.var(), io).error() == mv::status::corrupt);
  CHECK(tests == 0);
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "bin" / "driftwire"));
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "stage"));
  // A fetch that fails changes nothing either.
  io.fetch = [](const std::string&, const std::string&) -> mv::expected { return mv::err(mv::status::io); };
  CHECK(nu::install(o, s.var(), io).error() == mv::status::io);
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "bin" / "driftwire"));
}

TEST_CASE("nas update: a binary that fails its self-test is not installed", "[nas-update]") {
  scratch s;
  const std::string bytes = "new";
  nu::offer o{"1.4.0", "x86_64", "https://example.com/x", sha_of(bytes), bytes.size()};
  fs::create_directories(fs::path(s.var()) / "bin");
  std::ofstream(fs::path(s.var()) / "bin" / "driftwire", std::ios::binary) << "old";
  nu::io io;
  io.fetch = [&](const std::string&, const std::string& to) -> mv::expected {
    std::ofstream(to, std::ios::binary) << bytes;
    return {};
  };
  io.self_test = [](const std::string&) { return false; };
  CHECK(nu::install(o, s.var(), io).error() == mv::status::cancelled);
  CHECK(read(fs::path(s.var()) / "bin" / "driftwire") == "old");
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "state" / "trial"));
}

TEST_CASE("nas update: a good one is installed, the one before kept, and a trial begun", "[nas-update]") {
  scratch s;
  const std::string bytes = "new";
  nu::offer o{"1.4.0", "x86_64", "https://example.com/x", sha_of(bytes), bytes.size()};
  fs::create_directories(fs::path(s.var()) / "bin");
  fs::create_directories(fs::path(s.var()) / "serve");
  std::ofstream(fs::path(s.var()) / "bin" / "driftwire", std::ios::binary) << "old";
  std::ofstream(fs::path(s.var()) / "serve" / "serve.json", std::ios::binary) << "{\"port\":7878}";
  std::string tested;
  nu::io io;
  io.fetch = [&](const std::string& url, const std::string& to) -> mv::expected {
    CHECK(url == o.url);
    std::ofstream(to, std::ios::binary) << bytes;
    return {};
  };
  io.self_test = [&](const std::string& bin) {
    tested = read(bin);
    return true;
  };
  REQUIRE(nu::install(o, s.var(), io));
  CHECK(tested == "new");  // the self-test ran the staged bytes
  CHECK(read(fs::path(s.var()) / "bin" / "driftwire") == "new");
  CHECK(read(fs::path(s.var()) / "bin" / "driftwire.prev") == "old");
  CHECK(read(fs::path(s.var()) / "state" / "trial") == "1.4.0\n0\n");
  CHECK(read(fs::path(s.var()) / "serve" / "serve.json") == "{\"port\":7878}");  // pairings untouched
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "stage"));
  nu::trial_passed(s.var());
  CHECK_FALSE(fs::exists(fs::path(s.var()) / "state" / "trial"));
}

TEST_CASE("nas update: a version rolled back from is skipped until a newer one", "[nas-update]") {
  scratch s;
  fs::create_directories(fs::path(s.var()) / "state");
  CHECK_FALSE(nu::skipped(s.var(), "1.4.0"));
  std::ofstream(fs::path(s.var()) / "state" / "skip") << "1.4.0\n";
  CHECK(nu::skipped(s.var(), "1.4.0"));
  CHECK(nu::skipped(s.var(), "1.3.0"));
  CHECK_FALSE(nu::skipped(s.var(), "1.4.1"));
}
