// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// TLS over a link with BoringSSL (tls.h; docs/plans/driftwire-tls-spike.md).
//
// The link stays the transport: TLS reads and writes memory BIOs, and this
// file moves their bytes over the link, so a splice's two threads (one
// reader, one writer) can use a TLS link as any other. The SSL object is
// under one lock, never held across the link's I/O; what TLS has to send
// (records the writer made, and the rare ones a read makes, such as a key
// update's answer) goes through one queue that whoever holds the write lock
// sends, in order.
//
// The client's ClientHello is Chrome's (BoringSSL is Chrome's TLS): GREASE,
// permuted extensions, ECH GREASE, ALPS on the new codepoint, brotli
// certificate compression offered, X25519MLKEM768 first, Chrome's ciphers
// and signature algorithms, OCSP and SCTs asked for, ALPN h2 and http/1.1.
#include <openssl/bytestring.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sodium.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "addons/driftwire/tls.h"

namespace mv::driftwire::tls {

namespace {

using clock = std::chrono::steady_clock;

struct ctx_free {
  void operator()(SSL_CTX* c) const { SSL_CTX_free(c); }
};
struct ssl_free {
  void operator()(SSL* s) const { SSL_free(s); }
};
using ctx_ptr = std::unique_ptr<SSL_CTX, ctx_free>;
using ssl_ptr = std::unique_ptr<SSL, ssl_free>;

// The bytes TLS wrote, taken out of its memory BIO.
void drain(BIO* wbio, std::vector<std::uint8_t>& out) {
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  if (BIO_mem_contents(wbio, &data, &len) && len > 0) {
    out.insert(out.end(), data, data + len);
    (void)BIO_reset(wbio);
  }
}

int left_ms(clock::time_point deadline) {
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
  return ms <= 0 ? 0 : static_cast<int>(std::min<long long>(ms, 1 << 30));
}

// A brotli certificate the client offers to take (as Chrome does); this
// build's own server never compresses, so a compressed one is refused.
int no_decompress(SSL*, CRYPTO_BUFFER**, std::size_t, const std::uint8_t*, std::size_t) { return 0; }

class tls_link final : public link {
 public:
  tls_link(std::unique_ptr<link> inner, ssl_ptr ssl, BIO* rbio, BIO* wbio)
      : inner_(std::move(inner)), ssl_(std::move(ssl)), rbio_(rbio), wbio_(wbio) {}
  ~tls_link() override { inner_->shutdown(); }

  // The handshake, on one thread, before the link is shared.
  expected handshake(clock::time_point deadline) {
    for (;;) {
      int r = 0, e = 0;
      std::vector<std::uint8_t> out;
      {
        std::lock_guard lock(m_);
        r = SSL_do_handshake(ssl_.get());
        e = SSL_get_error(ssl_.get(), r);
        drain(wbio_, out);
      }
      if (!out.empty()) {
        if (auto w = inner_->write_all(out); !w) return w;
      }
      if (r == 1) return {};
      if (e != SSL_ERROR_WANT_READ) return err(status::corrupt);
      if (auto f = feed(left_ms(deadline)); !f) return f;
    }
  }

  // This session's exporter, for the knock.
  std::array<std::uint8_t, 32> exporter() {
    std::array<std::uint8_t, 32> out{};
    std::lock_guard lock(m_);
    (void)SSL_export_keying_material(ssl_.get(), out.data(), out.size(), kExporterLabel.data(),
                                     kExporterLabel.size(), nullptr, 0, 0);
    return out;
  }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::lock_guard w(w_);
    std::vector<std::uint8_t> out;
    {
      std::lock_guard lock(m_);
      while (!bytes.empty()) {
        const int n = static_cast<int>(std::min<std::size_t>(bytes.size(), 16 * 1024));
        const int r = SSL_write(ssl_.get(), bytes.data(), n);
        if (r <= 0) return err(status::io);
        bytes = bytes.subspan(static_cast<std::size_t>(r));
      }
      out = std::move(pending_);
      pending_.clear();
      drain(wbio_, out);
    }
    return inner_->write_all(out);
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    const auto deadline =
        timeout_ms < 0 ? clock::time_point::max() : clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      int r = 0, e = 0;
      {
        std::lock_guard lock(m_);
        r = SSL_read(ssl_.get(), into.data(), static_cast<int>(std::min<std::size_t>(into.size(), 1 << 30)));
        e = SSL_get_error(ssl_.get(), r);
        drain(wbio_, pending_);
      }
      flush_if_free();
      if (r > 0) return static_cast<std::size_t>(r);
      if (e == SSL_ERROR_ZERO_RETURN) return std::size_t{0};
      if (e != SSL_ERROR_WANT_READ) return err(status::io);
      if (auto f = feed(timeout_ms < 0 ? -1 : left_ms(deadline)); !f) {
        if (f.error() == status::io && closed_) return std::size_t{0};  // the peer closed
        return err(f.error());
      }
    }
  }

