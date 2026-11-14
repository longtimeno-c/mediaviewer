# Spike: the Tunnel looking like HTTPS (slice 4)

2026-11-12, for [driftwire-menu-and-tunnel](driftwire-menu-and-tunnel.md) slice 4 ("its own
spike first"). The question: which TLS library, under
[11-licensing](../design/11-licensing.md), and can its ClientHello pass for a browser's?

## What has to be true

- The connection to the relay is TLS 1.3 on TCP 443, and its ClientHello looks like Chrome's to a
  filter that fingerprints it (JA3 / JA4). Chrome has permuted its extensions since 110, so JA3
  is noise for Chrome itself; JA4 (sorted ciphers, sorted extensions, the signature algorithms,
  ALPN) is what is matched now.
- The sealed hop (CPace, then Noise) runs inside unchanged. TLS adds camouflage, not security:
  the client does not check the certificate (the hop proves the server key).
- A connection that does not prove it holds the key is answered as an ordinary web site, so an
  active prober sees a web server, not a Driftwire relay.

## Libraries looked at

| Library | Licence (vcpkg port) | With GPL-3.0-or-later | Chrome's ClientHello |
|---|---|---|---|
| **BoringSSL** | Apache-2.0 | Compatible (as Crashpad, 11) | **Yes**: it is Chrome's own TLS. GREASE (`SSL_CTX_set_grease_enabled`), extension permutation (`SSL_CTX_set_permute_extensions`), ECH GREASE (`SSL_set_enable_ech_grease`), ALPS (`SSL_add_application_settings`, the new codepoint), certificate compression offers, X25519MLKEM768, Chrome's cipher and signature-algorithm order |
| OpenSSL 3.6 | Apache-2.0 | Compatible | No GREASE, no extension permutation, no ALPS, no ECH GREASE: the JA4 extension count and hash cannot match |
| mbedTLS 3.6 | Apache-2.0 OR GPL-2.0+ | Compatible | Less still (no hybrid ML-KEM group, a fixed extension set) |
| wolfSSL | GPL-3.0 (or commercial) | Compatible | Not a browser's; a uTLS-like layer would be ours to keep |

uTLS (Go) is out: not C++. A hand-written ClientHello in front of a library is out too: the
handshake transcript must match what was sent.

## Decision

**BoringSSL**, from the pinned vcpkg baseline (2026-08-13, `0.20260813.0`; built by vcpkg with
Perl, NASM and Go as build tools only). It cannot share an install with OpenSSL; nothing in
MediaViewer's tree pulls OpenSSL today (checked `vcpkg_installed`). Linkage: **static**, in the
Driftwire engine and `mv-driftwire-relay` only (not the app or the core); Apache-2.0 needs its
notice carried, as Crashpad's. Its `bssl` tool is not shipped.

## What gets built

- `tls.h` (portable) and `tls_boringssl.cpp`: a TLS link over a link (memory BIOs, so the existing
  link stays the transport; one reader and one writer at a time, as a splice runs it).
- The client profile is Chrome's: TLS 1.2–1.3, GREASE, permuted extensions, ECH GREASE, ALPN
  `h2`, `http/1.1`, ALPS for `h2`, brotli certificate compression offered, X25519MLKEM768 then
  X25519, P-256, P-384, Chrome's TLS 1.2 cipher list and signature algorithms, OCSP stapling and
  SCTs asked for, SNI when the relay is a name (none for an address, as a browser does).
- The server (`mv-driftwire-relay --tls-port 443`) presents a certificate: `--tls-cert` and
  `--tls-key` (a real one, such as Let's Encrypt's, is what passes a prober's look), else a
  self-signed one made at start. It picks `http/1.1`.
- **The knock.** Inside TLS the client's first 32 bytes are a keyed BLAKE2b of the session's TLS
  exporter (`EXPORTER-driftwire-knock`), keyed by an Argon2id hash of the server key (made once
  per start on each side). A knock is good for one TLS session only, so recorded bytes say
  nothing again. With a good knock, the hop runs as on the plain port (all roles). With anything
  else, the bytes read so far are the start of an HTTP request, and the server answers as a web
  site (`--decoy-dir` for its pages, else a small built-in one; `Server: nginx`). A network that
  breaks TLS with its own certificate sees the knock, but Argon2id makes offline guesses at a
  typed key slow, and a made key (130 bits) is out of reach either way.
- The Tunnel gets **Look like a web site (port 443)**, off by default (`tunnel_tls`, the prefs'
  `tunnel_tls_port`): its forwards go to the relay's TLS port. Codes keep the relay's plain port
  (a sender has no server key to knock with).

## ProVerif

`relay.pv` does not change: TLS here makes no security claim, the hop inside is the same bytes,
and the knock is an access gate in front of it (its only property, that it does not open without
the key or again on another session, is the keyed hash over a per-session exporter). Said in
tools/formal/README.md.

## Checked in tests

The JA4 of the ClientHello, computed from the bytes on the wire: part a `t13d1516h2` and part b
(the cipher hash) Chrome's `8daaf6152771`. The extension hash moves with Chrome's own ECH and ALPS
codepoints from release to release, so it is not pinned.
