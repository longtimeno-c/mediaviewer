// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Tunnel looking like HTTPS (slice 4; docs/plans/driftwire-tls-spike.md,
// docs/design/28 "The Tunnel"): the connection to the relay server is TLS 1.3
// on TCP 443 with a ClientHello a filter takes for Chrome's, the relay's
// sealed hop (splice.h) inside it unchanged, and a connection that does not
// prove the server key answered as an ordinary web site.
//
// TLS here is camouflage, not security: the client does not check the
// certificate (the hop inside proves the key, CPace then Noise). What gates
// the hop is the knock: the client's first kKnockBytes inside TLS are a keyed
// BLAKE2b of this TLS session's exporter, keyed by an Argon2id hash of the
// server key. A knock is good for its own session only; anything else is
// the start of an HTTP request, and gets a web page.
//
// BoringSSL (Apache-2.0, Chrome's TLS) in tls_boringssl.cpp; tls_none.cpp
// where the build has none (available() is false). Portable (D9): links only.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "wire/link.h"
#include "core/result.h"

namespace mv::driftwire::tls {

// Whether this build carries TLS (BoringSSL).
[[nodiscard]] bool available() noexcept;

inline constexpr std::size_t kKnockBytes = 32;
inline constexpr std::uint16_t kDefaultPort = 443;
// The client's handshake and knock, and the server's, within this.
inline constexpr int kHandshakeMs = 10000;
// A connection that knocks with nothing (or less) waits this long before the
// web site answers or closes it, as a web server's header timeout.
inline constexpr int kIdleMs = 10000;

using knock_key = std::array<std::uint8_t, 32>;
// Argon2id (libsodium's interactive limits) of the server key: slow on
// purpose, so a network that breaks TLS with its own certificate and reads
// a knock cannot test guesses at a typed key quickly. Once per start on
// each side. nullopt when it could not run (out of memory).
[[nodiscard]] std::optional<knock_key> derive_knock_key(std::string_view server_key);
// The knock for a session's exporter (pure).
[[nodiscard]] std::array<std::uint8_t, kKnockBytes> knock_of(const knock_key& k,
                                                             std::span<const std::uint8_t> exporter);
inline constexpr std::string_view kExporterLabel = "EXPORTER-driftwire-knock";

// The client: TLS with Chrome's ClientHello over `l`, then the knock. `sni`:
// the relay's name, empty for an address (a browser sends none then). `k`
// nullptr: no knock, a browser's connection (the tests' prober).
// status::unsupported_format without TLS in this build.
[[nodiscard]] result<std::unique_ptr<link>> client(std::unique_ptr<link> l, const std::string& sni,
                                                   const knock_key* k, int timeout_ms = kHandshakeMs);

// The server's certificate: a PEM chain and key from files (a real one, such
// as Let's Encrypt's, is what passes a prober's look), else one made now,
// self-signed, for `name`.
class server_context {
 public:
  virtual ~server_context() = default;
};
[[nodiscard]] result<std::unique_ptr<server_context>> make_server_context(const std::string& cert_pem_path,
                                                                          const std::string& key_pem_path,
                                                                          const std::string& name);

// The web site a connection without a knock sees.
struct decoy {
  std::string dir;  // its pages (index.html and the files beside it); empty: the built-in page
};
// The answer to an HTTP request head (pure but for reading `d.dir`): a 200
// with the page, a 404, or a 400, "Server: nginx", Connection: close.
[[nodiscard]] std::string decoy_response(std::string_view request_head, const decoy& d);

enum class accepted_as : std::uint8_t { relay, website, failed };
// The server: the TLS handshake, then the knock. relay: the link for the hop
// (its first bytes are the hop's head). website: answered as a web site and
// closed (nothing returned). failed: the handshake did not finish.
[[nodiscard]] std::unique_ptr<link> accept(server_context& ctx, std::unique_ptr<link> l, const knock_key& k,
                                           const decoy& d, accepted_as& as, int timeout_ms = kHandshakeMs);

// A ClientHello's JA4 (the fingerprint filters match: a_b_c), from the bytes
// of a TLS record that carries it. nullopt when it is not one. Pure; the
// tests check this build's against Chrome's.
[[nodiscard]] std::optional<std::string> ja4_of(std::span<const std::uint8_t> record);

}  // namespace mv::driftwire::tls
