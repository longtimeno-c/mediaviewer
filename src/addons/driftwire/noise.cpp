// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/noise.h"

#include <sodium.h>

#include <cstring>

namespace mv::driftwire::noise {

static_assert(crypto_kem_mlkem768_PUBLICKEYBYTES == kKemPublicBytes);
static_assert(crypto_kem_mlkem768_CIPHERTEXTBYTES == kKemCiphertextBytes);
static_assert(crypto_kem_mlkem768_SHAREDSECRETBYTES == 32);
static_assert(crypto_scalarmult_BYTES == kDhBytes);

namespace {

constexpr std::size_t kBlockBytes = 128;  // BLAKE2b

// The patterns (Noise §7.5, §9 psk; the hfs modifier, revision 1).
constexpr token kXX1[] = {token::e};
constexpr token kXX2[] = {token::e, token::ee, token::s, token::es};
constexpr token kXX3[] = {token::s, token::se};
constexpr std::span<const token> kXX[] = {kXX1, kXX2, kXX3};
constexpr token kNNpsk01[] = {token::psk, token::e};
constexpr token kNNpsk02[] = {token::e, token::ee};
constexpr std::span<const token> kNNpsk0[] = {kNNpsk01, kNNpsk02};
constexpr token kXXhfs1[] = {token::e, token::e1};
constexpr token kXXhfs2[] = {token::e, token::ee, token::ekem1, token::s, token::es};
constexpr token kXXhfs3[] = {token::s, token::se};
constexpr std::span<const token> kXXhfs[] = {kXXhfs1, kXXhfs2, kXXhfs3};
constexpr token kXXpsk33[] = {token::s, token::se, token::psk};
constexpr std::span<const token> kXXpsk3[] = {kXX1, kXX2, kXXpsk33};
constexpr token kXXhfspsk01[] = {token::psk, token::e, token::e1};
constexpr std::span<const token> kXXhfspsk0[] = {kXXhfspsk01, kXXhfs2, kXXhfs3};
constexpr token kNNhfspsk01[] = {token::psk, token::e, token::e1};
constexpr token kNNhfspsk02[] = {token::e, token::ee, token::ekem1};
constexpr std::span<const token> kNNhfspsk0[] = {kNNhfspsk01, kNNhfspsk02};

const pattern kPatternXX{"Noise_XX_25519_ChaChaPoly_BLAKE2b", kXX, false};
const pattern kPatternNNpsk0{"Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b", kNNpsk0, true};
const pattern kPatternXXhfs{"Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b", kXXhfs, false};
const pattern kPatternXXpsk3{"Noise_XXpsk3_25519_ChaChaPoly_BLAKE2b", kXXpsk3, true};
const pattern kPatternXXhfspsk0{"Noise_XXhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b", kXXhfspsk0, true};
const pattern kPatternNNhfspsk0{"Noise_NNhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b", kNNhfspsk0, true};

void blake2b(std::span<const std::span<const std::uint8_t>> parts, std::span<std::uint8_t, kHashBytes> out) noexcept {
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, kHashBytes);
  for (const auto& p : parts) crypto_generichash_update(&st, p.data(), p.size());
  crypto_generichash_final(&st, out.data(), kHashBytes);
  sodium_memzero(&st, sizeof st);
}

}  // namespace

const pattern& xx() { return kPatternXX; }
const pattern& nn_psk0() { return kPatternNNpsk0; }
const pattern& xx_hfs() { return kPatternXXhfs; }
const pattern& nn_hfs_psk0() { return kPatternNNhfspsk0; }
const pattern& xx_psk3() { return kPatternXXpsk3; }
const pattern& xx_hfs_psk0() { return kPatternXXhfspsk0; }

// ---- HMAC and HKDF (Noise §4.3) ------------------------------------------------------

void hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data,
          std::span<std::uint8_t, kHashBytes> out) noexcept {
  std::uint8_t k[kBlockBytes]{};
  if (key.size() > kBlockBytes) {
    blake2b(std::array{key}, std::span<std::uint8_t, kHashBytes>(k, kHashBytes));
  } else if (!key.empty()) {
    std::memcpy(k, key.data(), key.size());
  }
  std::uint8_t pad[kBlockBytes];
  for (std::size_t i = 0; i < kBlockBytes; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
  std::uint8_t inner[kHashBytes];
  blake2b(std::array<std::span<const std::uint8_t>, 2>{std::span<const std::uint8_t>(pad), data}, inner);
  for (std::size_t i = 0; i < kBlockBytes; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
  blake2b(std::array<std::span<const std::uint8_t>, 2>{std::span<const std::uint8_t>(pad), std::span<const std::uint8_t>(inner)},
          out);
  sodium_memzero(k, sizeof k);
  sodium_memzero(pad, sizeof pad);
  sodium_memzero(inner, sizeof inner);
}

void hkdf(std::span<const std::uint8_t, kHashBytes> chaining_key, std::span<const std::uint8_t> ikm,
          std::span<std::uint8_t, kHashBytes> out1, std::span<std::uint8_t, kHashBytes> out2,
          std::span<std::uint8_t, kHashBytes> out3) noexcept {
  std::uint8_t temp[kHashBytes];
  hmac(chaining_key, ikm, temp);
  const std::uint8_t one = 1;
  hmac(temp, {&one, 1}, out1);
  std::uint8_t buf[kHashBytes + 1];
  std::memcpy(buf, out1.data(), kHashBytes);
  buf[kHashBytes] = 2;
  hmac(temp, buf, out2);
  std::memcpy(buf, out2.data(), kHashBytes);
  buf[kHashBytes] = 3;
  hmac(temp, buf, out3);
  sodium_memzero(temp, sizeof temp);
  sodium_memzero(buf, sizeof buf);
}

// ---- CipherState (Noise §5.1) ------------------------------------------------------------

void cipher_state::init(std::span<const std::uint8_t, kKeyBytes> k) noexcept {
  std::memcpy(k_.data(), k.data(), kKeyBytes);
  n_ = 0;
  has_key_ = true;
}

void cipher_state::clear() noexcept {
  sodium_memzero(k_.data(), k_.size());
  n_ = 0;
  has_key_ = false;
}

namespace {
void nonce_of(std::uint64_t n, std::uint8_t out[crypto_aead_chacha20poly1305_ietf_NPUBBYTES]) noexcept {
  std::memset(out, 0, crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
  for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<std::uint8_t>(n >> (8 * i));
}
}  // namespace

expected cipher_state::encrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> plain,
                               std::vector<std::uint8_t>& out) {
  if (!has_key_) {
    out.assign(plain.begin(), plain.end());
    return {};
  }
  if (n_ == UINT64_MAX) return err(status::invalid_arg);  // 2^64-1 is reserved (Noise §5.1)
  std::uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
  nonce_of(n_, nonce);
  out.resize(plain.size() + kTagBytes);
  unsigned long long len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(out.data(), &len, plain.data(), plain.size(), ad.data(), ad.size(),
                                            nullptr, nonce, k_.data());
  out.resize(static_cast<std::size_t>(len));
  ++n_;
  return {};
}

expected cipher_state::decrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> sealed,
                               std::vector<std::uint8_t>& out) {
  if (!has_key_) {
    out.assign(sealed.begin(), sealed.end());
    return {};
  }
  if (n_ == UINT64_MAX || sealed.size() < kTagBytes) return err(status::corrupt);
  std::uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
  nonce_of(n_, nonce);
  out.resize(sealed.size() - kTagBytes);
  unsigned long long len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(out.data(), &len, nullptr, sealed.data(), sealed.size(), ad.data(),
                                                ad.size(), nonce, k_.data()) != 0) {
    out.clear();
    return err(status::corrupt);
  }
  out.resize(static_cast<std::size_t>(len));
  ++n_;
  return {};
}

split_keys::~split_keys() {
  sodium_memzero(initiator_to_responder.data(), kKeyBytes);
  sodium_memzero(responder_to_initiator.data(), kKeyBytes);
  sodium_memzero(extra.data(), kKeyBytes);
}

// ---- SymmetricState and HandshakeState (Noise §5.2, §5.3) ------------------------------------

