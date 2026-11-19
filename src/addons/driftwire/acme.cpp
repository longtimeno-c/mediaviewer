// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The ACME client (acme.h) with BoringSSL: ES256 account and certificate
// keys, JWS, the CSR, and reading a certificate's end and names.
#include "addons/driftwire/acme.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sodium.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "core/json.h"

namespace mv::driftwire::acme {

namespace {

using pkey_ptr = bssl::UniquePtr<EVP_PKEY>;

std::string sha256(std::string_view s) {
  std::string out(crypto_hash_sha256_BYTES, '\0');
  crypto_hash_sha256(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(s.data()),
                     s.size());
  return out;
}

pkey_ptr new_p256() {
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
  pkey_ptr key(EVP_PKEY_new());
  if (!ec || !key || !EC_KEY_generate_key(ec.get()) || !EVP_PKEY_assign_EC_KEY(key.get(), ec.release())) return nullptr;
  return key;
}

std::string pem_of(EVP_PKEY* key) {
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  if (!bio || !PEM_write_bio_PKCS8PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr)) return {};
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  if (!BIO_mem_contents(bio.get(), &data, &len)) return {};
  return std::string(reinterpret_cast<const char*>(data), len);
}

pkey_ptr key_of(std::string_view pem) {
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<ossl_ssize_t>(pem.size())));
  if (!bio) return nullptr;
  pkey_ptr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key || EVP_PKEY_id(key.get()) != EVP_PKEY_EC) return nullptr;
  const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(key.get());
  if (!ec || EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) != NID_X9_62_prime256v1) return nullptr;
  return key;
}

// The account key's JWK, in the order RFC 7638's thumbprint wants (crv, kty, x, y).
std::string jwk_of(EVP_PKEY* key) {
  const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(key);
  bssl::UniquePtr<BIGNUM> x(BN_new()), y(BN_new());
  if (!ec || !x || !y ||
      !EC_POINT_get_affine_coordinates_GFp(EC_KEY_get0_group(ec), EC_KEY_get0_public_key(ec), x.get(), y.get(),
                                           nullptr)) {
    return {};
  }
  std::string xb(32, '\0'), yb(32, '\0');
  if (!BN_bn2bin_padded(reinterpret_cast<std::uint8_t*>(xb.data()), 32, x.get()) ||
      !BN_bn2bin_padded(reinterpret_cast<std::uint8_t*>(yb.data()), 32, y.get())) {
    return {};
  }
  return R"({"crv":"P-256","kty":"EC","x":")" + b64url(xb) + R"(","y":")" + b64url(yb) + R"("})";
}

// ES256: the raw r || s of ECDSA over SHA-256.
std::string es256(EVP_PKEY* key, std::string_view signing_input) {
  const std::string digest = sha256(signing_input);
  bssl::UniquePtr<ECDSA_SIG> sig(ECDSA_do_sign(reinterpret_cast<const std::uint8_t*>(digest.data()), digest.size(),
                                              EVP_PKEY_get0_EC_KEY(key)));
  if (!sig) return {};
  const BIGNUM *r = nullptr, *s = nullptr;
  ECDSA_SIG_get0(sig.get(), &r, &s);
  std::string out(64, '\0');
  if (!BN_bn2bin_padded(reinterpret_cast<std::uint8_t*>(out.data()), 32, r) ||
      !BN_bn2bin_padded(reinterpret_cast<std::uint8_t*>(out.data()) + 32, 32, s)) {
    return {};
  }
  return out;
}

