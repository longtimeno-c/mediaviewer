// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A real certificate for the VPN's web-site look (addons/driftwire/acme.h,
// docs/plans/vpn-stealth.md §1): the ACME client against a stand-in CA in
// this process that checks every JWS signature, nonce and URL as Let's
// Encrypt does, and looks the DNS-01 TXT up in a stand-in DuckDNS; the
// certificate keeper's schedule; the relay taking a new certificate. Never
// Let's Encrypt, never duckdns.org.
#include "catch_compat.h"

#if defined(MV_DRIFTWIRE_TLS)  // the stand-in CA is BoringSSL too

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sodium.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "addons/driftwire/acme.h"
#include "addons/driftwire/cli/host_keepers.h"
#include "addons/driftwire/ddns.h"
#include "core/json.h"
#include "import_fixture.h"
#include "wire/secrets.h"
#include "wire/splice.h"
#include "wire/tls.h"

namespace acme = mv::driftwire::acme;
namespace ddns = mv::driftwire::ddns;
namespace cli = mv::driftwire::cli;
namespace relay = mv::driftwire::relay;
namespace json = mv::json;
namespace fs = std::filesystem;
using mv::test::scratch_dir;
using mv::test::write_bytes;

namespace {

constexpr const char* kToken = "0a1b2c3d-4e5f-6a7b-8c9d-0e1f2a3b4c5d";
constexpr const char* kSub = "myhome-42";
constexpr const char* kName = "myhome-42.duckdns.org";
constexpr const char* kBase = "https://acme.test";  // never fetched: the stand-in answers in process

std::string unb64(std::string_view s) {
  std::string out(s.size(), '\0');
  std::size_t len = 0;
  if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), s.data(), s.size(), nullptr, &len,
                        nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
    return {};
  }
  out.resize(len);
  return out;
}

template <class F>
bool eventually(F f, int seconds = 15) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < until) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return f();
}

std::string sha256(std::string_view s) {
  std::string out(32, '\0');
  crypto_hash_sha256(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(s.data()),
                     s.size());
  return out;
}

// A stand-in DuckDNS: the TXT record of kSub, as the fetch sets it.
struct duck {
  std::mutex m;
  std::string txt;
  std::vector<std::string> urls;
  bool refuse = false;
  ddns::fetch_fn fetch() {
    return [this](const std::string& url, int) {
      std::lock_guard g(m);
      urls.push_back(url);
      if (refuse) return ddns::fetch_answer{200, "KO"};
      const auto at = url.find("&txt=");
      const auto end = url.find('&', at + 5);
      txt = url.substr(at + 5, end - at - 5);
      if (url.find("&clear=true") != std::string::npos) txt.clear();
      return ddns::fetch_answer{200, "OK\n" + txt + "\nUPDATED"};
    };
  }
};