struct handshake::state {
  const pattern* p = nullptr;
  bool initiator = false;
  std::size_t next = 0;  // the next message's index
  bool split_done = false;
  // SymmetricState.
  std::array<std::uint8_t, kHashBytes> ck{}, h{};
  cipher_state cs;
  // Keys.
  std::array<std::uint8_t, 32> s{}, s_pub{}, e{}, e_pub{}, re{}, rs{}, psk{};
  bool has_s = false, has_e = false, has_re = false, has_rs = false, has_psk = false;
  std::array<std::uint8_t, crypto_kem_mlkem768_SECRETKEYBYTES> e1{};
  std::array<std::uint8_t, kKemPublicBytes> e1_pub{}, re1{};
  bool has_e1 = false, has_re1 = false;
  test_keys fixed;
  std::vector<std::uint8_t> tmp;

  ~state() {
    sodium_memzero(ck.data(), ck.size());
    sodium_memzero(s.data(), s.size());
    sodium_memzero(e.data(), e.size());
    sodium_memzero(psk.data(), psk.size());
    sodium_memzero(e1.data(), e1.size());
    if (fixed.e) sodium_memzero(fixed.e->data(), 32);
    if (fixed.e1_seed) sodium_memzero(fixed.e1_seed->data(), 64);
    if (fixed.kem_seed) sodium_memzero(fixed.kem_seed->data(), 32);
  }

  void mix_hash(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint8_t, kHashBytes> out;
    blake2b(std::array<std::span<const std::uint8_t>, 2>{std::span<const std::uint8_t>(h), data}, out);
    h = out;
  }
  void mix_key(std::span<const std::uint8_t> ikm) noexcept {
    std::array<std::uint8_t, kHashBytes> o1, o2, o3;
    hkdf(ck, ikm, o1, o2, o3);
    ck = o1;
    cs.init(std::span<const std::uint8_t, kKeyBytes>(o2.data(), kKeyBytes));
    sodium_memzero(o2.data(), o2.size());
    sodium_memzero(o3.data(), o3.size());
  }
  void mix_key_and_hash(std::span<const std::uint8_t> ikm) noexcept {
    std::array<std::uint8_t, kHashBytes> o1, o2, o3;
    hkdf(ck, ikm, o1, o2, o3);
    ck = o1;
    mix_hash(o2);
    cs.init(std::span<const std::uint8_t, kKeyBytes>(o3.data(), kKeyBytes));
    sodium_memzero(o3.data(), o3.size());
  }
  expected encrypt_and_hash(std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& out) {
    if (auto r = cs.encrypt(h, plain, tmp); !r) return r;
    mix_hash(tmp);
    out.insert(out.end(), tmp.begin(), tmp.end());
    return {};
  }
  // Takes `len` plaintext bytes' worth from `in` (plus the tag once there is a key).
  expected decrypt_and_hash(std::span<const std::uint8_t>& in, std::size_t len, std::span<std::uint8_t> plain) {
    const std::size_t sealed = len + (cs.has_key() ? kTagBytes : 0);
    if (in.size() < sealed) return err(status::corrupt);
    const auto c = in.first(sealed);
    if (auto r = cs.decrypt(h, c, tmp); !r) return r;
    if (tmp.size() != len) return err(status::corrupt);
    // An empty payload has no buffer on either side, and memcpy's pointers
    // must not be null even for no bytes (UBSan).
    if (len != 0) std::memcpy(plain.data(), tmp.data(), len);
    sodium_memzero(tmp.data(), tmp.size());
    mix_hash(c);
    in = in.subspan(sealed);
    return {};
  }
  expected dh(const std::array<std::uint8_t, 32>& priv, const std::array<std::uint8_t, 32>& pub) {
    std::uint8_t out[kDhBytes];
    // libsodium refuses an all-zero result (a low-order point).
    if (crypto_scalarmult(out, priv.data(), pub.data()) != 0) return err(status::corrupt);
    mix_key(out);
    sodium_memzero(out, sizeof out);
    return {};
  }
};