  void shutdown() noexcept override { inner_->shutdown(); }
  [[nodiscard]] transport via() const noexcept override { return inner_->via(); }
  [[nodiscard]] const char* through() const noexcept override { return inner_->through(); }

 private:
  // More ciphertext from the link into TLS (the reader's, or the handshake's).
  expected feed(int timeout_ms) {
    if (timeout_ms == 0) return err(status::timeout);
    std::array<std::uint8_t, 16 * 1024> buf{};
    auto n = inner_->read_some(buf, timeout_ms);
    if (!n) return err(n.error());
    if (*n == 0) {
      closed_ = true;
      return err(status::io);
    }
    std::lock_guard lock(m_);
    if (BIO_write(rbio_, buf.data(), static_cast<int>(*n)) != static_cast<int>(*n)) return err(status::internal);
    return {};
  }

  // What a read made TLS say, sent now unless a writer is sending (it sends
  // the queue first next time).
  void flush_if_free() {
    std::unique_lock w(w_, std::try_to_lock);
    if (!w.owns_lock()) return;
    std::vector<std::uint8_t> out;
    {
      std::lock_guard lock(m_);
      out = std::move(pending_);
      pending_.clear();
    }
    if (!out.empty()) (void)inner_->write_all(out);
  }

  std::unique_ptr<link> inner_;
  std::mutex w_;  // one sender on the link at a time, in order
  std::mutex m_;  // the SSL object, its BIOs and pending_
  ssl_ptr ssl_;
  BIO* rbio_;  // owned by ssl_
  BIO* wbio_;  // owned by ssl_
  std::vector<std::uint8_t> pending_;  // under m_
  bool closed_ = false;                // the reader's own
};

template <class Configure>
std::unique_ptr<tls_link> wrap(std::unique_ptr<link> l, SSL_CTX* ctx, Configure configure) {
  ssl_ptr ssl(SSL_new(ctx));
  if (!ssl || !configure(ssl.get())) {
    l->shutdown();
    return nullptr;
  }
  BIO* rbio = BIO_new(BIO_s_mem());
  BIO* wbio = BIO_new(BIO_s_mem());
  if (!rbio || !wbio) {
    BIO_free(rbio);
    BIO_free(wbio);
    l->shutdown();
    return nullptr;
  }
  BIO_set_mem_eof_return(rbio, -1);  // empty: "want more", not the end
  BIO_set_mem_eof_return(wbio, -1);
  SSL_set_bio(ssl.get(), rbio, wbio);
  return std::make_unique<tls_link>(std::move(l), std::move(ssl), rbio, wbio);
}

