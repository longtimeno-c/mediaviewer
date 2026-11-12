// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The NAS owner's passphrase (owner_pass.h, docs/plans/nas-owner-and-guests.md
// §3): the verifier, the signed answer and its session binding, the lockout.
// The claim and the owner's session over a real service are in
// test_phone_pairing.cpp.
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

#include "driftwire_fixture.h"

#include "addons/driftwire/channel.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/owner_pass.h"
#include "addons/driftwire/secrets.h"

namespace owner = mv::driftwire::serve::owner;
namespace sp = mv::driftwire;
namespace fs = std::filesystem;

namespace {

using key32 = std::array<std::uint8_t, 32>;
using binding_t = std::array<std::uint8_t, owner::kBindingBytes>;

// The cheapest cost a client will pay, so the tests stay quick.
constexpr std::uint64_t kTestOps = owner::kMinOps;
constexpr std::uint64_t kTestMem = owner::kMinMem;

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

binding_t random_binding() {
  binding_t b;
  randombytes_buf(b.data(), b.size());
  return b;
}

}  // namespace

TEST_CASE("serve owner: a passphrase is kept only as a public key, and signs for one session", "[driftwire-serve]") {
  REQUIRE(sodium_init() >= 0);
  owner::signer s;
  const auto v = owner::make("correct horse battery", 1000, kTestOps, kTestMem, &s);
  REQUIRE(v);
  CHECK(v->pk == s.pk);
  // The record holds the salt, the cost and the public key, never the passphrase.
  const std::string text = owner::verifier_json(*v);
  CHECK(text.find("correct horse") == std::string::npos);
  const auto back = owner::parse_verifier(text);
  REQUIRE(back);
  CHECK(back->pk == v->pk);
  CHECK(back->salt == v->salt);
  CHECK(back->set == 1000);

  // The same passphrase and salt make the same key; another passphrase does not.
  const auto again = owner::derive("correct horse battery", v->salt, v->ops, v->mem);
  REQUIRE(again);
  CHECK(again->pk == v->pk);
  const auto other = owner::derive("correct horse battery!", v->salt, v->ops, v->mem);
  REQUIRE(other);
  CHECK(other->pk != v->pk);

  const binding_t binding = random_binding();
  const key32 nonce = random_key(), service = random_key(), client = random_key();
  const auto sig = owner::sign(s, binding, nonce, service, client);
  CHECK(owner::check(*v, sig, binding, nonce, service, client));
  // Anything else bound into it and the answer is worthless: another session,
  // another nonce, another service, another device, another passphrase.
  CHECK_FALSE(owner::check(*v, sig, random_binding(), nonce, service, client));
  CHECK_FALSE(owner::check(*v, sig, binding, random_key(), service, client));
  CHECK_FALSE(owner::check(*v, sig, binding, nonce, random_key(), client));
  CHECK_FALSE(owner::check(*v, sig, binding, nonce, service, random_key()));
  CHECK_FALSE(owner::check(*v, owner::sign(*other, binding, nonce, service, client), binding, nonce, service, client));
}

TEST_CASE("serve owner: short passphrases and unpayable costs are refused", "[driftwire-serve]") {
  CHECK_FALSE(owner::acceptable("short"));
  CHECK_FALSE(owner::acceptable("123456789"));
  CHECK(owner::acceptable("1234567890"));
  CHECK(owner::acceptable("ééééééééé\xc3\xa9"));        // ten code points in twenty bytes
  CHECK_FALSE(owner::acceptable("éééééééé"));           // eight code points in sixteen bytes
  CHECK_FALSE(owner::acceptable("abcdefghi\xff"));      // not UTF-8
  CHECK_FALSE(owner::acceptable(std::string(owner::kMaxBytes + 1, 'a')));
  CHECK_FALSE(owner::make("short", 0, kTestOps, kTestMem));
  // A tampered record cannot make a client spend a gigabyte, or accept a toy cost.
  CHECK_FALSE(owner::sane_cost(1, kTestMem));
  CHECK_FALSE(owner::sane_cost(kTestOps, owner::kMaxMem * 2));
  CHECK_FALSE(owner::sane_cost(kTestOps, 1024));
  CHECK_FALSE(owner::derive("1234567890", owner::salt_t{}, 1, kTestMem));
  owner::verifier v;
  v.ops = 1;
  CHECK_FALSE(owner::parse_verifier(owner::verifier_json(v)));
  CHECK_FALSE(owner::parse_verifier("{\"v\":1}"));
  CHECK_FALSE(owner::parse_verifier("not json"));
}

