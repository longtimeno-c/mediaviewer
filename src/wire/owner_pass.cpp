// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/owner_pass.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/json.h"

namespace mv::driftwire::serve::owner {

namespace {

constexpr char kSeedLabel[] = "mv.serve.owner.seed.v1";
constexpr char kSignLabel[] = "mv.serve.owner.v1";

template <std::size_t N>
std::string hex(const std::array<std::uint8_t, N>& b) {
  std::string out(N * 2, '0');
  sodium_bin2hex(out.data(), out.size() + 1, b.data(), b.size());
  return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> unhex(const std::string* s) {
  if (!s || s->size() != N * 2) return std::nullopt;
  std::array<std::uint8_t, N> out{};
  std::size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), s->data(), s->size(), nullptr, &len, nullptr) != 0 || len != N) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::uint64_t> positive(const json::value& doc, std::string_view key) {
  const auto v = doc.integer(key);
  if (!v || *v <= 0) return std::nullopt;
  return static_cast<std::uint64_t>(*v);
}

std::optional<counter> parse_counter(const json::value* v) {
  if (!v || v->k != json::kind::object) return std::nullopt;
  const auto misses = v->integer("misses");
  const auto until = v->integer("until");
  const auto level = v->integer("level");
  if (!misses || !until || !level || *misses < 0 || *misses > 1000000 || *level < 0 || *level > 64) return std::nullopt;
  counter c;
  c.misses = static_cast<int>(*misses);
  c.until = *until;
  c.level = static_cast<int>(*level);
  return c;
}

void write_counter(json::writer& w, const counter& c) {
  w.begin_object().key("misses").integer(c.misses).key("until").integer(c.until).key("level").integer(c.level).end_object();
}

}  // namespace

signer::~signer() { sodium_memzero(sk.data(), sk.size()); }

bool sane_cost(std::uint64_t ops, std::uint64_t mem) noexcept {
  return ops >= kMinOps && ops <= kMaxOps && mem >= kMinMem && mem <= kMaxMem;
}

std::string verifier_json(const verifier& v) {
  json::writer w;
  w.begin_object()
      .key("v").integer(1)
      .key("alg").string("argon2id13+ed25519")
      .key("ops").integer(static_cast<std::int64_t>(v.ops))
      .key("mem").integer(static_cast<std::int64_t>(v.mem))
      .key("salt").string(hex(v.salt))
      .key("pk").string(hex(v.pk))
      .key("set").integer(v.set)
      .end_object();
  return w.take();
}

std::optional<verifier> parse_verifier(std::string_view text) {
  if (text.size() > 4096) return std::nullopt;
  const auto doc = json::parse(text, 2);
  if (!doc || doc->integer("v") != 1) return std::nullopt;
  const std::string* alg = doc->str("alg");
  if (!alg || *alg != "argon2id13+ed25519") return std::nullopt;
  const auto ops = positive(*doc, "ops");
  const auto mem = positive(*doc, "mem");
  const auto salt = unhex<kSaltBytes>(doc->str("salt"));
  const auto pk = unhex<32>(doc->str("pk"));
  const auto set = doc->integer("set");
  if (!ops || !mem || !sane_cost(*ops, *mem) || !salt || !pk || !set) return std::nullopt;
  verifier v;
  v.ops = *ops;
  v.mem = *mem;
  v.salt = *salt;
  v.pk = *pk;
  v.set = *set;
  return v;
}