// The stand-in CA (RFC 8555, the parts the client uses), strict as Let's
// Encrypt: a POST must carry a nonce it gave and has not seen, the URL it
// was sent to, and a valid ES256 signature by the jwk (a new account) or the
// account its kid names; a challenge passes when the TXT is the key
// authorization's digest.
class stand_in_ca {
 public:
  explicit stand_in_ca(duck& d) : dns_(d) {
    bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
    REQUIRE(EC_KEY_generate_key(ec.get()));
    ca_key_.reset(EVP_PKEY_new());
    REQUIRE(EVP_PKEY_assign_EC_KEY(ca_key_.get(), ec.release()));
    ca_.reset(X509_new());
    X509_set_version(ca_.get(), X509_VERSION_3);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(ca_.get()), "CN", MBSTRING_UTF8,
                               reinterpret_cast<const std::uint8_t*>("Stand-in CA"), -1, -1, 0);
    X509_set_issuer_name(ca_.get(), X509_get_subject_name(ca_.get()));
    X509_set_pubkey(ca_.get(), ca_key_.get());
    const std::time_t now = std::time(nullptr);
    X509_time_adj_ex(X509_getm_notBefore(ca_.get()), -1, 0, &now);
    X509_time_adj_ex(X509_getm_notAfter(ca_.get()), 3650, 0, &now);
    REQUIRE(X509_sign(ca_.get(), ca_key_.get(), EVP_sha256()));
  }

  // What the client was told on the way.
  std::atomic<int> bad_nonce_once{0};  // answer badNonce to the next POST this many times
  std::atomic<int> rate_limit{0};      // answer 429 with Retry-After to newOrder this many times
  int leaf_days = 90;
  int accounts_made = 0, orders = 0, rejected = 0, checked_txt = 0;
  std::string last_error;

  acme::http_fn http() {
    return [this](const acme::http_request& r, int) { return answer(r); };
  }

 private:
  struct account {
    bssl::UniquePtr<EC_KEY> key;
    std::string thumb;
  };
  struct order {
    std::string name, token, status = "pending", chain;
    std::string account;
  };

  acme::http_response answer(const acme::http_request& r) {
    std::lock_guard g(m_);
    acme::http_response out;
    out.nonce = fresh_nonce();
    if (r.method == "GET" && r.url == std::string(kBase) + "/directory") {
      out.status = 200;
      out.body = std::string(R"({"newNonce":")") + kBase + R"(/nonce","newAccount":")" + kBase +
                 R"(/account","newOrder":")" + kBase + R"(/order"})";
      return out;
    }
    if (r.method == "GET" && r.url == std::string(kBase) + "/nonce") {
      out.status = 204;
      return out;
    }
    if (r.method != "POST") return fail(out, 405, "malformed");
    // The JWS: protected header, payload, signature.
    const auto jws = json::parse(r.body, 4);
    const std::string* p64 = jws ? jws->str("protected") : nullptr;
    const std::string* body64 = jws ? jws->str("payload") : nullptr;
    const std::string* sig64 = jws ? jws->str("signature") : nullptr;
    if (!p64 || !body64 || !sig64) return fail(out, 400, "malformed");
    const auto prot = json::parse(unb64(*p64), 4);
    if (!prot || !prot->str("alg") || *prot->str("alg") != "ES256") return fail(out, 400, "badSignatureAlgorithm");
    const std::string* nonce = prot->str("nonce");
    if (!nonce || !given_.count(*nonce) || used_.count(*nonce)) return fail(out, 400, "badNonce");
    used_.insert(*nonce);
    if (bad_nonce_once.load() > 0) {
      --bad_nonce_once;
      return fail(out, 400, "badNonce");
    }
    if (!prot->str("url") || *prot->str("url") != r.url) return fail(out, 401, "unauthorized");
    // Who signs: a jwk (newAccount only) or a kid we made.
    std::string kid;
    EC_KEY* key = nullptr;
    bssl::UniquePtr<EC_KEY> from_jwk;
    std::string thumb;
    if (const json::value* jwk = prot->find("jwk")) {
      if (r.url != std::string(kBase) + "/account") return fail(out, 400, "malformed");
      const std::string x = unb64(*jwk->str("x")), y = unb64(*jwk->str("y"));
      from_jwk.reset(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
      bssl::UniquePtr<BIGNUM> bx(BN_bin2bn(reinterpret_cast<const std::uint8_t*>(x.data()), x.size(), nullptr));
      bssl::UniquePtr<BIGNUM> by(BN_bin2bn(reinterpret_cast<const std::uint8_t*>(y.data()), y.size(), nullptr));
      if (!EC_KEY_set_public_key_affine_coordinates(from_jwk.get(), bx.get(), by.get())) {
        return fail(out, 400, "badPublicKey");
      }
      key = from_jwk.get();
      thumb = acme::b64url(sha256(R"({"crv":"P-256","kty":"EC","x":")" + *jwk->str("x") + R"(","y":")" +
                                  *jwk->str("y") + R"("})"));
    } else if (const std::string* k = prot->str("kid"); k && accounts_.count(*k)) {
      kid = *k;
      key = accounts_[kid].key.get();
    } else {
      return fail(out, 400, "accountDoesNotExist");
    }
    const std::string sig = unb64(*sig64);
    if (sig.size() != 64) return fail(out, 400, "malformed");
    const std::string digest = sha256(*p64 + "." + *body64);
    bssl::UniquePtr<ECDSA_SIG> es(ECDSA_SIG_new());
    BIGNUM* rr = BN_bin2bn(reinterpret_cast<const std::uint8_t*>(sig.data()), 32, nullptr);
    BIGNUM* ss = BN_bin2bn(reinterpret_cast<const std::uint8_t*>(sig.data()) + 32, 32, nullptr);
    ECDSA_SIG_set0(es.get(), rr, ss);
    if (ECDSA_do_verify(reinterpret_cast<const std::uint8_t*>(digest.data()), digest.size(), es.get(), key) != 1) {
      return fail(out, 400, "unauthorized");
    }
    const std::string payload = unb64(*body64);
    const std::string url = r.url;

    if (url == std::string(kBase) + "/account") {
      for (auto& [id, a] : accounts_) {
        if (a.thumb == thumb) {
          out.status = 200;
          out.location = id;
          out.body = R"({"status":"valid"})";
          return out;
        }
      }
      if (payload.find("\"termsOfServiceAgreed\":true") == std::string::npos) return fail(out, 400, "malformed");
      const std::string id = std::string(kBase) + "/acct/" + std::to_string(++accounts_made);
      accounts_[id] = account{std::move(from_jwk), thumb};
      out.status = 201;
      out.location = id;
      out.body = R"({"status":"valid"})";
      return out;
    }
    if (url == std::string(kBase) + "/order") {
      if (rate_limit.load() > 0) {
        --rate_limit;
        out.retry_after = 7200;
        return fail(out, 429, "rateLimited");
      }
      const auto req = json::parse(payload, 6);
      const json::value* ids = req ? req->find("identifiers") : nullptr;
      if (!ids || ids->a.size() != 1 || !ids->a[0].str("value")) return fail(out, 400, "malformed");
      const int n = ++orders;
      order o;
      o.name = *ids->a[0].str("value");
      o.account = kid;
      std::array<std::uint8_t, 16> t{};
      RAND_bytes(t.data(), t.size());
      o.token = acme::b64url(std::string(reinterpret_cast<const char*>(t.data()), t.size()));
      orders_[n] = o;
      out.status = 201;
      out.location = std::string(kBase) + "/o/" + std::to_string(n);
      out.body = order_json(n);
      return out;
    }
    // The rest are an order's own: /o/N, /a/N, /c/N, /f/N, /cert/N.
    const auto slash = url.rfind('/');
    const int n = std::atoi(url.c_str() + slash + 1);
    if (!orders_.count(n) || orders_[n].account != kid) return fail(out, 403, "unauthorized");
    order& o = orders_[n];
    const std::string kind = url.substr(std::string(kBase).size(), slash - std::string(kBase).size());
    if (kind == "/a") {
      out.status = 200;
      out.body = authz_json(n);
      return out;
    }
    if (kind == "/c") {
      // The CA looks the TXT up now: the key authorization's digest.
      ++checked_txt;
      std::string seen;
      {
        std::lock_guard d(dns_.m);
        seen = dns_.txt;
      }
      const std::string want = acme::b64url(sha256(o.token + "." + accounts_[kid].thumb));
      o.status = seen == want ? "valid" : "invalid";
      out.status = 200;
      out.body = R"({"type":"dns-01","status":"processing"})";
      return out;
    }
    if (kind == "/o") {
      out.status = 200;
      out.body = order_json(n);
      return out;
    }
    if (kind == "/f") {
      if (o.status != "valid") return fail(out, 403, "orderNotReady");
      const auto req = json::parse(payload, 4);
      const std::string der = req && req->str("csr") ? unb64(*req->str("csr")) : std::string();
      const auto* p = reinterpret_cast<const std::uint8_t*>(der.data());
      bssl::UniquePtr<X509_REQ> csr(d2i_X509_REQ(nullptr, &p, static_cast<long>(der.size())));
      if (!csr) return fail(out, 400, "badCSR");
      bssl::UniquePtr<EVP_PKEY> pub(X509_REQ_get_pubkey(csr.get()));
      if (!pub || X509_REQ_verify(csr.get(), pub.get()) != 1) return fail(out, 400, "badCSR");
      o.chain = issue(pub.get(), o.name) + pem_of_ca();
      o.status = "issued";
      out.status = 200;
      out.body = order_json(n);
      return out;
    }
    if (kind == "/cert") {
      out.status = 200;
      out.body = o.chain;
      return out;
    }
    return fail(out, 404, "malformed");
  }

  acme::http_response& fail(acme::http_response& out, int status, const char* type) {
    ++rejected;
    last_error = type;
    out.status = status;
    out.body = std::string(R"({"type":"urn:ietf:params:acme:error:)") + type + R"("})";
    return out;
  }

  std::string fresh_nonce() {
    std::array<std::uint8_t, 12> b{};
    RAND_bytes(b.data(), b.size());
    std::string n = acme::b64url(std::string(reinterpret_cast<const char*>(b.data()), b.size()));
    given_.insert(n);
    return n;
  }

  std::string order_json(int n) {
    const order& o = orders_[n];
    const std::string id = std::to_string(n);
    const std::string st = o.status == "issued" ? "valid" : o.status == "valid" ? "ready" : o.status;
    std::string j = R"({"status":")" + st + R"(","authorizations":[")" + kBase + "/a/" + id + R"("],"finalize":")" +
                    kBase + "/f/" + id + "\"";
    if (o.status == "issued") j += std::string(R"(,"certificate":")") + kBase + "/cert/" + id + "\"";
    return j + "}";
  }

  std::string authz_json(int n) {
    const order& o = orders_[n];
    const std::string id = std::to_string(n);
    const std::string st = o.status == "issued" ? "valid" : o.status;
    std::string err = st == "invalid" ? R"(,"error":{"type":"urn:ietf:params:acme:error:unauthorized"})" : "";
    return R"({"status":")" + st + R"(","identifier":{"type":"dns","value":")" + o.name +
           R"("},"challenges":[{"type":"http-01","url":")" + kBase + "/h/" + id + R"(","token":"x"},)" +
           R"({"type":"dns-01","url":")" + kBase + "/c/" + id + R"(","token":")" + o.token + "\"" + err + "}]}";
  }

  std::string issue(EVP_PKEY* pub, const std::string& name) {
    bssl::UniquePtr<X509> x(X509_new());
    X509_set_version(x.get(), X509_VERSION_3);
    ASN1_INTEGER_set(X509_get_serialNumber(x.get()), ++serial_);
    X509_set_issuer_name(x.get(), X509_get_subject_name(ca_.get()));
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x.get()), "CN", MBSTRING_UTF8,
                               reinterpret_cast<const std::uint8_t*>(name.data()), -1, -1, 0);
    const std::time_t now = std::time(nullptr);
    X509_time_adj_ex(X509_getm_notBefore(x.get()), -1, 0, &now);
    X509_time_adj_ex(X509_getm_notAfter(x.get()), leaf_days, 0, &now);
    X509_set_pubkey(x.get(), pub);
    const std::string san = "DNS:" + name;
    bssl::UniquePtr<X509_EXTENSION> ext(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str()));
    X509_add_ext(x.get(), ext.get(), -1);
    X509_sign(x.get(), ca_key_.get(), EVP_sha256());
    return pem(x.get());
  }
  std::string pem_of_ca() { return pem(ca_.get()); }
  static std::string pem(X509* x) {
    bssl::UniquePtr<BIO> b(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(b.get(), x);
    const std::uint8_t* d = nullptr;
    std::size_t n = 0;
    BIO_mem_contents(b.get(), &d, &n);
    return std::string(reinterpret_cast<const char*>(d), n);
  }

  duck& dns_;
  std::mutex m_;
  bssl::UniquePtr<EVP_PKEY> ca_key_;
  bssl::UniquePtr<X509> ca_;
  std::set<std::string> given_, used_;
  std::map<std::string, account> accounts_;
  std::map<int, order> orders_;
  long serial_ = 1;
};