handshake::handshake(const pattern& p, bool initiator, std::span<const std::uint8_t> prologue,
                     std::optional<std::array<std::uint8_t, 32>> s, std::optional<std::array<std::uint8_t, 32>> psk,
                     test_keys fixed)
    : st_(std::make_unique<state>()) {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  st_->p = &p;
  st_->initiator = initiator;
  st_->fixed = std::move(fixed);
  const std::size_t name_len = std::strlen(p.name);
  if (name_len <= kHashBytes) {
    std::memcpy(st_->h.data(), p.name, name_len);
  } else {
    blake2b(std::array{std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(p.name), name_len)}, st_->h);
  }
  st_->ck = st_->h;
  st_->mix_hash(prologue);
  if (s) {
    st_->s = *s;
    crypto_scalarmult_base(st_->s_pub.data(), st_->s.data());
    st_->has_s = true;
    sodium_memzero(s->data(), s->size());
  }
  if (psk) {
    st_->psk = *psk;
    st_->has_psk = true;
    sodium_memzero(psk->data(), psk->size());
  }
}

handshake::~handshake() = default;

bool handshake::done() const noexcept { return st_ && st_->next >= st_->p->messages.size(); }

std::array<std::uint8_t, kHashBytes> handshake::hash() const noexcept { return st_->h; }

std::optional<std::array<std::uint8_t, 32>> handshake::remote_static() const {
  if (!st_->has_rs) return std::nullopt;
  return st_->rs;
}

expected handshake::write_message(std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out) {
  state& st = *st_;
  if (st.split_done || done() || (st.next % 2 == 0) != st.initiator) return err(status::invalid_arg);
  out.clear();
  for (const token t : st.p->messages[st.next]) {
    switch (t) {
      case token::e: {
        if (st.fixed.e) {
          st.e = *st.fixed.e;
        } else {
          randombytes_buf(st.e.data(), st.e.size());
        }
        crypto_scalarmult_base(st.e_pub.data(), st.e.data());
        st.has_e = true;
        out.insert(out.end(), st.e_pub.begin(), st.e_pub.end());
        st.mix_hash(st.e_pub);
        if (st.p->psk) st.mix_key(st.e_pub);
        break;
      }
      case token::s:
        if (!st.has_s) return err(status::invalid_arg);
        if (auto r = st.encrypt_and_hash(st.s_pub, out); !r) return r;
        break;
      case token::ee:
        if (auto r = st.dh(st.e, st.re); !r) return r;
        break;
      case token::es:
        if (auto r = st.initiator ? st.dh(st.e, st.rs) : st.dh(st.s, st.re); !r) return r;
        break;
      case token::se:
        if (auto r = st.initiator ? st.dh(st.s, st.re) : st.dh(st.e, st.rs); !r) return r;
        break;
      case token::ss:
        if (auto r = st.dh(st.s, st.rs); !r) return r;
        break;
      case token::psk:
        if (!st.has_psk) return err(status::invalid_arg);
        st.mix_key_and_hash(st.psk);
        break;
      case token::e1: {
        const int made = st.fixed.e1_seed
                             ? crypto_kem_mlkem768_seed_keypair(st.e1_pub.data(), st.e1.data(), st.fixed.e1_seed->data())
                             : crypto_kem_mlkem768_keypair(st.e1_pub.data(), st.e1.data());
        if (made != 0) return err(status::internal);
        st.has_e1 = true;
        if (auto r = st.encrypt_and_hash(st.e1_pub, out); !r) return r;
        break;
      }
      case token::ekem1: {
        if (!st.has_re1) return err(status::invalid_arg);
        std::uint8_t ct[kKemCiphertextBytes], ss_kem[32];
        const int enc = st.fixed.kem_seed
                            ? crypto_kem_mlkem768_enc_deterministic(ct, ss_kem, st.re1.data(), st.fixed.kem_seed->data())
                            : crypto_kem_mlkem768_enc(ct, ss_kem, st.re1.data());
        if (enc != 0) return err(status::corrupt);  // a public key that is not well formed
        auto r = st.encrypt_and_hash(ct, out);
        if (r) st.mix_key(ss_kem);
        sodium_memzero(ss_kem, sizeof ss_kem);
        if (!r) return r;
        break;
      }
    }
  }
  if (auto r = st.encrypt_and_hash(payload, out); !r) return r;
  if (out.size() > kMaxMessageBytes) return err(status::invalid_arg);
  ++st.next;
  return {};
}