bool acceptable(std::string_view p) noexcept {
  if (p.size() > kMaxBytes || !json::detail::valid_utf8(p)) return false;
  const auto points = static_cast<std::size_t>(
      std::count_if(p.begin(), p.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
  return points >= kMinChars;
}

std::optional<signer> derive(std::string_view passphrase, const salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
  if (!sane_cost(ops, mem) || passphrase.size() > kMaxBytes) return std::nullopt;
  if (sodium_init() < 0) return std::nullopt;
  std::array<std::uint8_t, 32> k{};
  if (crypto_pwhash(k.data(), k.size(), passphrase.data(), passphrase.size(), salt.data(), ops,
                    static_cast<std::size_t>(mem), crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return std::nullopt;  // out of memory
  }
  std::array<std::uint8_t, crypto_sign_SEEDBYTES> seed{};
  crypto_generichash(seed.data(), seed.size(), reinterpret_cast<const std::uint8_t*>(kSeedLabel), sizeof kSeedLabel - 1,
                     k.data(), k.size());
  sodium_memzero(k.data(), k.size());
  signer s;
  crypto_sign_seed_keypair(s.pk.data(), s.sk.data(), seed.data());
  sodium_memzero(seed.data(), seed.size());
  return s;
}

std::optional<verifier> make(std::string_view passphrase, std::int64_t now_unix, std::uint64_t ops, std::uint64_t mem,
                             signer* out) {
  if (!acceptable(passphrase)) return std::nullopt;
  if (sodium_init() < 0) return std::nullopt;
  verifier v;
  randombytes_buf(v.salt.data(), v.salt.size());
  v.ops = ops;
  v.mem = mem;
  v.set = now_unix;
  auto s = derive(passphrase, v.salt, ops, mem);
  if (!s) return std::nullopt;
  v.pk = s->pk;
  if (out) *out = *s;
  return v;
}

namespace {

std::vector<std::uint8_t> signed_bytes(std::span<const std::uint8_t, kBindingBytes> binding, const key32& nonce,
                                       const key32& service, const key32& client) {
  // Sized once and filled in place (GCC 12's -Warray-bounds misreads a chain of inserts at -O3).
  constexpr std::size_t label = sizeof kSignLabel - 1;
  std::vector<std::uint8_t> m(label + kBindingBytes + 3 * 32);
  std::uint8_t* at = m.data();
  std::memcpy(at, kSignLabel, label);
  at += label;
  std::memcpy(at, binding.data(), kBindingBytes);
  at += kBindingBytes;
  for (const key32* k : {&nonce, &service, &client}) {
    std::memcpy(at, k->data(), 32);
    at += 32;
  }
  return m;
}

}  // namespace

signature sign(const signer& s, std::span<const std::uint8_t, kBindingBytes> binding, const key32& nonce,
               const key32& service, const key32& client) {
  const auto m = signed_bytes(binding, nonce, service, client);
  signature sig{};
  crypto_sign_detached(sig.data(), nullptr, m.data(), m.size(), s.sk.data());
  return sig;
}

bool check(const verifier& v, const signature& sig, std::span<const std::uint8_t, kBindingBytes> binding,
           const key32& nonce, const key32& service, const key32& client) noexcept {
  try {
    const auto m = signed_bytes(binding, nonce, service, client);
    return crypto_sign_verify_detached(sig.data(), m.data(), m.size(), v.pk.data()) == 0;
  } catch (...) {
    return false;
  }
}

stored load(secrets::store& store, verifier& out) {
  if (!store.exists(kPassphraseName)) return stored::none;
  const auto bytes = store.get(kPassphraseName);
  if (!bytes) return stored::unreadable;
  const auto v = parse_verifier(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!v) return stored::unreadable;
  out = *v;
  return stored::set;
}

bool save(secrets::store& store, const verifier& v) {
  const std::string text = verifier_json(v);
  return store.put(kPassphraseName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void counter::miss(std::int64_t now) noexcept {
  ++misses;
  if (misses <= kFreeMisses) return;
  const std::int64_t span = level >= 7 ? kMaxLockSeconds : std::min(kMaxLockSeconds, kFirstLockSeconds << level);
  until = now + span;
  if (level < 64) ++level;
}

std::string lockout_json(const lockout& l) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("claim");
  write_counter(w, l.claim);
  w.key("owner");
  write_counter(w, l.owner);
  w.end_object();
  return w.take();
}

std::optional<lockout> parse_lockout(std::string_view text) {
  if (text.size() > 4096) return std::nullopt;
  const auto doc = json::parse(text, 3);
  if (!doc || doc->integer("v") != 1) return std::nullopt;
  const auto claim = parse_counter(doc->find("claim"));
  const auto own = parse_counter(doc->find("owner"));
  if (!claim || !own) return std::nullopt;
  return lockout{*claim, *own};
}

lockout load_lockout(secrets::store& store, std::int64_t now) {
  if (!store.exists(kLockoutName)) return {};
  const auto bytes = store.get(kLockoutName);
  if (bytes) {
    if (auto l = parse_lockout(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()))) return *l;
  }
  lockout l;
  l.claim.misses = l.owner.misses = kFreeMisses;
  l.claim.until = l.owner.until = now + kFirstLockSeconds;
  (void)save_lockout(store, l);  // readable again, and over in kFirstLockSeconds
  return l;
}

bool save_lockout(secrets::store& store, const lockout& l) {
  const std::string text = lockout_json(l);
  return store.put(kLockoutName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool reset(secrets::store& store) {
  (void)store.erase(kPassphraseName);
  (void)store.erase(kLockoutName);
  return !store.exists(kPassphraseName) && !store.exists(kLockoutName);
}

}  // namespace mv::driftwire::serve::owner