acme::order_options options_for(stand_in_ca& ca, duck& d) {
  acme::order_options o;
  o.directory = std::string(kBase) + "/directory";
  o.name = kName;
  o.http = ca.http();
  const auto fetch = d.fetch();
  o.txt = [fetch](const std::string& v) { return ddns::set_duckdns_txt(fetch, "http://duck.test", kSub, kToken, v); };
  o.wait = [](int) { return true; };  // no propagation to wait for in process
  o.propagation_ms = 0;
  o.poll_ms = 0;
  return o;
}

}  // namespace

TEST_CASE("acme: a certificate by DNS-01 through DuckDNS's TXT, from a CA that checks every signature",
          "[acme][vpn-stealth]") {
  if (!acme::available()) SKIP("this build has no BoringSSL");
  duck d;
  stand_in_ca ca(d);
  auto o = options_for(ca, d);
  const auto got = acme::order(o);
  INFO(acme::failure_words(got.why) << " / " << ca.last_error);
  REQUIRE(got.why == acme::failure::none);
  CHECK(ca.rejected == 0);
  CHECK(ca.accounts_made == 1);
  CHECK(ca.checked_txt == 1);
  // The chain is for the name, from the CA; the key is the certificate's own, and the account's kept.
  const auto info = acme::inspect(got.chain_pem, kName);
  REQUIRE(info);
  CHECK(info->for_name);
  CHECK_FALSE(info->self_signed);
  CHECK(info->not_after == got.not_after);
  CHECK(got.not_after > std::time(nullptr) + 80 * 86400);
  CHECK_FALSE(acme::inspect(got.chain_pem, "other.duckdns.org")->for_name);
  CHECK(got.key_pem.find("BEGIN PRIVATE KEY") != std::string::npos);
  CHECK(got.account_key_pem.find("BEGIN PRIVATE KEY") != std::string::npos);
  CHECK(got.account_key_pem != got.key_pem);
  // DuckDNS: the TXT set (the right value, nothing but base64url), then cleared; the token in the URL only.
  {
    std::lock_guard g(d.m);
    REQUIRE(d.urls.size() == 2);
    CHECK(d.urls[0].find("domains=myhome-42&token=" + std::string(kToken) + "&txt=") != std::string::npos);
    CHECK(d.urls[1].find("&txt=&clear=true") != std::string::npos);
    CHECK(d.txt.empty());
  }
  // The next order reuses the account (Let's Encrypt's limits count accounts too).
  o.account_key_pem = got.account_key_pem;
  const auto again = acme::order(o);
  REQUIRE(again.why == acme::failure::none);
  CHECK(ca.accounts_made == 1);
  CHECK(again.account_key_pem == got.account_key_pem);
  CHECK(again.key_pem != got.key_pem);  // a fresh key each certificate
}