TEST_CASE("serve owner: five wrong answers are free, then the lock doubles to a day", "[driftwire-serve]") {
  owner::counter c;
  const std::int64_t t = 1'000'000;
  for (int i = 0; i < owner::kFreeMisses; ++i) {
    c.miss(t);
    CHECK_FALSE(c.locked(t));
  }
  c.miss(t);
  CHECK(c.locked(t));
  CHECK(c.until == t + owner::kFirstLockSeconds);
  c.miss(t);
  CHECK(c.until == t + 2 * owner::kFirstLockSeconds);
  for (int i = 0; i < 20; ++i) c.miss(t);
  CHECK(c.until == t + owner::kMaxLockSeconds);
  CHECK_FALSE(c.locked(c.until));
  c.success();
  CHECK(c.misses == 0);
  CHECK_FALSE(c.locked(t));
}

TEST_CASE("serve owner: the store keeps the verifier and the lockout, and never fails open", "[driftwire-serve]") {
  scratch_dir dir{"serve_owner"};
  REQUIRE(sp::secrets::owner_only_folder(utf8(dir.root() / "s")));
  auto store = sp::secrets::file_store(utf8(dir.root() / "s"));
  REQUIRE(store);
  owner::verifier v;
  CHECK(owner::load(*store, v) == owner::stored::none);
  const auto made = owner::make("a long passphrase", 5, kTestOps, kTestMem);
  REQUIRE(made);
  REQUIRE(owner::save(*store, *made));
  REQUIRE(owner::load(*store, v) == owner::stored::set);
  CHECK(v.pk == made->pk);

  // A broken record is never "none": that would let the next claim choose it.
  const std::string junk = "{\"v\":1,\"alg\":\"argon2id13+ed25519\"}";
  REQUIRE(store->put(owner::kPassphraseName, std::span(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size())));
  CHECK(owner::load(*store, v) == owner::stored::unreadable);

  // The lockout survives, and a broken one is a lock, not a clean slate.
  owner::lockout l;
  for (int i = 0; i <= owner::kFreeMisses; ++i) l.owner.miss(100);
  REQUIRE(owner::save_lockout(*store, l));
  const auto read = owner::load_lockout(*store, 100);
  CHECK(read.owner.locked(100));
  CHECK_FALSE(read.claim.locked(100));
  REQUIRE(store->put(owner::kLockoutName, std::span(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size())));
  const auto broken = owner::load_lockout(*store, 100);
  CHECK(broken.owner.locked(100));
  CHECK(broken.claim.locked(100));

  // Reset clears both.
  REQUIRE(owner::reset(*store));
  CHECK(owner::load(*store, v) == owner::stored::none);
  CHECK_FALSE(owner::load_lockout(*store, 100).owner.locked(100));
}

TEST_CASE("serve owner: a library channel's handshake hash is shared by its ends and new each session", "[driftwire-serve]") {
  REQUIRE(sodium_init() >= 0);
  auto listen = sp::net::listen_internet(0, true);
  REQUIRE(listen);
  const key32 access = random_key(), service = random_key(), device = random_key();
  const auto one = [&] {
    std::unique_ptr<sp::channel> server_side;
    // No REQUIRE off the test's thread: it would end the run, not fail the test.
    std::thread t([&] {
      auto l = (*listen)->accept(5000);
      if (!l) return;
      std::shared_ptr<sp::link> shared(std::move(*l));
      auto h = sp::channel::read_library_hello(*shared, std::chrono::steady_clock::now() + std::chrono::seconds(5));
      if (!h) return;
      sp::channel::options o;
      o.access_key = access;
      o.static_key = service;
      auto ch = sp::channel::open_library_server(shared, std::move(*h), "0000000000000000", "svc", o);
      if (ch) server_side = std::move(*ch);
    });
    auto l = sp::net::connect({"::1", (*listen)->port()}, 3000);
    REQUIRE(l);
    sp::channel::options o;
    o.access_key = access;
    o.static_key = device;
    auto client = sp::channel::open_library(std::shared_ptr<sp::link>(std::move(*l)), "1111111111111111", "phone", o);
    t.join();
    REQUIRE(client);
    REQUIRE(server_side);
    CHECK((*client)->handshake_hash() == server_side->handshake_hash());
    CHECK((*client)->handshake_hash() != binding_t{});
    // The secret Remember used is a constant on a library channel: never a binding.
    CHECK((*client)->pairing_secret() == server_side->pairing_secret());
    const auto h = (*client)->handshake_hash();
    const auto p = (*client)->pairing_secret();
    (*client)->shutdown();
    server_side->shutdown();
    return std::make_pair(h, p);
  };
  const auto a = one();
  const auto b = one();
  CHECK(a.first != b.first);
  CHECK(a.second == b.second);  // why pairing_secret() could not be used (channel.cpp, library channels)
}

