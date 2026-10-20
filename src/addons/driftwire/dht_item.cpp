// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/dht_item.h"

#include <sodium.h>

#include <chrono>
#include <cstring>

#include "addons/driftwire/sha1.h"
#include "addons/driftwire/wordlist.inc"

namespace mv::driftwire::dht {

namespace {

constexpr char kArgonContext[] = "MediaViewer Driftwire DHT code v2";
constexpr char kSaltContext[] = "MediaViewer Driftwire DHT salt v1";
constexpr std::uint8_t kMagic[4] = {'M', 'V', 'S', 'D'};
constexpr std::uint8_t kKind4 = 4, kKind6 = 6;
constexpr std::uint8_t kFlagHard = 1;

void put64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t get64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

int index_of(std::string_view w) noexcept {
  if (w.size() < 3) return -1;
  for (int i = 0; i < 2048; ++i) {
    const std::string_view full = words::kList[i];
    // A whole word, or its first four letters (BIP-39 makes those unique).
    if (full == w || (w.size() == 4 && full.substr(0, 4) == w)) return i;
  }
  return -1;
}

}  // namespace

keys::~keys() {
  sodium_memzero(secret_key.data(), secret_key.size());
  sodium_memzero(box.data(), box.size());
  sodium_memzero(salt_base.data(), salt_base.size());
  sodium_memzero(password.data(), password.size());
}

word_code random_code() {
  (void)sodium_init();
  word_code c;
  for (auto& w : c) w = static_cast<std::uint16_t>(randombytes_uniform(2048));
  return c;
}

std::string words_text(const word_code& c) {
  std::string out;
  for (std::size_t i = 0; i < c.size(); ++i) {
    if (i) out += ' ';
    out += words::kList[c[i] & 2047];
  }
  return out;
}

std::optional<word_code> parse_words(std::string_view text) {
  if (text.size() > 200) return std::nullopt;
  word_code c{};
  std::size_t n = 0;
  std::string w;
  auto flush = [&]() -> bool {
    if (w.empty()) return true;
    if (n >= kWords) return false;
    const int i = index_of(w);
    if (i < 0) return false;
    c[n++] = static_cast<std::uint16_t>(i);
    w.clear();
    return true;
  };
  for (const char ch : text) {
    if (ch == ' ' || ch == '-' || ch == '.' || ch == ',' || ch == '\t' || ch == '\n' || ch == '\r') {
      if (!flush()) return std::nullopt;
      continue;
    }
    if (ch >= 'A' && ch <= 'Z') {
      w.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else if (ch >= 'a' && ch <= 'z') {
      w.push_back(ch);
    } else {
      return std::nullopt;
    }
    if (w.size() > 8) return std::nullopt;
  }
  if (!flush() || n != kWords) return std::nullopt;
  return c;
}

std::int64_t code_hour(std::int64_t unix_ms) noexcept { return unix_ms / (60 * 60 * 1000); }

result<keys> derive(const word_code& c, bool fast, std::int64_t hour) {
  if (sodium_init() < 0) return err(status::internal);
  if (hour < 0) {
    hour = code_hour(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
  }
  // Argon2's salt: the context and the hour, hashed to its 16 bytes.
  std::uint8_t salt[crypto_pwhash_SALTBYTES];
  std::uint8_t h[8];
  for (int i = 0; i < 8; ++i) h[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(hour) >> (8 * i));
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, sizeof salt);
  crypto_generichash_update(&st, reinterpret_cast<const std::uint8_t*>(kArgonContext), sizeof kArgonContext - 1);
  crypto_generichash_update(&st, h, sizeof h);
  crypto_generichash_final(&st, salt, sizeof salt);
  // The code as text, lower case, single spaces: the same for any way it was typed.
  const std::string text = words_text(c);
  std::uint8_t out[32 * 4];
  const unsigned long long ops = fast ? crypto_pwhash_OPSLIMIT_MIN : crypto_pwhash_OPSLIMIT_MODERATE;
  const std::size_t mem = fast ? crypto_pwhash_MEMLIMIT_MIN : crypto_pwhash_MEMLIMIT_MODERATE;
  if (crypto_pwhash(out, sizeof out, text.data(), text.size(), salt, ops, mem,
                    crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return err(status::out_of_memory);  // libsodium's only failure here
  }
  keys k;
  crypto_sign_seed_keypair(k.public_key.data(), k.secret_key.data(), out);
  std::memcpy(k.box.data(), out + 32, 32);
  std::memcpy(k.salt_base.data(), out + 64, 32);
  std::memcpy(k.password.data(), out + 96, 32);
  sodium_memzero(out, sizeof out);
  return k;
}

std::string salt_of(const keys& k, role r, int i) {
  std::uint8_t in[2] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(i)};
  std::uint8_t out[16];
  crypto_generichash_state st;
  crypto_generichash_init(&st, k.salt_base.data(), k.salt_base.size(), sizeof out);
  crypto_generichash_update(&st, reinterpret_cast<const std::uint8_t*>(kSaltContext), sizeof kSaltContext - 1);
  crypto_generichash_update(&st, in, sizeof in);
  crypto_generichash_final(&st, out, sizeof out);
  return std::string(reinterpret_cast<const char*>(out), sizeof out);
}

std::array<std::uint8_t, 20> target_of(const keys& k, std::string_view salt) {
  std::vector<std::uint8_t> in(k.public_key.begin(), k.public_key.end());
  in.insert(in.end(), salt.begin(), salt.end());
  return sha1(in);
}

std::string seal(const keys& k, std::string_view salt, const contents& c) {
  std::uint8_t plain[kPlainBytes] = {};
  std::memcpy(plain, kMagic, 4);
  plain[4] = 1;  // version
  put64(plain + 5, static_cast<std::uint64_t>(c.written_ms));
  plain[13] = c.gone ? 1 : 0;
  std::size_t at = 15;
  std::uint8_t count = 0;
  for (const auto& e : c.candidates) {
    const auto ip = net::parse_address(e.address);
    if (!ip || e.port == 0 || count >= kMaxCandidates) continue;
    const std::size_t len = ip->v6 ? 16 : 4;
    if (at + 1 + len + 2 > kPlainBytes) break;
    plain[at++] = ip->v6 ? kKind6 : kKind4;
    std::memcpy(plain + at, ip->bytes.data(), len);
    at += len;
    plain[at++] = static_cast<std::uint8_t>(e.port >> 8);
    plain[at++] = static_cast<std::uint8_t>(e.port);
    ++count;
  }
  plain[14] = count;
  // After the candidates (#472): flags and the TCP port. Older readers stop
  // at the count and never look here.
  if (at + 3 <= kPlainBytes) {
    plain[at++] = c.hard ? kFlagHard : 0;
    plain[at++] = static_cast<std::uint8_t>(c.tcp_port >> 8);
    plain[at++] = static_cast<std::uint8_t>(c.tcp_port);
  }
  std::string v(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + kPlainBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES,
                '\0');
  auto* out = reinterpret_cast<std::uint8_t*>(v.data());
  randombytes_buf(out, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  unsigned long long len = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(out + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &len, plain,
                                             sizeof plain, reinterpret_cast<const std::uint8_t*>(salt.data()),
                                             salt.size(), nullptr, out, k.box.data());
  sodium_memzero(plain, sizeof plain);
  return v;
}

std::optional<contents> open(const keys& k, std::string_view salt, std::string_view v, std::int64_t now_ms) {
  constexpr std::size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  if (v.size() != kNonce + kPlainBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES) return std::nullopt;
  std::uint8_t plain[kPlainBytes];
  unsigned long long len = 0;
  const auto* in = reinterpret_cast<const std::uint8_t*>(v.data());
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, nullptr, in + kNonce, v.size() - kNonce,
                                                 reinterpret_cast<const std::uint8_t*>(salt.data()), salt.size(), in,
                                                 k.box.data()) != 0 ||
      len != kPlainBytes) {
    return std::nullopt;
  }
  auto c = parse_plain(plain, now_ms);
  sodium_memzero(plain, sizeof plain);
  return c;
}

std::optional<contents> parse_plain(std::span<const std::uint8_t> plain, std::int64_t now_ms) {
  if (plain.size() != kPlainBytes || std::memcmp(plain.data(), kMagic, 4) != 0 || plain[4] != 1) return std::nullopt;
  contents c;
  c.written_ms = static_cast<std::int64_t>(get64(plain.data() + 5));
  // A recording of an old item (or a clock far off) is not taken.
  if (c.written_ms < now_ms - kItemMaxAgeMs || c.written_ms > now_ms + kItemSkewMs) return std::nullopt;
  c.gone = plain[13] != 0;
  const std::uint8_t count = plain[14];
  if (count > kMaxCandidates) return std::nullopt;
  std::size_t at = 15;
  for (std::uint8_t i = 0; i < count; ++i) {
    if (at >= kPlainBytes) return std::nullopt;
    const std::uint8_t kind = plain[at++];
    const std::size_t alen = kind == kKind6 ? 16 : kind == kKind4 ? 4 : 0;
    if (alen == 0 || at + alen + 2 > kPlainBytes) return std::nullopt;
    net::address ip;
    ip.v6 = kind == kKind6;
    std::memcpy(ip.bytes.data(), plain.data() + at, alen);
    at += alen;
    const std::uint16_t port = static_cast<std::uint16_t>(plain[at] << 8 | plain[at + 1]);
    at += 2;
    if (port == 0) return std::nullopt;
    c.candidates.push_back({net::address_text(ip), port});
  }
  if (at + 3 <= kPlainBytes) {  // zeros from a writer before #472's trailer: not hard, no TCP
    c.hard = (plain[at] & kFlagHard) != 0;
    c.tcp_port = static_cast<std::uint16_t>(plain[at + 1] << 8 | plain[at + 2]);
  }
  return c;
}

std::string signed_bytes(std::string_view salt, std::int64_t seq, std::string_view bencoded_v) {
  std::string s;
  if (!salt.empty()) s += "4:salt" + std::to_string(salt.size()) + ":" + std::string(salt);
  s += "3:seqi" + std::to_string(seq) + "e1:v" + std::string(bencoded_v);
  return s;
}

std::array<std::uint8_t, 64> sign(const keys& k, std::string_view salt, std::int64_t seq, std::string_view bencoded_v) {
  const std::string m = signed_bytes(salt, seq, bencoded_v);
  std::array<std::uint8_t, 64> sig;
  crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const std::uint8_t*>(m.data()), m.size(),
                       k.secret_key.data());
  return sig;
}

bool verify(std::span<const std::uint8_t, 32> public_key, std::string_view salt, std::int64_t seq,
            std::string_view bencoded_v, std::span<const std::uint8_t> signature) {
  if (signature.size() != 64) return false;
  const std::string m = signed_bytes(salt, seq, bencoded_v);
  return crypto_sign_verify_detached(signature.data(), reinterpret_cast<const std::uint8_t*>(m.data()), m.size(),
                                     public_key.data()) == 0;
}

}  // namespace mv::driftwire::dht