TEST_CASE("acme: a stale nonce is retried; a TXT not set, a refused DuckDNS and a rate limit say why",
          "[acme][vpn-stealth]") {
  if (!acme::available()) SKIP("this build has no BoringSSL");
  duck d;
  stand_in_ca ca(d);
  // badNonce once: a fresh nonce, the same request again.
  ca.bad_nonce_once = 1;
  CHECK(acme::order(options_for(ca, d)).why == acme::failure::none);
  // DuckDNS says KO (a wrong token): no challenge is even asked.
  d.refuse = true;
  const int checked = ca.checked_txt;
  CHECK(acme::order(options_for(ca, d)).why == acme::failure::dns);
  CHECK(ca.checked_txt == checked);
  d.refuse = false;
  // The TXT set to something else (the CA sees the wrong value): dns, and the record still cleared.
  auto o = options_for(ca, d);
  const auto fetch = d.fetch();
  o.txt = [fetch](const std::string& v) {
    return ddns::set_duckdns_txt(fetch, "http://duck.test", kSub, kToken, v.empty() ? v : "not-the-value");
  };
  CHECK(acme::order(o).why == acme::failure::dns);
  {
    std::lock_guard g(d.m);
    CHECK(d.txt.empty());
  }
  // Too many orders: rate_limited, with the CA's Retry-After.
  ca.rate_limit = 1;
  const auto limited = acme::order(options_for(ca, d));
  CHECK(limited.why == acme::failure::rate_limited);
  CHECK(limited.retry_after == 7200);
  // Nobody there.
  auto nobody = options_for(ca, d);
  nobody.http = [](const acme::http_request&, int) { return acme::http_response{}; };
  CHECK(acme::order(nobody).why == acme::failure::network);
  // A wait that says stop: stopped, the TXT cleared.
  auto stop = options_for(ca, d);
  stop.wait = [](int) { return false; };
  CHECK(acme::order(stop).why == acme::failure::stopped);
  {
    std::lock_guard g(d.m);
    CHECK(d.txt.empty());
  }
}