#if !defined(_WIN32)
TEST_CASE("serve owner: the command line sets, shows and resets the passphrase, and never keeps it", "[driftwire-serve]") {
  scratch_dir dir{"serve_owner_cli"};
  const fs::path data = dir.root() / "svc";
  fs::create_directories(data);
  const std::string base = std::string("'") + MV_DRIFTWIRE_CLI_TEST_PATH + "' serve --test-data '" + utf8(data) + "' ";
  // `input` goes to stdin (a pipe, never a terminal).
  const auto run = [&](const std::string& rest, const std::string& input, int& code) {
    const fs::path in = dir.root() / "stdin.txt";
    {
      std::ofstream f(in, std::ios::binary | std::ios::trunc);
      f << input;
    }
    FILE* p = popen((base + rest + " <'" + utf8(in) + "' 2>&1").c_str(), "r");
    REQUIRE(p);
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, p)) out += buf;
    code = pclose(p);
    return out;
  };
  int code = 0;
  // With no terminal, init asks to be told how to get one, and makes nothing.
  std::string out = run("init", "", code);
  CHECK(code != 0);
  CHECK(out.find("--passphrase-stdin") != std::string::npos);
  CHECK_FALSE(fs::exists(data / "secrets" / "identity"));
  // Too short: nothing made either.
  out = run("init --passphrase-stdin", "short\n", code);
  CHECK(code != 0);
  CHECK_FALSE(fs::exists(data / "secrets" / "identity"));
  out = run("init --passphrase-stdin", "a long owner passphrase\n", code);
  CHECK(code == 0);
  CHECK(out.find("passphrase set") != std::string::npos);
  CHECK(out.find("access-key ") != std::string::npos);
  // Run again, it never replaces the passphrase.
  out = run("init --passphrase-stdin", "another long passphrase\n", code);
  CHECK(code == 0);
  auto store = sp::secrets::file_store(utf8(data / "secrets"));
  REQUIRE(store);
  owner::verifier first;
  REQUIRE(owner::load(*store, first) == owner::stored::set);
  CHECK(owner::derive("a long owner passphrase", first.salt, first.ops, first.mem)->pk == first.pk);
  // Nothing on disk is the passphrase.
  for (const auto& e : fs::recursive_directory_iterator(data)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("a long owner passphrase") == std::string::npos);
  }
  out = run("status", "", code);
  CHECK(code == 0);
  CHECK(out.find("passphrase set") != std::string::npos);
  CHECK(out.find("identity ") != std::string::npos);
  CHECK(out.find("lockout owner 0 wrong") != std::string::npos);
  // A lockout, then the recovery: reset with a new one clears both.
  owner::lockout l;
  for (int i = 0; i <= owner::kFreeMisses; ++i) l.owner.miss(std::time(nullptr));
  REQUIRE(owner::save_lockout(*store, l));
  CHECK(run("status", "", code).find("lockout owner until") != std::string::npos);
  out = run("passphrase reset --passphrase-stdin", "the new owner passphrase\n", code);
  CHECK(code == 0);
  CHECK(out.find("lockouts cleared") != std::string::npos);
  owner::verifier second;
  REQUIRE(owner::load(*store, second) == owner::stored::set);
  CHECK(second.pk != first.pk);
  CHECK(run("status", "", code).find("lockout owner 0 wrong") != std::string::npos);
  // Reset never leaves none behind: with none, the next claim would choose it.
  out = run("passphrase reset --no-passphrase", "", code);
  CHECK(code != 0);
  out = run("passphrase reset --passphrase-stdin", "short\n", code);
  CHECK(code != 0);
  CHECK(owner::load(*store, second) == owner::stored::set);
  // The flags go only where a passphrase is asked for.
  out = run("devices --no-passphrase", "", code);
  CHECK(code != 0);
}
#endif