// Chrome's profile, made once.
SSL_CTX* client_ctx() {
  static SSL_CTX* made = [] {
    SSL_CTX* c = SSL_CTX_new(TLS_method());
    if (!c) return c;
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);
    SSL_CTX_set_grease_enabled(c, 1);
    SSL_CTX_set_permute_extensions(c, 1);
    (void)SSL_CTX_set_strict_cipher_list(
        c,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
    (void)SSL_CTX_set1_groups_list(c, "X25519MLKEM768:X25519:P-256:P-384");
    static const std::uint16_t kSigalgs[] = {
        SSL_SIGN_ECDSA_SECP256R1_SHA256, SSL_SIGN_RSA_PSS_RSAE_SHA256, SSL_SIGN_RSA_PKCS1_SHA256,
        SSL_SIGN_ECDSA_SECP384R1_SHA384, SSL_SIGN_RSA_PSS_RSAE_SHA384, SSL_SIGN_RSA_PKCS1_SHA384,
        SSL_SIGN_RSA_PSS_RSAE_SHA512,    SSL_SIGN_RSA_PKCS1_SHA512};
    (void)SSL_CTX_set_verify_algorithm_prefs(c, kSigalgs, sizeof kSigalgs / sizeof kSigalgs[0]);
    static const std::uint8_t kAlpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    (void)SSL_CTX_set_alpn_protos(c, kAlpn, sizeof kAlpn);
    SSL_CTX_enable_ocsp_stapling(c);
    SSL_CTX_enable_signed_cert_timestamps(c);
    (void)SSL_CTX_add_cert_compression_alg(c, TLSEXT_cert_compression_brotli, nullptr, no_decompress);
    // Camouflage only: the hop inside proves the server key.
    SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
    return c;
  }();
  return made;
}

// The server's: http/1.1 when offered (what its web site speaks).
int select_alpn(SSL*, const std::uint8_t** out, std::uint8_t* out_len, const std::uint8_t* in, unsigned in_len,
                void*) {
  for (unsigned i = 0; i < in_len;) {
    const unsigned n = in[i];
    if (i + 1 + n > in_len) break;
    if (n == 8 && std::memcmp(in + i + 1, "http/1.1", 8) == 0) {
      *out = in + i + 1;
      *out_len = 8;
      return SSL_TLSEXT_ERR_OK;
    }
    i += 1 + n;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

class boring_server_context final : public server_context {
 public:
  ctx_ptr ctx;
};

// A self-signed P-256 certificate for `name`, a year from yesterday.
bool self_signed(SSL_CTX* ctx, const std::string& name) {
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  if (!ec || !key || !EC_KEY_generate_key(ec.get()) || !EVP_PKEY_assign_EC_KEY(key.get(), ec.release())) return false;
  bssl::UniquePtr<X509> x(X509_new());
  if (!x || !X509_set_version(x.get(), X509_VERSION_3)) return false;
  std::uint8_t serial[16];
  RAND_bytes(serial, sizeof serial);
  serial[0] &= 0x7f;
  bssl::UniquePtr<BIGNUM> sn(BN_bin2bn(serial, sizeof serial, nullptr));
  if (!sn || !BN_to_ASN1_INTEGER(sn.get(), X509_get_serialNumber(x.get()))) return false;
  const std::time_t now = std::time(nullptr);
  if (!X509_time_adj_ex(X509_getm_notBefore(x.get()), -1, 0, &now) ||
      !X509_time_adj_ex(X509_getm_notAfter(x.get()), 365, 0, &now)) {
    return false;
  }
  X509_NAME* subject = X509_get_subject_name(x.get());
  if (!X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const std::uint8_t*>(name.data()),
                                  static_cast<ossl_ssize_t>(name.size()), -1, 0) ||
      !X509_set_issuer_name(x.get(), subject) || !X509_set_pubkey(x.get(), key.get())) {
    return false;
  }
  const std::string san = "DNS:" + name;
  bssl::UniquePtr<X509_EXTENSION> ext(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str()));
  if (!ext || !X509_add_ext(x.get(), ext.get(), -1)) return false;
  if (!X509_sign(x.get(), key.get(), EVP_sha256())) return false;
  return SSL_CTX_use_certificate(ctx, x.get()) && SSL_CTX_use_PrivateKey(ctx, key.get());
}

}  // namespace

bool available() noexcept { return true; }