TEST_CASE("acme: the certificate keeper gets one, keeps it, renews it, and backs off when it fails",
          "[acme][vpn-stealth][host]") {
  if (!acme::available()) SKIP("this build has no BoringSSL");
  scratch_dir dir("acme_keeper");
  duck d;
  stand_in_ca ca(d);
  REQUIRE(mv::driftwire::secrets::owner_only_folder(mv::test::utf8(dir / "secrets")));
  auto store = mv::driftwire::secrets::file_store(mv::test::utf8(dir / "secrets"));
  REQUIRE(store);
  REQUIRE(ddns::save_token(*store, kToken));
  std::atomic<std::int64_t> now{static_cast<std::int64_t>(std::time(nullptr))};
  cli::cert_keeper_config c;
  c.http = ca.http();
  c.fetch = d.fetch();
  c.directory = std::string(kBase) + "/directory";
  c.ddns_base = "http://duck.test";
  c.now_unix = [&] { return now.load(); };
  c.propagation_ms = 0;
  c.poll_ms = 0;
  c.on_loopback = true;
  std::atomic<int> changed{0};
  cli::keeper_context ctx;
  ctx.folder = mv::test::utf8(dir.root());
  ctx.secrets = store.get();
  ctx.vpn = true;
  ctx.relay_port = 47614;
  ctx.relay_tls_port = 47615;
  ctx.ddns_subdomain = kSub;
  ctx.cert_name = kName;
  ctx.loopback_only = true;
  ctx.cert_changed = [&] { ++changed; };
  {
    auto k = cli::make_cert_keeper(c);
    k->start(ctx);
    REQUIRE(eventually([&] { return changed.load() == 1; }));
    REQUIRE(eventually([&] { return k->status_line().find("Let's Encrypt for " + std::string(kName)) != std::string::npos; }));
    // The files: the chain, its key and the account's, the person's alone.
    const auto files = cli::usable_certificate(ctx.folder, kName, now.load());
    REQUIRE(files);
    CHECK(fs::exists(dir / "tls" / "account.pem"));
#if !defined(_WIN32)
    CHECK((fs::status(dir / "tls" / "key.pem").permissions() & fs::perms::group_all) == fs::perms::none);
#endif
    CHECK_FALSE(cli::usable_certificate(ctx.folder, "other.example.com", now.load()));
    // A key that is not the certificate's (a write cut between the two files): no certificate.
    const auto key = mv::test::read_bytes(dir / "tls" / "key.pem");
    fs::copy_file(dir / "tls" / "account.pem", dir / "tls" / "key.pem", fs::copy_options::overwrite_existing);
    CHECK_FALSE(cli::usable_certificate(ctx.folder, kName, now.load()));
    write_bytes(dir / "tls" / "key.pem", key);
    CHECK(cli::usable_certificate(ctx.folder, kName, now.load()));
    // Asked again (the ports changed): a good certificate is kept, no new order.
    k->ports_changed(ctx);
    k->ports_changed(ctx);
    std::this_thread::yield();
    k->stop();
    CHECK(ca.orders == 1);
    CHECK(changed.load() == 1);
  }
  // 70 days on (under 30 left of 90): renewed.
  now += 70 * 86400;
  {
    auto k = cli::make_cert_keeper(c);
    k->start(ctx);
    REQUIRE(eventually([&] { return changed.load() == 2; }));
    k->stop();
    CHECK(ca.orders == 2);
    CHECK(ca.accounts_made == 1);  // the account kept in tls/account.pem
  }
  // The look off: nothing asked; no DuckDNS name: nothing asked, and said why.
  {
    auto off = ctx;
    off.relay_tls_port = 0;
    auto k = cli::make_cert_keeper(c);
    k->start(off);
    REQUIRE(eventually([&] { return k->status_line().find("web-site look is off") != std::string::npos; }));
    auto none = ctx;
    none.ddns_subdomain.clear();
    k->ports_changed(none);
    REQUIRE(eventually([&] { return k->status_line().find("needs a DuckDNS name") != std::string::npos; }));
    k->stop();
    CHECK(ca.orders == 2);
  }
  // A failure (the CA does not see the TXT): self-signed said, and no second order before the back-off ends.
  scratch_dir dir2("acme_keeper_fail");
  auto fail = ctx;
  fail.folder = mv::test::utf8(dir2.root());
  d.refuse = true;
  {
    auto k = cli::make_cert_keeper(c);
    k->start(fail);
    REQUIRE(eventually([&] { return k->status_line().find("self-signed: the name's TXT") != std::string::npos; }));
    k->ports_changed(fail);
    REQUIRE(eventually([&] { return k->status_line().find("trying again") != std::string::npos; }));
    k->stop();
  }
  CHECK_FALSE(cli::usable_certificate(fail.folder, kName, now.load()));
  d.refuse = false;
}

