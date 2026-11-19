# The VPN host, harder to spot

The self-hosted VPN (`driftwire host vpn on`, or a NAS's `set_vpn`) is meant to get a laptop or phone
through networks that block sites. A filter that wants to block *it* has four easy tells today, and a
few harder ones. This plan says what each costs to remove and what stays visible anyway.

How it worked before this plan (2026-11-19, on `claude/vpn-hosts-on-main`):

- The VPN code is `mediaviewer://relay#name.duckdns.org:47614#KEY`: the relay's **plain** port. The
  plain hop starts with `MVRL`, a version and a role **in the clear**: one byte pattern identifies it.
- The web-site look (`tls.*`: Chrome's ClientHello, the knock, an nginx-like decoy) exists but is
  opt-in on the host (`vpn on --tls-port N`), a separate switch on the laptop (**Look like a web
  site**), and the code does not say it is there. Its certificate is self-signed for the name.
- The router keeper already maps the look's port to outside 443, falling back to the inside port
  when the router's 443 is someone else's (`host_keepers.cpp`).

**Owner calls (2026-11-19):** Let's Encrypt by DNS-01, yes; the look on 443 by default with the code
carrying it, yes; names: **both** DuckDNS and the person's own domain (the CNAME route below);
first-flight padding, yes.

## Recommendations

### 1. Self-signed certificate → Let's Encrypt by DNS-01 on DuckDNS (host)

A prober that connects to the home address sees a self-signed certificate: rare for a real site.

- **Build an ACME client** (RFC 8555) in the add-on: account key and CSR with BoringSSL (already
  linked for `tls.*`), JWS ES256, `newAccount` → `newOrder` → DNS-01 → `finalize` → the chain.
  The TXT value goes to DuckDNS with the same one GET as the address
  (`update?domains=SUB&token=…&txt=VALUE`), and is cleared after.
- **The certificate keeper**, beside the DuckDNS and router keepers in `driftwire host`: at start and
  daily; renews 30 days before expiry; on failure 1 h, 2 h … 24 h apart, never more than 3 orders a
  day (Let's Encrypt allows 5 failed validations an hour and 5 duplicate certificates a week per
  name; `duckdns.org` is on the public suffix list, so each name has its own 50-a-week budget).
- **Fallback:** the self-signed certificate until a real one is in hand, and the last good one while
  renewal fails. The relay takes a new certificate without a restart (new connections only).
- Files in the host's folder (`tls/`, mode 600). The account key never leaves it.
- HTTPS to Let's Encrypt through the OS's own client (NSURLSession, WinHTTP, libcurl), which
  `cli::https_get` already wraps: it gains POST, a request header and response headers.
- **Tests on loopback:** a stand-in ACME server that checks every JWS signature and nonce, a stand-in
  DuckDNS with TXT, the keeper's schedule with made-up times. Never Let's Encrypt or duckdns.org.
- **NAS:** a Synology already has DSM's own Let's Encrypt; `serve` takes `--tls-cert/--tls-key`
  paths (the relay already does). No ACME client on the NAS in this slice.

### 2. Port 47614 → the look on 443, by default, and the code says so

- **`host vpn on` turns the look on** (inside port: the relay's port + 1, any user may listen on it,
  so Linux needs no privilege and a host that already serves 443 keeps it). The router maps outside
  **443** to it, as now; when the router's 443 is someone else's, the fallback is outside **8443**
  (still an HTTPS-looking port), else the inside port. `--tls-port off` is the old plain VPN.
- **With the look on, the plain relay port is not mapped outside** for the VPN: the `MVRL` pattern is
  never on the internet. (The plain port stays on the LAN.)
- **The code carries the look:** `mediaviewer://relay-tls#name:443#KEY`. Connect sets the relay field
  to `name:443#KEY`, Look like a web site on and its port 443, in one step, on both chromes and the
  phone (one parser, `serve::parse_relay_connect_code`, returns the port and whether it is TLS). The
  old `mediaviewer://relay#` form still parses as the plain relay.
- **The NAS** (`set_vpn`): the same look on its port + 1, self-signed or DSM's certificate; DSM's
  router page is asked for 443 in the SPK's service file.

### 3. DuckDNS in the name → your own domain, by CNAME

`*.duckdns.org` is on some blocklists, and it is in the SNI of every connection.

- **`driftwire host name domain vpn.example.com`**: the person adds two CNAMEs at their registrar,
  `vpn.example.com → SUB.duckdns.org` and `_acme-challenge.vpn.example.com →
  _acme-challenge.SUB.duckdns.org`. DuckDNS still keeps the address; the code, the SNI and the
  certificate carry the person's own name, and DNS-01 follows the CNAME, so no registrar API.
- **A custom DDNS update URL:** not now. Every registrar's API differs; the CNAME needs none.
- **ECH: no.** ECH hides the inner name behind a public *outer* name shared by many sites (a CDN).
  A single home address has no crowd to hide in: the outer name would be this host's own name, or a
  name that does not belong to this address, which is itself a tell. BoringSSL could do it later if
  the host ever sits behind a CDN.

### 4. Fingerprints

- **ClientHello:** already Chrome's (BoringSSL with Chrome's settings, extension order permuted,
  GREASE, ECH GREASE, X25519MLKEM768; JA4 checked in tests). Keep the test's JA4 in step with Chrome.
- **The decoy, under active probes**, two concrete fixes:
  - A short request (`GET / HTTP/1.0` + blank line is 18 bytes, under the 32-byte knock) waits 10 s
    for the knock today; nginx answers at once. Answer as soon as a request head is complete.
  - Error pages and headers byte-for-byte as nginx's (its `<center>nginx</center>` pages, a `Date`
    header), so a prober comparing answers with a real nginx sees one.
