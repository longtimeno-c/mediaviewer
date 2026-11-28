// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A real certificate for the relay's web-site look (docs/plans/vpn-stealth.md
// §1): an ACME client (RFC 8555) that gets one from Let's Encrypt by DNS-01,
// the TXT record set through DuckDNS (ddns::set_txt), so no port 80 is
// needed. One name per certificate: DuckDNS keeps one TXT value for a name,
// so the person's own domain (whose `_acme-challenge` is a CNAME to the
// DuckDNS name's) or the DuckDNS name, not both.
//
//   order()  the whole exchange, on the caller's thread (a keeper's worker):
//            the directory, a nonce, the account (made once, its key kept
//            by the caller), the order, the TXT set and a wait for it to
//            reach the CA's resolvers, the challenge, polling, a fresh P-256
//            key and its CSR, finalize, polling, the chain; the TXT cleared.
//            Every request is a JWS signed with the account key (ES256).
//
// The certificate is camouflage, as tls.h says: the hop inside proves the
// relay's key, so a failed or wrong issuance never weakens the VPN; the
// relay keeps a self-signed one until a real one is in hand.
//
// The HTTP is the caller's (http_fn): the command line's own HTTPS client
// (cli::https_request; WinHTTP, NSURLSession, libcurl) in the host, a
// loopback stand-in in the tests, never the real Let's Encrypt in a test.
// Nothing here logs; the caller's status words carry the name only (it is
// the person's own and public in CT logs anyway), never the token.
//
// BoringSSL for the keys, the signatures and the CSR (available(): false in
// a build without it, as tls.h). Worker threads only. Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mv::driftwire::acme {

inline constexpr const char* kLetsEncrypt = "https://acme-v02.api.letsencrypt.org/directory";
inline constexpr int kTimeoutMs = 30000;

// Whether this build can (BoringSSL).
[[nodiscard]] bool available() noexcept;

struct http_request {
  std::string method;  // "GET" or "POST" (a POST's body is application/jose+json)
  std::string url;
  std::string body;
};
struct http_response {
  int status = 0;  // 0: nothing answered
  std::string body;
  std::string location;          // Location
  std::string nonce;             // Replay-Nonce
  std::int64_t retry_after = 0;  // Retry-After, seconds (0: none)
};
using http_fn = std::function<http_response(const http_request&, int timeout_ms)>;

// DNS-01's record: "" clears it. True when the provider took it.
using txt_fn = std::function<bool(const std::string& value)>;

enum class failure : std::uint8_t {
  none,
  unsupported,   // no BoringSSL in this build
  network,       // the CA did not answer
  rate_limited,  // the CA said too many (retry_after says when)
  dns,           // the TXT could not be set, or the CA did not see it
  refused,       // the CA said no (an account, an order, a name it will not sign)
  bad_answer,    // an answer that was not ACME's
  stopped,
};
[[nodiscard]] const char* failure_words(failure f) noexcept;

struct order_options {
  std::string directory = kLetsEncrypt;
  std::string name;              // the one name the certificate is for
  std::string account_key_pem;   // the account's key from before; "" makes one (issued::account_key_pem)
  http_fn http;
  txt_fn txt;
  // Waits `ms` (the TXT reaching the CA's resolvers; between polls), or
  // returns false when stopping. Empty: a plain sleep.
  std::function<bool(int ms)> wait;
  int propagation_ms = 60000;  // after the TXT is set, before the CA is told to look
  int poll_ms = 3000;
  int polls = 40;
};

struct outcome {
  failure why = failure::none;
  std::string chain_pem;        // the certificate and its issuers, as the CA gave them
  std::string key_pem;          // the certificate's own key (P-256), PKCS#8
  std::string account_key_pem;  // the account's (as given, or made now): keep it for the next order
  std::int64_t not_after = 0;   // the certificate's end, unix seconds
  std::int64_t retry_after = 0; // rate_limited: the CA's word on when, seconds (0: none)
};
[[nodiscard]] outcome order(const order_options& o);

// The end of the first certificate in `pem` (unix seconds), and whether it is
// for `name` (its subject alternative names). nullopt: not a certificate.
struct cert_info {
  std::int64_t not_after = 0;
  bool for_name = false;
  bool self_signed = false;  // issuer == subject
};
[[nodiscard]] std::optional<cert_info> inspect(std::string_view pem, std::string_view name);
// Whether `key_pem` is the private key of the first certificate in `cert_pem`.
[[nodiscard]] bool key_matches(std::string_view cert_pem, std::string_view key_pem);

// The pieces (the tests and the stand-in CA).
// base64url without padding.
[[nodiscard]] std::string b64url(std::string_view bytes);
// The DNS-01 TXT value for a token and an account key (RFC 8555 §8.4).
[[nodiscard]] std::string dns01_value(std::string_view token, std::string_view account_key_pem);

}  // namespace mv::driftwire::acme