// A CSR (DER) for `name` with `key`.
std::string csr_of(EVP_PKEY* key, const std::string& name) {
  bssl::UniquePtr<X509_REQ> req(X509_REQ_new());
  if (!req || !X509_REQ_set_version(req.get(), X509_REQ_VERSION_1) || !X509_REQ_set_pubkey(req.get(), key)) return {};
  X509_NAME* subject = X509_REQ_get_subject_name(req.get());
  if (!X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const std::uint8_t*>(name.data()),
                                  static_cast<ossl_ssize_t>(name.size()), -1, 0)) {
    return {};
  }
  bssl::UniquePtr<STACK_OF(X509_EXTENSION)> exts(sk_X509_EXTENSION_new_null());
  const std::string san = "DNS:" + name;
  X509_EXTENSION* ext = X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
  if (!exts || !ext || !sk_X509_EXTENSION_push(exts.get(), ext)) {
    X509_EXTENSION_free(ext);
    return {};
  }
  if (!X509_REQ_add_extensions(req.get(), exts.get()) || !X509_REQ_sign(req.get(), key, EVP_sha256())) return {};
  std::uint8_t* der = nullptr;
  const int len = i2d_X509_REQ(req.get(), &der);
  if (len <= 0) return {};
  std::string out(reinterpret_cast<const char*>(der), static_cast<std::size_t>(len));
  OPENSSL_free(der);
  return out;
}

bool has(const std::string& body, std::string_view error_type) {
  return body.find(std::string("urn:ietf:params:acme:error:") + std::string(error_type)) != std::string::npos;
}

failure classify(const http_response& r) {
  if (r.status == 0) return failure::network;
  if (r.status == 429 || has(r.body, "rateLimited")) return failure::rate_limited;
  if (has(r.body, "dns") || has(r.body, "unauthorized") || has(r.body, "incorrectResponse") ||
      has(r.body, "connection")) {
    return failure::dns;
  }
  if (r.status >= 400 && r.status < 500) return failure::refused;
  return failure::bad_answer;
}

// One ACME conversation: the directory, the nonce, the account's signature.
class session {
 public:
  session(const order_options& o, EVP_PKEY* account) : o_(o), account_(account), jwk_(jwk_of(account)) {}

  failure why = failure::none;
  std::int64_t retry_after = 0;

  bool wait(int ms) const {
    if (o_.wait) return o_.wait(ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return true;
  }

  bool fail(failure f, const http_response* r = nullptr) {
    why = f;
    if (r) retry_after = r->retry_after;
    return false;
  }

  bool directory() {
    const auto r = o_.http({"GET", o_.directory, {}}, kTimeoutMs);
    if (r.status != 200) return fail(r.status ? classify(r) : failure::network, &r);
    const auto doc = json::parse(r.body, 4);
    const std::string* nn = doc ? doc->str("newNonce") : nullptr;
    const std::string* na = doc ? doc->str("newAccount") : nullptr;
    const std::string* no = doc ? doc->str("newOrder") : nullptr;
    if (!nn || !na || !no) return fail(failure::bad_answer);
    new_nonce_ = *nn;
    new_account_ = *na;
    new_order_ = *no;
    return true;
  }

  // A signed POST (`payload` "" is POST-as-GET), with a fresh nonce on badNonce once.
  http_response post(const std::string& url, const std::string& payload, bool with_jwk = false) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (nonce_.empty()) {
        const auto n = o_.http({"GET", new_nonce_, {}}, kTimeoutMs);
        if (n.nonce.empty()) return n.status ? http_response{0, {}, {}, {}, 0} : n;
        nonce_ = n.nonce;
      }
      json::writer p;
      p.begin_object().key("alg").string("ES256");
      if (with_jwk) {
        p.key("jwk").raw(jwk_);
      } else {
        p.key("kid").string(kid_);
      }
      p.key("nonce").string(nonce_).key("url").string(url).end_object();
      nonce_.clear();
      const std::string protected64 = b64url(p.str());
      const std::string payload64 = b64url(payload);
      const std::string sig = es256(account_, protected64 + "." + payload64);
      if (sig.empty()) return {};
      json::writer jws;
      jws.begin_object()
          .key("protected").string(protected64)
          .key("payload").string(payload64)
          .key("signature").string(b64url(sig))
          .end_object();
      auto r = o_.http({"POST", url, jws.take()}, kTimeoutMs);
      nonce_ = r.nonce;
      if (r.status == 400 && has(r.body, "badNonce") && attempt == 0) continue;
      return r;
    }
    return {};
  }

  bool account() {
    const auto r = post(new_account_, R"({"termsOfServiceAgreed":true})", true);
    if ((r.status != 200 && r.status != 201) || r.location.empty()) return fail(classify(r), &r);
    kid_ = r.location;
    return true;
  }

  // Polls `url` (POST-as-GET) until its status leaves `pending`/`processing`.
  std::optional<json::value> poll(const std::string& url) {
    for (int i = 0; i < o_.polls; ++i) {
      const auto r = post(url, "");
      if (r.status != 200) {
        fail(classify(r), &r);
        return std::nullopt;
      }
      auto doc = json::parse(r.body, 8);
      const std::string* st = doc ? doc->str("status") : nullptr;
      if (!st) {
        fail(failure::bad_answer);
        return std::nullopt;
      }
      if (*st != "pending" && *st != "processing") return doc;
      if (!wait(o_.poll_ms)) {
        fail(failure::stopped);
        return std::nullopt;
      }
    }
    fail(failure::network);  // still pending after every poll: the CA is slow, try later
    return std::nullopt;
  }

  const std::string& new_order() const { return new_order_; }
  const std::string& jwk() const { return jwk_; }

 private:
  const order_options& o_;
  EVP_PKEY* account_;
  std::string jwk_, kid_, nonce_, new_nonce_, new_account_, new_order_;
};

}  // namespace