- **Server fingerprint (JA4S/JARM):** BoringSSL's ServerHello is not OpenSSL's, so a prober that
  compares the TLS server with the `Server: nginx` header can tell. Not fixable without OpenSSL;
  listed below.

### 5. Traffic shape

- **First-flight padding: yes, small.** Every connection's first records after the handshake have
  the same sizes (knock, then the relay's CPace and Noise messages): a classifier's easiest feature
  for tunnels inside TLS. Both sides send 0–255 random bytes (with a length) right after the knock,
  in the TLS layer only. A new exporter label for the padded knock; the relay accepts both, so an
  older laptop still connects; a new laptop needs an updated relay (the code's `relay-tls` form is
  new, so the VPN code path never meets an old relay).
- **Connection reuse: no.** Each forward is its own TLS connection, as a browser's are; multiplexing
  would make a few very long-lived connections, which is the tell the owner named, and costs
  head-of-line blocking.
- **Constant-rate padding: no.** It costs the home's upload, which is the VPN's speed.

## What stays detectable (honest list)

1. **Volume and timing to one residential address.** A laptop that sends everything to one home IP,
   all day, from a café's network, is visible to anyone who looks at flows. Nothing here hides that.
2. **The address is residential.** Filters that score residential or dynamic ranges see it; a VPS
   relay (`mv-driftwire-relay`) moves the exit but not the pattern.
3. **The DNS lookup of the name**, unless the system uses encrypted DNS. With your own domain the
   CNAME to `duckdns.org` shows in that lookup.
4. **The certificate's names are public** in Certificate Transparency logs once Let's Encrypt
   issues one: anyone can list `SUB.duckdns.org` or your domain and probe it. The decoy is what they
   find.
5. **The TLS server fingerprint** (BoringSSL, not nginx's OpenSSL) under a careful active prober.
6. **Many TLS connections at once to one small site** while the VPN carries all traffic.
7. **The knock itself** is unforgeable without the key, but a network that intercepts TLS with its
   own root (a managed device) sees the knock and the relay's handshake inside; it still cannot read
   or test the key quickly (Argon2id).
8. **Self-signed until issuance works**, and for good when Let's Encrypt cannot be reached, without a
   DuckDNS name, or on a NAS.
9. **A NAS's look on a non-standard port** (47615) unless its owner maps outside 443 to it.
10. **The plain relay port stays open on the home network**, and outside too with `--tls-port off`
    (or when the look cannot listen): its `MVRL` head is a fixed pattern.

## Built (2026-11-19, one PR stacked on the #711 port)

All five, as recommended, with these differences:

- **The NAS** gets the look on its VPN port + 1 by default and the `relay-tls` code; it stays
  self-signed (no ACME client in `serve`, and no `--tls-cert` for `serve` yet: DSM's own certificate
  could be passed later). Outside 443 for the NAS is the person's to map on the router (the SPK's
  service file names the port).
- **The look's inside port** is the relay's + 1 (47615), not 443, on every OS.
- **Own domain**: `host name domain NAME|none` and the card's **Your own domain** row; DuckDNS still
  keeps the address. A custom DDNS update URL was not built.
- Design: `docs/design/28-driftwire.md` ("The VPN exit", "The NAS as a VPN host", "Connect to a VPN
  host", "The Tunnel").

Not done here (follow-ups):

- `serve --tls-cert/--tls-key` for a NAS with its own certificate.
- A custom DDNS update URL (a provider other than DuckDNS keeping the address).
- Not run against the real Let's Encrypt or duckdns.org (by design, never in tests); Windows and
  Linux `https_request` are compiled by CI, not run.
- A laptop holding a plain code from before (`name:47614#KEY`) stops reaching the host from outside
  once the look is on (its port is no longer mapped), and a NAS owner who forwarded only 47614 must
  forward 47615 too; nothing tells them to copy the code again. Harmless while the VPN hosts are
  unreleased (#711's port, #730); a note on the card would be the fix if that changes.
- No test drives an older client's plain knock against this build's relay (the old client is not in
  the tree); the server's code path for it is the one that was there before.