result<std::unique_ptr<link>> client(std::unique_ptr<link> l, const std::string& sni, const knock_key* k,
                                     int timeout_ms) {
  if (!l) return err(status::invalid_arg);
  SSL_CTX* ctx = client_ctx();
  if (!ctx) {
    l->shutdown();
    return err(status::internal);
  }
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  auto t = wrap(std::move(l), ctx, [&](SSL* s) {
    // What Chrome sets per connection.
    SSL_set_connect_state(s);
    SSL_set_enable_ech_grease(s, 1);
    SSL_set_alps_use_new_codepoint(s, 1);
    if (!SSL_add_application_settings(s, reinterpret_cast<const std::uint8_t*>("h2"), 2, nullptr, 0)) return false;
    return sni.empty() || SSL_set_tlsext_host_name(s, sni.c_str()) == 1;
  });
  if (!t) return err(status::internal);
  if (auto h = t->handshake(deadline); !h) {
    t->shutdown();
    return err(h.error());
  }
  if (k) {
    const auto knock = knock_of(*k, t->exporter());
    if (auto w = t->write_all(knock); !w) {
      t->shutdown();
      return err(w.error());
    }
  }
  return std::unique_ptr<link>(std::move(t));
}

result<std::unique_ptr<server_context>> make_server_context(const std::string& cert_pem_path,
                                                            const std::string& key_pem_path,
                                                            const std::string& name) {
  auto out = std::make_unique<boring_server_context>();
  out->ctx.reset(SSL_CTX_new(TLS_method()));
  SSL_CTX* c = out->ctx.get();
  if (!c) return err(status::internal);
  SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);
  SSL_CTX_set_alpn_select_cb(c, select_alpn, nullptr);
  if (!cert_pem_path.empty() || !key_pem_path.empty()) {
    if (SSL_CTX_use_certificate_chain_file(c, cert_pem_path.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(c, key_pem_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(c) != 1) {
      return err(status::invalid_arg);
    }
  } else if (!self_signed(c, name.empty() ? std::string("localhost") : name)) {
    return err(status::internal);
  }
  return std::unique_ptr<server_context>(std::move(out));
}

std::unique_ptr<link> accept(server_context& sc, std::unique_ptr<link> l, const knock_key& k, const decoy& d,
                             accepted_as& as, int timeout_ms) {
  as = accepted_as::failed;
  if (!l) return nullptr;
  auto& ctx = static_cast<boring_server_context&>(sc);
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  auto t = wrap(std::move(l), ctx.ctx.get(), [](SSL* s) {
    SSL_set_accept_state(s);
    return true;
  });
  if (!t) return nullptr;
  if (!t->handshake(deadline)) {
    t->shutdown();
    return nullptr;
  }
  const auto want = knock_of(k, t->exporter());
  // The knock, or the start of an HTTP request: what came first decides.
  std::vector<std::uint8_t> got;
  const auto idle_until = clock::now() + std::chrono::milliseconds(kIdleMs);
  std::array<std::uint8_t, 4096> buf{};
  bool closed = false;
  while (got.size() < kKnockBytes) {
    const int ms = left_ms(idle_until);
    if (ms == 0) break;
    auto n = t->read_some(std::span(buf).first(kKnockBytes - got.size()), ms);
    if (!n || *n == 0) {
      closed = !n ? n.error() != status::timeout : true;
      break;
    }
    got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
  }
  if (got.size() == kKnockBytes && sodium_memcmp(got.data(), want.data(), kKnockBytes) == 0) {
    as = accepted_as::relay;
    return std::unique_ptr<link>(std::move(t));
  }
  // A web site: the request's head (through its blank line, 8 KiB at most),
  // then the page, and the end.
  as = accepted_as::website;
  std::string head(got.begin(), got.end());
  while (!closed && head.find("\r\n\r\n") == std::string::npos && head.size() < 8192) {
    const int ms = left_ms(idle_until);
    if (ms == 0) break;
    auto n = t->read_some(buf, ms);
    if (!n || *n == 0) break;
    head.append(reinterpret_cast<const char*>(buf.data()), *n);
  }
  if (head.find("\r\n\r\n") != std::string::npos || head.size() >= 8192) {
    const auto page = decoy_response(head, d);
    (void)t->write_all({reinterpret_cast<const std::uint8_t*>(page.data()), page.size()});
  }
  t->shutdown();
  return nullptr;
}

}  // namespace mv::driftwire::tls