TEST_CASE("acme: the relay takes a new certificate without a restart", "[acme][vpn-stealth][driftwire]") {
  if (!acme::available() || !mv::driftwire::tls::available()) SKIP("this build has no BoringSSL");
  scratch_dir dir("acme_relay_swap");
  duck d;
  stand_in_ca ca(d);
  const auto got = acme::order(options_for(ca, d));
  REQUIRE(got.why == acme::failure::none);
  write_bytes(dir / "cert.pem", std::vector<std::uint8_t>(got.chain_pem.begin(), got.chain_pem.end()));
  write_bytes(dir / "key.pem", std::vector<std::uint8_t>(got.key_pem.begin(), got.key_pem.end()));
  relay::server::options o;
  o.key = "TEST-RELAY-KEY-0123456789";
  o.loopback_only = true;
  o.tls_port = 1;
  o.tls_name = kName;
  auto s = relay::server::start(o);
  REQUIRE(s);
  CHECK((*s)->set_tls_certificate(mv::test::utf8(dir / "cert.pem"), mv::test::utf8(dir / "key.pem")));
  // A key that is not the certificate's: refused, the old one kept.
  write_bytes(dir / "other.pem", std::vector<std::uint8_t>(got.account_key_pem.begin(), got.account_key_pem.end()));
  CHECK_FALSE((*s)->set_tls_certificate(mv::test::utf8(dir / "cert.pem"), mv::test::utf8(dir / "other.pem")));
  // Still a web site to a browser, on the new certificate.
  auto c = mv::driftwire::net::connect({"127.0.0.1", (*s)->tls_port()}, 3000);
  REQUIRE(c);
  auto t = mv::driftwire::tls::client(std::move(*c), kName, nullptr);
  REQUIRE(t);
  const std::string req = "GET / HTTP/1.0\r\n\r\n";
  REQUIRE((*t)->write_all({reinterpret_cast<const std::uint8_t*>(req.data()), req.size()}));
  std::vector<std::uint8_t> buf(4096);
  auto n = (*t)->read_some(buf, 5000);
  REQUIRE(n);
  CHECK(std::string(reinterpret_cast<const char*>(buf.data()), *n).rfind("HTTP/1.1 200 OK", 0) == 0);
  // A relay without the look has nothing to swap.
  o.tls_port = 0;
  auto plain = relay::server::start(o);
  REQUIRE(plain);
  CHECK_FALSE((*plain)->set_tls_certificate(mv::test::utf8(dir / "cert.pem"), mv::test::utf8(dir / "key.pem")));
}

#endif  // MV_DRIFTWIRE_TLS