bool available() noexcept { return true; }

const char* failure_words(failure f) noexcept {
  switch (f) {
    case failure::none: return "issued";
    case failure::unsupported: return "this build cannot get one";
    case failure::network: return "Let's Encrypt did not answer";
    case failure::rate_limited: return "Let's Encrypt asked to wait (too many tries)";
    case failure::dns: return "the name's TXT record could not be set or seen (check the DuckDNS token, or the "
                              "_acme-challenge CNAME of your own domain)";
    case failure::refused: return "Let's Encrypt refused it";
    case failure::bad_answer: return "an answer that was not Let's Encrypt's";
    case failure::stopped: return "stopped";
  }
  return "?";
}

std::string b64url(std::string_view bytes) {
  if (bytes.empty()) return {};
  std::string out(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::string dns01_value(std::string_view token, std::string_view account_key_pem) {
  auto key = key_of(account_key_pem);
  if (!key) return {};
  const std::string auth = std::string(token) + "." + b64url(sha256(jwk_of(key.get())));
  return b64url(sha256(auth));
}

outcome order(const order_options& o) {
  outcome out;
  if (sodium_init() < 0) {}  // safe to repeat
  pkey_ptr account = o.account_key_pem.empty() ? new_p256() : key_of(o.account_key_pem);
  if (!account) {
    out.why = failure::refused;
    return out;
  }
  out.account_key_pem = o.account_key_pem.empty() ? pem_of(account.get()) : o.account_key_pem;
  session s(o, account.get());
  const auto done = [&](failure f) {
    out.why = f;
    out.retry_after = s.retry_after;
    return out;
  };
  if (!s.directory() || !s.account()) return done(s.why);

  json::writer req;
  req.begin_object().key("identifiers").begin_array();
  req.begin_object().key("type").string("dns").key("value").string(o.name).end_object();
  req.end_array().end_object();
  const auto made = s.post(s.new_order(), req.str());
  if (made.status != 201 || made.location.empty()) {
    s.fail(classify(made), &made);
    return done(s.why);
  }
  const std::string order_url = made.location;
  const auto ord = json::parse(made.body, 8);
  const json::value* authzs = ord ? ord->find("authorizations") : nullptr;
  const std::string* finalize = ord ? ord->str("finalize") : nullptr;
  if (!authzs || authzs->k != json::kind::array || authzs->a.size() != 1 || !finalize) return done(failure::bad_answer);
  const std::string finalize_url = *finalize;

  // The one authorization: its dns-01 challenge, unless the CA already holds it valid.
  const std::string authz_url = authzs->a[0].s;
  const auto az = s.post(authz_url, "");
  if (az.status != 200) {
    s.fail(classify(az), &az);
    return done(s.why);
  }
  const auto azd = json::parse(az.body, 8);
  const std::string* az_status = azd ? azd->str("status") : nullptr;
  if (!az_status) return done(failure::bad_answer);
  if (*az_status != "valid") {
    const json::value* ch = azd->find("challenges");
    std::string ch_url, token;
    if (ch && ch->k == json::kind::array) {
      for (const auto& c : ch->a) {
        const std::string* t = c.str("type");
        if (t && *t == "dns-01" && c.str("url") && c.str("token")) {
          ch_url = *c.str("url");
          token = *c.str("token");
        }
      }
    }
    if (ch_url.empty() || token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
                              std::string::npos) {
      return done(failure::bad_answer);
    }
    const std::string txt = b64url(sha256(token + "." + b64url(sha256(s.jwk()))));
    if (!o.txt || !o.txt(txt)) return done(failure::dns);
    struct clear_txt {
      const txt_fn& f;
      ~clear_txt() { (void)f(""); }
    } cleared{o.txt};
    if (!s.wait(o.propagation_ms)) return done(failure::stopped);
    const auto go = s.post(ch_url, "{}");
    if (go.status != 200) {
      s.fail(classify(go), &go);
      return done(s.why);
    }
    const auto result = s.poll(authz_url);
    if (!result) return done(s.why);
    if (*result->str("status") != "valid") {
      // The CA's word is in the challenge's error: most often it did not see the TXT.
      http_response r;
      r.status = 403;
      r.body = az.body;
      for (const auto& c : result->find("challenges") ? result->find("challenges")->a : std::vector<json::value>{}) {
        if (const json::value* e = c.find("error")) {
          if (const std::string* t = e->str("type")) r.body = *t;
        }
      }
      const failure f = classify(r);
      return done(f == failure::refused ? failure::dns : f);
    }
  }

  // The certificate's own key, its CSR, and finalize.
  pkey_ptr cert_key = new_p256();
  if (!cert_key) return done(failure::refused);
  const std::string csr = csr_of(cert_key.get(), o.name);
  if (csr.empty()) return done(failure::refused);
  json::writer fin;
  fin.begin_object().key("csr").string(b64url(csr)).end_object();
  const auto f = s.post(finalize_url, fin.str());
  if (f.status != 200) {
    s.fail(classify(f), &f);
    return done(s.why);
  }
  const auto final_order = s.poll(order_url);
  if (!final_order) return done(s.why);
  const std::string* cert_url = final_order->str("certificate");
  if (*final_order->str("status") != "valid" || !cert_url) return done(failure::refused);
  const auto chain = s.post(*cert_url, "");
  if (chain.status != 200) {
    s.fail(classify(chain), &chain);
    return done(s.why);
  }
  const auto info = inspect(chain.body, o.name);
  if (!info || !info->for_name) return done(failure::bad_answer);
  out.chain_pem = chain.body;
  out.key_pem = pem_of(cert_key.get());
  out.not_after = info->not_after;
  return done(failure::none);
}

std::optional<cert_info> inspect(std::string_view pem, std::string_view name) {
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<ossl_ssize_t>(pem.size())));
  if (!bio) return std::nullopt;
  bssl::UniquePtr<X509> x(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!x) return std::nullopt;
  cert_info info;
  std::int64_t end = 0;
  if (!ASN1_TIME_to_posix(X509_get0_notAfter(x.get()), &end)) return std::nullopt;
  info.not_after = end;
  info.self_signed = X509_NAME_cmp(X509_get_subject_name(x.get()), X509_get_issuer_name(x.get())) == 0;
  bssl::UniquePtr<GENERAL_NAMES> names(
      static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(x.get(), NID_subject_alt_name, nullptr, nullptr)));
  for (std::size_t i = 0; names && i < sk_GENERAL_NAME_num(names.get()); ++i) {
    const GENERAL_NAME* g = sk_GENERAL_NAME_value(names.get(), i);
    if (g->type != GEN_DNS) continue;
    const auto* s = g->d.dNSName;
    const std::string_view dns(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                               static_cast<std::size_t>(ASN1_STRING_length(s)));
    if (dns.size() == name.size()) {
      bool same = true;
      for (std::size_t c = 0; c < dns.size(); ++c) {
        const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
        same = same && lower(dns[c]) == lower(name[c]);
      }
      info.for_name = info.for_name || same;
    }
  }
  return info;
}

}  // namespace mv::driftwire::acme