expected handshake::read_message(std::span<const std::uint8_t> message, std::vector<std::uint8_t>& payload) {
  state& st = *st_;
  if (st.split_done || done() || (st.next % 2 == 0) == st.initiator) return err(status::invalid_arg);
  if (message.size() > kMaxMessageBytes) return err(status::corrupt);
  std::span<const std::uint8_t> in = message;
  for (const token t : st.p->messages[st.next]) {
    switch (t) {
      case token::e: {
        if (in.size() < kDhBytes) return err(status::corrupt);
        std::memcpy(st.re.data(), in.data(), kDhBytes);
        in = in.subspan(kDhBytes);
        st.has_re = true;
        st.mix_hash(st.re);
        if (st.p->psk) st.mix_key(st.re);
        break;
      }
      case token::s:
        if (auto r = st.decrypt_and_hash(in, kDhBytes, st.rs); !r) return r;
        st.has_rs = true;
        break;
      case token::ee:
        if (auto r = st.dh(st.e, st.re); !r) return r;
        break;
      case token::es:
        if (auto r = st.initiator ? st.dh(st.e, st.rs) : st.dh(st.s, st.re); !r) return r;
        break;
      case token::se:
        if (auto r = st.initiator ? st.dh(st.s, st.re) : st.dh(st.e, st.rs); !r) return r;
        break;
      case token::ss:
        if (auto r = st.dh(st.s, st.rs); !r) return r;
        break;
      case token::psk:
        if (!st.has_psk) return err(status::invalid_arg);
        st.mix_key_and_hash(st.psk);
        break;
      case token::e1:
        if (auto r = st.decrypt_and_hash(in, kKemPublicBytes, st.re1); !r) return r;
        st.has_re1 = true;
        break;
      case token::ekem1: {
        if (!st.has_e1) return err(status::invalid_arg);
        std::uint8_t ct[kKemCiphertextBytes], ss_kem[32];
        if (auto r = st.decrypt_and_hash(in, kKemCiphertextBytes, ct); !r) return r;
        // ML-KEM's implicit rejection: a bad ciphertext gives a key the peer
        // does not have, and the payload below does not open.
        if (crypto_kem_mlkem768_dec(ss_kem, ct, st.e1.data()) != 0) return err(status::corrupt);
        st.mix_key(ss_kem);
        sodium_memzero(ss_kem, sizeof ss_kem);
        break;
      }
    }
  }
  const std::size_t plain = st.cs.has_key() ? (in.size() < kTagBytes ? SIZE_MAX : in.size() - kTagBytes) : in.size();
  if (plain == SIZE_MAX) return err(status::corrupt);
  payload.resize(plain);
  if (auto r = st.decrypt_and_hash(in, plain, payload); !r) return r;
  ++st.next;
  return {};
}

result<split_keys> handshake::split() {
  state& st = *st_;
  if (!done() || st.split_done) return err(status::invalid_arg);
  std::array<std::uint8_t, kHashBytes> o1, o2, o3;
  hkdf(st.ck, {}, o1, o2, o3);
  split_keys k;
  std::memcpy(k.initiator_to_responder.data(), o1.data(), kKeyBytes);
  std::memcpy(k.responder_to_initiator.data(), o2.data(), kKeyBytes);
  std::memcpy(k.extra.data(), o3.data(), kKeyBytes);
  k.h = st.h;
  sodium_memzero(o1.data(), o1.size());
  sodium_memzero(o2.data(), o2.size());
  sodium_memzero(o3.data(), o3.size());
  st.split_done = true;
  // The handshake's secrets go now; only h stays readable.
  sodium_memzero(st.ck.data(), st.ck.size());
  sodium_memzero(st.e.data(), st.e.size());
  sodium_memzero(st.s.data(), st.s.size());
  sodium_memzero(st.e1.data(), st.e1.size());
  sodium_memzero(st.psk.data(), st.psk.size());
  st.cs.clear();
  return k;
}

}  // namespace mv::driftwire::noise
