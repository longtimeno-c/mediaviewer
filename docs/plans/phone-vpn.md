# Plan — Driftwire VPN in MediaViewer for NAS (iPhone)

Status: **plan, not built** (written 2026-11-14). Nothing here starts, and no entitlement is
requested, before the owner signs off on the calls in §9. The owner's ask: the iPhone app
"MediaViewer for NAS" gets Driftwire Tunnel's VPN mode, so the phone's traffic leaves through the
person's own Driftwire relay server.

What it builds on:

- **Driftwire Tunnel**, slices 2–5, on `main` since PR #669 (2026-11-13):
  [driftwire-menu-and-tunnel](driftwire-menu-and-tunnel.md) and
  [28 "The Tunnel"](../design/28-driftwire.md#the-tunnel-2026-11-12). The relay forwards TCP streams
  and datagram flows for key holders, there is an optional HTTPS look on 443, and Driftwire's own
  TCP/IP stack (`netstack.*`) and VPN runner (`vpn.*`) turn packets back into forwards.
- **The phone's reach to the NAS**, on `claude/mobile-integration-c6a3b5` (PR #663), not yet on
  `main`. That branch has `docs/plans/mobile.md` §7–8 and `docs/plans/nas-relay.md`: the phone
  races the NAS's local address and public name, falls back to the person's own relay 1.5 s later,
  and then upgrades to a punched direct path (§9 there). Paths below such as `src/mobile/link.cpp`
  and `src.ios/…` are on that branch.

Labels: **[code]** read in the repo at the cited place. **[Apple]** Apple's documentation or
guidelines, checked as noted in §10. **[spike]** unknown until S1 measures it. **[est]** my own
arithmetic.

## 0. The short version

- **Shape.** An iOS packet tunnel app extension (`NEPacketTunnelProvider`, an `.appex` in the app's
  `PlugIns`) runs the **whole** VPN: the device, `netstack`, `vpn::runner`, the dialer, BoringSSL
  and the sealed hop. iOS suspends the app, so the Mac's design does not carry over. On the Mac the
  extension is a thin pump that hands packets to the add-on over loopback. On iOS there is nothing
  awake to hand them to. The shared code moves unchanged behind one new `vpn::device` that wraps
  `NEPacketTunnelFlow`, plus a small C ABI for the extension.
- **The relay.** It is the same relay the phone already uses to reach the NAS. **But the phone does
  not hold that relay's key today.** By design, only the address is in the QR, and a phone joins
  the relay as a sender, which needs no key ([code] `nas-relay.md` §2, `src/mobile/link.cpp:98-106`).
  A forward needs the server key ([code] `splice.*` `kForwarder`, CPace on the server key). How the
  phone gets a forward credential is owner call 1. The recommendation is that the NAS hands the
  relay key, over the sealed session, to phones the owner marks as allowed (never guests).
- **Two problems that S1 must fix first** ([code], before any measuring):
  1. The TLS knock key is derived with Argon2id at libsodium's INTERACTIVE limit, which needs
     **64 MiB** (`tls_common.cpp:118-120`). That is more than the extension's whole memory ceiling.
     The app derives it once and the extension receives the 32-byte result.
  2. `vpn::runner::ticker` wakes **every 100 ms for as long as the VPN runs**, sending or not
     (`vpn.cpp:161-171`). On a phone that is battery spent on nothing. It must sleep when no
     segment is waiting to be resent.
- **IPv6 leaks on every platform today** ([code]). The Mac provider sets only `ipv4Settings`
  (`PacketTunnelProvider.swift:30-41`), and Windows routes only `0.0.0.0/1` and `128.0.0.0/1`.
  With the kill switch off, an app's IPv6 traffic goes around the tunnel. Most mobile networks hand
  out IPv6, so the phone must claim IPv6 too. The desktop fix is a follow-up of its own (§11).
- **Apple.** On iOS the packet-tunnel capability is one checkbox on both App IDs; it is not a
  request to Apple ([Apple], §4.1). App Review 5.4 applies in full. The App Store listing has to
  say what the VPN does. "Data Not Collected" stays true only if MediaViewer runs **no** relay that
  carries real traffic. **China mainland** (and any other storefront that requires a VPN licence)
  has to come off the storefront list, or the VPN has to go in a separate app (owner call 3).
- **When.** In a later update, after 1.0 is approved and not in the 1.0 or 1.1 submission (§4.4).
- **SAMPLE.** A reviewer cannot see a VPN without a relay. The recommendation is a **Tunnel demo
  inside the app**: a small relay on loopback, running inside the extension, that carries the
  reviewer's real traffic out of the phone's own connection, clearly labelled. No server run by
  the project, so nothing reverses "no public relay will exist" (owner call 5, §8).

## 1. The shape

### 1.1 Processes

```
MediaViewer for NAS (app)                        Driftwire VPN (packet tunnel .appex)
  Settings toggle, status, consent screen          NEPacketTunnelProvider (Swift, thin)
  NETunnelProviderManager: save, start, stop        ├─ flow_device: NEPacketTunnelFlow ⇄ vpn::device
  writes the tunnel item (Keychain group)           └─ C ABI mv_vpn_*  →  vpn::runner (shared C++)
  derives the knock key (Argon2id, 64 MiB)                                  netstack · tunnel::dialer
                                                                            tls (BoringSSL) · relay::forward
                                                                            → NWConnection-free BSD sockets
```

- **Why the stack runs in the extension.** On the Mac, `vpn_mac.mm` keeps the stack in the
  add-on and the provider sends packets to it over loopback after a token ([code]
  `mac_tunnel/README.md`, `vpn.h` `framed_device`). An iOS app is suspended within seconds of going
  to the background [Apple]. A VPN that stops when the person switches apps is not a VPN. So
  everything runs in the extension, which iOS keeps running while the tunnel is up.
- **What is shared, unchanged:** `netstack.*`, `vpn::runner`, `tunnel::dialer`, `tls_*`,
  `relay::forward`, `sealed_link`, CPace and Noise. Their header comments already describe them as
  portable behind ports (D9).
- **What is new, iOS only:**
  - `vpn::device` over `NEPacketTunnelFlow` (`flow_device`). Its Swift half reads
    `readPacketObjects` into a bounded ring and writes `writePacketObjects` in batches. The C++
    half's `read(…, timeout)` blocks on that ring, so `runner::pump` stays as it is.
  - A five-call C ABI in `src/mobile/vpn_abi.*`: `mv_vpn_start(config)`, `mv_vpn_packet_in`,
    `mv_vpn_set_packet_out(cb)`, `mv_vpn_state`, `mv_vpn_stop`. It uses POD structs and status
    codes, catches everything at the boundary, and never calls into Swift except through the
    packet-out callback, which the Swift side hands to `NEPacketTunnelFlow`'s own queue.
  - A static library target, `mv_driftwire_vpn`, holding only what the extension links: the files
    above plus `core`, libsodium, BLAKE3 and BoringSSL. **Not** the catalogue, search, ONNX Runtime
    or SQLite, which keeps the extension's binary and footprint small.
- **Sockets.** The dialer uses Driftwire's BSD-socket `net` layer as on the Mac. An extension's own
  sockets are not routed into its own tunnel [Apple: a provider's connections bypass its tunnel].
  S1 checks this with `includeAllNetworks` on.

### 1.2 The relay address and key

**Today** [code]: the pairing record, a JSON value in the Keychain (`Keychain.swift`, service-scoped
generic password, `AfterFirstUnlockThisDeviceOnly`, the app's default access group), holds
`relay: "host:port"` from the QR. The phone computes its waiter token from the access key and its
device key. It has **no** relay server key. `nas-relay.md` §2 says "The key never reaches a phone
or the QR".

The VPN needs a forward credential at the relay. The options (owner call 1):

| | How | For | Against |
|---|---|---|---|
| **A (recommended)** | The NAS gives it. `driftwire serve` already keeps the relay key in its secret store (`set_relay`, nas-relay §2). A new owner-granted op, `relay_forward`, returns it inside the sealed session, **only** to a phone the owner has marked "may use the relay as a VPN" on the desktop card. Guest and timed phones never get it. | No new relay protocol. No relay state on disk. The phone needs no new QR field, and an old QR keeps working. | Every allowed phone holds the relay's one key, so it can also register waiters, as the NAS does. Revoking a phone means rotating the relay key (re-entered on the server, the desktop and the NAS). Fine for one owner's own phones. |
| B | **Per-phone forward keys at the relay.** The relay accepts a list of forwarder keys (`--forward-keys <file>`), each one forward-only. The hello names the key by a keyed hash, as a sender names its token. | Revoking one phone touches nothing else. A forward key cannot register a waiter. | A relay protocol change and a file on the server the person must edit or the desktop must reach. The relay is "no state on disk" today. |
| C | **Leave through the NAS**, not the relay. The phone's existing session to `driftwire serve` gains a forward op, and traffic exits at home. | No relay key on any phone. Revocation is the device list's. | Not what was asked ("through the relay"). The service stops being "moves bytes and nothing else" (mobile.md §7.2). Every byte crosses the relay twice and the home uplink once. |

**Where the extension reads it.** The extension reads **one secret, the tunnel item**, plus
non-secret state in the App Group. It never sees the pairing record, the access key or the device
key:

- A Keychain item in a **shared access group**, `$(AppIdentifierPrefix)io.github.longtimeno-c.mediaviewer.mobile.vpn`,
  holding: the relay `host:port`, the forward key, the 32-byte **knock key** (derived in the app,
  §1.4), the TLS-look flag, and the SNI. It is stored `AfterFirstUnlockThisDeviceOnly`, so the
  extension can read it before the person unlocks after a reboot, and it is never backed up.
- **Careful with the default group.** Adding `keychain-access-groups` to the app makes the first
  listed group the default for new items [Apple]. List the app's own ID first, so the existing
  pairing and device-key items stay where they are and the extension cannot read them, **and** set
  `kSecAttrAccessGroup` to the `.vpn` group explicitly on the tunnel item. A unit test reads the pairing record with the
  extension's entitlements and expects `errSecItemNotFound`.
- **An App Group** (`group.io.github.longtimeno-c.mediaviewer.mobile`) for non-secret state only:
  - the NAS's addresses to exclude (§1.3), which the app writes before it starts the tunnel;
  - the extension's counters (streams open, flows, the last problem) for the Settings row;
  - the on-demand choice.
  Not the key.
- **App → extension while it runs:** `NETunnelProviderSession.sendProviderMessage` carries "the
  NAS's addresses changed" (a new public address, a punched peer). The extension re-reads the App
  Group and answers once its new settings are applied.
- `NETunnelProviderProtocol.providerConfiguration` carries only non-secret settings. It is stored
  by the system's VPN configuration and readable by the app [Apple]. `serverAddress` is the relay
  name, which iOS is expected to show in Settings → VPN (S3 checks).

### 1.3 Routes, DNS, IPv6

- **Interface:** `198.18.0.1/30`, with DNS at `198.18.0.2`, as in `vpn.h` `settings`. **Plus IPv6**
  (new, all hosts): an address in a fixed ULA `/64` of Driftwire's own and `NEIPv6Route.default()`
  as an included route. `netstack` already speaks IPv6 ([code] `netstack.h`). The relay connects
  out over whichever family the site's name resolves to.
- **DNS:** `NEDNSSettings(servers: ["198.18.0.2"])`, `matchDomains = [""]`. Queries become
  datagram flows to the relay's resolver (`kDnsHost`), so the network's DNS sees no name ([code]
  28). `--forward-udp-ports` defaults to 53, so QUIC falls back to TCP, as on the desktop.
- **Excluded routes**, set at start and re-set (`setTunnelNetworkSettings` again) when they
  change:
  - **The relay itself**, every resolved address, IPv4 and IPv6, including a NAT64-synthesised
    one. Resolved with `getaddrinfo` on the current path (mobile.md §7.3), never Driftwire's
    literal parser.
  - **The NAS's direct path. This exclusion is required, not optional.** The forwarder carries
    only ports 80 and 443, and only to public addresses ([code] `splice.h:57-61`). The NAS's
    service port inside the tunnel would be refused, so the library would stop working away from
    home.
    - Only the app's link knows these addresses: the public name's resolved addresses and the
      punch's candidates. The app writes them to the App Group before it starts the tunnel and
      sends `sendProviderMessage` when they change.
    - **Before a punch** (nas-relay §9), the app sends the NAS's candidates and waits for the
      extension's answer that they are excluded. Only then does it send SYNs, since SYNs sent
      earlier would enter the tunnel and be refused there. If the extension does not answer within
      1 s, the upgrade is skipped and the session stays on the relay.
    - The phone's relayed NAS connection already goes to the relay's address, which is excluded.
    - So the library keeps its own end-to-end Noise session and never goes through the
      forwarder.
  - **The local network:** the provider adds the Wi-Fi's own subnets, from `NWPathMonitor`'s
    interface addresses, as excluded routes. It re-sets them when the path changes, so the NAS at
    home and AirPlay keep working. This is needed from the start: `excludeLocalNetworks` (iOS 14.2+,
    true by default) takes effect only when `includeAllNetworks` or `enforceRoutes` is on [Apple].
    It is set too, for the kill-switch case.
- **Kill switch, "Block when the tunnel is down"** (off by default, as on the desktop):
  `includeAllNetworks = true` [Apple]. With it on, iOS holds traffic while the tunnel reconnects
  instead of letting it out. `excludeAPNs` and `excludeCellularServices` (iOS 16.4+, true by
  default) keep notifications, calls and SMS working, but **only in this mode** [Apple].
  - **A design risk, not a formality.** Apple documents that a provider's own connections bypass
    its tunnel only for the default case. Other VPN apps report the provider's own TCP failing
    silently under `includeAllNetworks`. It is also unverified whether excluded routes (the NAS's
    direct path) are honoured in this mode.
  - If S1 finds either one fails, iOS does not offer the switch. `block_unavailable()` then says
    why, the desktop's rule: "a kill switch that does not hold is worse than none"
    ([code] `vpn.h`).
- **MTU:** `netstack::kMtu` (1400), so it fits inside the TLS and Noise framing on a 1500-byte
  path.

### 1.4 Starting, on-demand, reconnects

- **The toggle** goes in Settings, in the section with **"Faster direct connection on mobile
  data"** ([code] `SettingsScreen.swift:11`):
  **Route all traffic through your relay** (off by default), with one line under it: "Every app's
  traffic leaves through your relay at ‹relay›. Your relay can see which sites you visit, not
  what you send over HTTPS." Under it are **Block when the tunnel is down** (off) and **Look like
  a web site (port 443)** (copied from the desktop's `tunnel_tls`).
  - The row shows for a library with a relay **and** a forward credential (call 1), or as the
    demo row in the sample library (§8).
  - The first switch-on shows the consent screen (§4.2) and then iOS's own "Add VPN
    Configurations" alert.
- **Saving:** `NETunnelProviderManager` with `localizedDescription` "MediaViewer — your relay"
  and the protocol above. The app derives the knock key once (Argon2id, 64 MiB, in the app's own
  budget) and writes the tunnel item before it saves.
- **On-demand** (owner call 4). The recommendation is **off in the first slice**. The person
  turns it on and off with the switch, or from iOS Settings → VPN. Later, an
  `NEOnDemandRuleConnect` for "any network", with an `NEOnDemandRuleDisconnect` for the home Wi-Fi
  (the network the NAS was found on) so the tunnel does not hairpin at home. Whether iOS restarts
  an on-demand tunnel by itself after a reboot or a provider crash is not stated by Apple. S3
  tests it.
- **Network changes.** A Wi-Fi ↔ cellular move breaks every forward to the relay, because their
  sockets are bound to the old path.
  - The provider watches `defaultPath` and sets `reasserting = true`. The dialer re-dials the relay
    on the new path. `runner` resets each local TCP connection whose forward died, so apps see a
    reset and retry, which they do on any network change.
  - Datagram flows reopen on their next datagram.
  - `sleep`/`wake`: nothing is held open while idle. A forward's own 10-minute idle timer at the
    relay ends quiet streams.
- **Reaching the relay on a filtering network:** the same as the desktop. The plain port, or 443
  with the HTTPS look. A network that allows only listed sites, or breaks TLS with its own
  certificate, defeats this as it defeats every tunnel ([code] 28 "What it exposes").

## 2. iOS limits

### 2.1 Memory: the extension's ceiling

Packet tunnel providers run under a memory limit that Apple calls undocumented. Apple's DTS
table on the developer forums gives **50 MiB from iOS 15** (15 MiB before). At least one iOS 17
report saw 15 MB again. S1 measures it on the devices we support; the plan does not rely on the
higher figure. Going over it is a jetsam kill, so the tunnel drops. What the shared code spends
today, at its desktop caps ([code], [est]):

| Piece | Desktop cap | Worst case per piece | At the cap |
|---|---|---|---|
| Argon2id knock key | once per start | 64 MiB | **over the ceiling alone**: moved to the app (§0) |
| `netstack` send buffer | `kSendBuffer` 256 KiB per connection, `kMaxConnections` 1024 | 256 KiB | 256 MiB |
| `netstack` receive window | `kRecvWindow` 64 KiB | 64 KiB | 64 MiB |
| Two threads per TCP forward (the worker and the splice's `back` thread, `vpn.cpp:217`, `splice.cpp:207-209`), two per UDP flow | `kMaxConnections`, `kMaxUdpFlows` 256 | stack touched (iOS reserves 512 KiB virtual per secondary thread [Apple]) | about 2,500 threads |
| BoringSSL per forward, with the HTTPS look on | one TLS connection per stream | SSL object plus record buffers, about 40–60 KiB [est] | about 60 MiB at 1024 |
| `sealed_link` frames | 16 KiB each way | 32 KiB | 32 MiB |

So the extension needs **an iOS profile** of the same code (`vpn::limits`, passed in, not
`#ifdef`s):

- about 96 TCP forwards (Safari opens about 6 per host) and 64 UDP flows;
- 64 KiB of send buffer, held back by the receive window when full;
- a worker pool instead of a thread per forward, if S1 shows thread count is what costs;
- the knock key handed in, never derived.

S1 measures this. Ship nothing until the extension's peak `phys_footprint` stays **under 60 % of
the measured ceiling** under the load in S1's verify.

### 2.2 Battery and idle

- **The 100 ms ticker** (§0) wakes the CPU ten times a second while nothing moves. It does three
  jobs: `stack::tick()` resends (go-back-N, 400 ms), `tick()` ends connections idle for `kIdleMs`
  (10 min), and `reap_locked()` joins finished workers ([code] `vpn.cpp:161-171`,
  `netstack.h:64`). Fix: wait until the earliest of the next retransmit deadline and the next idle
  deadline, and have a worker that finishes notify the ticker's condition variable so it is
  reaped at once.
- **No keep-alive to the relay of Driftwire's own.** But the phone's own apps keep long-lived
  connections, and with the kill switch off `excludeAPNs` does not apply [Apple]. So push and
  other persistent connections may sit in the tunnel, each holding a forward, two threads and its
  buffers. S1's idle measurement runs with a normal set of apps signed in, not a clean phone.
- **Measure:** S1 records CPU wakeups and energy in Instruments (Energy Log) with the tunnel on
  and the phone idle for 30 minutes, against the tunnel off. Gate: the difference is within run
  noise (runs alternated, as CLAUDE.md asks of every perf claim).
- **Throughput cost:** every byte is copied through `netstack` and sealed twice (TLS and Noise)
  in the extension. S1 measures a 100 MB download through the tunnel against direct, over the
  same Wi-Fi, alternated.

### 2.3 IPv6 and NAT64

App Review tests on an IPv6-only network with NAT64 [Apple]. In the tunnel:

- **The relay hop:** the relay name resolves through `getaddrinfo`, which synthesises an IPv6
  address on NAT64 networks. The excluded routes include that synthesised address.
- **Inside the tunnel:** the interface offers both families, so an app's IPv4 literal goes into
  the tunnel and leaves the relay over IPv4, even when the phone's own network is IPv6-only.
- **A relay given as an IPv4 literal** (`1.2.3.4:443`) is resolved through `getaddrinfo` too, so
  NAT64 synthesises its IPv6 address. The QR already prefers names (mobile.md §7.3).
- **Test:** the Mac's Internet Sharing "Create NAT64 Network" with the phone on it, the VPN on,
  Safari and an IPv4-only site.

## 3. The rules

### 3.1 D1–D9

- **D9 does not name iOS.** The phone app was planned as a **companion client, not a host of the
  viewer core** (mobile.md §3 call 2, recommended amendment). That amendment is **not yet in**
  `12-decision-log.md` (checked on `origin/claude/mobile-integration-c6a3b5`, 2026-11-14), so the
  VPN rests on a call that is not logged yet. This plan does not decide it. It needs only what D9
  already asks: the shared code (`netstack`, `vpn`, the dialer) stays free of Apple APIs, and
  `NetworkExtension` stays in the Swift provider and `flow_device`, just as `vpn_mac.mm` and
  `vpn_win.cpp` stay out of the shared files.
- **"Add-ons carry their own code"** (2026-10-11) **does not apply on iOS.** The iOS app has no
  add-on loader; it links Driftwire and AI statically (mobile.md §8.1). The extension is part of
  the app, just as the rest of the phone's Driftwire code is. The Mac's bend (12, "The Tunnel's VPN
  mode puts one thin piece in the app's bundle") was needed because macOS loads a system extension
  only from the installing app, while the stack stays in the add-on. On iOS the stack itself is in
  the extension, and that is not a bend of anything: there is no add-on to hold it.
- **No second app.** It is an app extension in the same app, with no app of its own.
- **D1–D8** are untouched: no canvas, no codec, no present path.

### 3.2 Rule 6: what leaves, and who sees it

Rule 6 is about the person's **files**. The VPN moves nothing of the library. Its traffic is the
phone's other apps, which leave anyway. What each party sees:

| Who | Sees | Does not see |
|---|---|---|
| The Wi-Fi or carrier | A TLS (or sealed) connection to the relay's address, its timing and size | Names (DNS goes through the relay), sites, content |
| **The relay (the person's own server)** | Every host name and address the phone visits, the DNS queries, timing and byte counts. **The content of plain `http://` and of unencrypted DNS-over-UDP**, because the relay is where those leave in the clear | The content of HTTPS and any other end-to-end encryption |
| The sites | The relay's address | The phone's network |
| MediaViewer's developers | **Nothing.** No server of the project's sits in the path, nothing is logged to us, and the iOS app has no crash upload | — |

- The relay already logs counts only, never a host ([code] 28 "Forwarding on the relay server").
  That stays the rule.
- The NAS's own traffic is excluded from the tunnel (§1.3), so the library's sealed Noise session
  never passes through the relay's forwarder.
- The consent screen (§4.2) says all of the above in plain words.

### 3.3 Licensing

- **BoringSSL** is Apache-2.0 ([code] `11-licensing.md` table; vcpkg's port declares
  `"license": "Apache-2.0"`), which is on `licence_check.py`'s permissive list. It must be added to
  `tools/ios/vcpkg.json`.
  - The port's `copyright` file passes the copyright-text scan's GPL regexes (checked by hand on
    the local vcpkg port, 2026-11-14). S2 runs the real check.
  - **`11-licensing.md`'s BoringSSL row says "the Driftwire engine and `mv-driftwire-relay` only,
    never the app or the core".** The iOS extension, and the app if the demo relay is linked into
    it, contradict that. S2 amends the row to name the iOS extension, and logs the change in 12.
  - `acknowledgements.py` must list it.
  - It cannot share an install with OpenSSL. Nothing on the iOS side pulls OpenSSL.
- **`netstack` is Driftwire's own** code (GPL-3.0-or-later, under the iOS app's additional
  permission, mobile.md §3 call 1). It is not lwIP (12, The Tunnel row), so it adds no third-party
  licence.
- **`--binary` check:** `licence_check.py --binary` must run on the `.app` **including the
  `.appex`**. Its Mach-O walk already covers every Mach-O in the bundle ([code] docstring). S2's
  verify says so.
- **The additional permission** for the App Store (mobile.md call 1) must cover the extension's
  code too. It covers "the iOS app", and the extension ships inside it, but the lawyer's text
  should name extensions.

## 4. Apple

### 4.1 Entitlements and what the owner clicks

On iOS, Network Extension packet tunnels are a capability any paid membership can enable. There is
no request form, unlike the content-filter and DNS-proxy cases of the past [Apple, §10]. The
Mac's system-extension entitlement on Developer ID is a separate question, already owed in 12.

In the **developer portal** (Certificates, Identifiers & Profiles), team 23FQ4A4Q35, Oakforge
Studios LTD:

1. **App ID `io.github.longtimeno-c.mediaviewer.mobile`** (explicit; exists): enable **Network
   Extensions**, **App Groups** (assign `group.io.github.longtimeno-c.mediaviewer.mobile`), and
   keep **Keychain Sharing** as an Xcode entitlement (`keychain-access-groups`; it needs no portal
   switch).
2. **New App ID `io.github.longtimeno-c.mediaviewer.mobile.vpn`** (explicit, description
   "MediaViewer VPN extension"): **Network Extensions**, **App Groups** (the same group).
3. **New App Group** `group.io.github.longtimeno-c.mediaviewer.mobile` if Xcode has not made it.
4. **Profiles:** with automatic signing (`CODE_SIGN_STYLE = Automatic`, [code]
   `MediaViewer.xcconfig`), Xcode regenerates both development and App Store profiles at the next
   archive. Nothing else to click. With manual signing, make an App Store profile for each App ID.
5. **App Store Connect:** nothing for the extension's App ID. It ships inside the app's record
   (6833437185).

Entitlements in the project (S2 adds them; listed so the owner sees them before clicking):

```
App:        com.apple.developer.networking.networkextension = [packet-tunnel-provider]
            com.apple.security.application-groups           = [group.io.github.longtimeno-c.mediaviewer.mobile]
            keychain-access-groups = [$(AppIdentifierPrefix)io.github.longtimeno-c.mediaviewer.mobile,
                                      $(AppIdentifierPrefix)io.github.longtimeno-c.mediaviewer.mobile.vpn]
Extension:  the same three (keychain group: the .vpn one only), NSExtensionPointIdentifier
            com.apple.networkextension.packet-tunnel, NSExtensionPrincipalClass the provider
```

### 4.2 App Review guideline 5.4 (VPN apps)

5.4 says, in summary ([Apple], checked in §10):

- **Apps offering VPN services must use the `NEVPNManager` API.** `NETunnelProviderManager` is
  its subclass, so this holds.
- **They may be offered only by developers enrolled as an organisation.** Oakforge Studios LTD is
  an organisation, so this holds.
- **A clear declaration of what user data is collected and how it is used, on an app screen,
  before the person uses the service.** This is the consent screen, shown before the first
  switch-on:

  > **Route all traffic through your relay**
  > Every app's traffic will leave through your own relay server at ‹relay›. Your relay can see
  > which sites you visit and when; it cannot see what you send over HTTPS. MediaViewer's
  > developers see nothing: we run no server, keep no logs and collect no data.
  > [Turn On]  [Not Now]

- **No selling, using or disclosing data to third parties**, committed to in the privacy policy.
  The privacy page (`packaging/site/iphone/privacy/`, on the appstore-listing branch) gains a "VPN"
  section that says so.
- **Local law; a licence where a territory requires one**, given in the review notes. China
  mainland requires one, and Apple removes VPN apps there without it (owner call 3).
- **Our own rule, not Apple's: not marketed as a way around restrictions.** 5.4 does not say
  this, but given 5.4's local-law clause it is the prudent line. The listing describes reaching
  the web through one's own server, privately. It never mentions getting past a school's, a
  country's or a workplace's filter, even though the desktop plan's "why" is a filtering network.
  The in-app words follow the same rule.

Elsewhere in the guidelines:

- **2.3 (accurate metadata).** The description and "What's New" must mention the VPN. A photo app
  that silently installs a VPN profile would be rejected, and rightly.
- **2.5.1 (public APIs only)** is met.

### 4.3 App Privacy answers and listing copy

- **App Privacy:** stays **"Data Not Collected"**, and it is true only on these conditions:
  - Apple counts as collected only data the developer or its partners can access [Apple]. The
    relay is the person's own server, so the developer accesses nothing (the same reasoning as
    phone backup, listing.md 1.1).
  - **If the project ever runs a relay that carries a reviewer's or anyone's real traffic, this
    answer becomes false** (browsing history at least). That is one more reason for §8's
    recommendation.
- **Listing (What's New and the description gain one paragraph):**

  > **Your own VPN.** Route all of your iPhone's traffic through the Driftwire relay you already
  > run for your NAS. Sites see your relay's address, not your network's; your network sees one
  > encrypted connection. Nothing goes to us: there is no MediaViewer server in the path.
  > Requires your own Driftwire relay server.

- **Review notes** gain how to see it (§8) and the line "This app's VPN connects only to a relay
  server the user runs themselves. The developer operates no VPN server and collects no data."
- **Export compliance:** the app already declares non-exempt encryption (mobile.md call 10). The
  extension adds BoringSSL (standard TLS) and no new category. Re-answer the questionnaire for the
  new build. France stays out until the declaration (call 10, unchanged).

### 4.4 The same app, and when

**Recommendation: ship it in the same app, as a later update (1.2 or after), and only after 1.0
is approved.** Not in the 1.0 submission, and not added to 1.1 before 1.0 is out. Why:

1. **1.0's review should be about the app.** A VPN brings in 5.4, its own consent screen,
   storefront changes and a reviewer who needs a relay. Any of these can bounce a first
   submission, and a rejection then holds up the library app that is otherwise ready.
2. **Storefronts.** China mainland has to come off before the VPN ships (call 3). Doing that in
   1.0 would drop a market for a feature most people will not use, and an update can make the
   change when it is the update that needs it.
3. **The unknowns are real.** The memory ceiling, the IPv6 change and the idle cost (§2) might
   change the design. They should be measured on TestFlight first, not under a review clock.
4. **The same app, not a separate one,** because the relay, its credential and the consent all
   come from the NAS pairing. A second app would need its own pairing, its own listing and its
   own 5.4 review for the same few people. A separate app is the fallback only if call 3 decides
   that China must keep the library app.

## 5. The slices

Each slice opens as a draft PR (the owner's standing rule) and gets an Opus review. The
present-loop gates (Windows PR 1, Mac PR 1) are untouched by phone-only code. Every slice that
changes shared code (`netstack`, `vpn`) re-runs them.

### S1 — Spike: the stack inside a packet tunnel extension (throwaway; device, TestFlight-free)

A minimal `.appex` with a development profile (Network Extensions on a **development** App ID is
the same checkbox; the owner enables it for this spike only after signing off this plan). It links
`mv_driftwire_vpn` and runs against a test relay on the developer's own Mac
(`mv-driftwire-relay` on the LAN, never public).

- Fix the two blockers first, in shared code, with tests:
  - the knock key handed in (`tunnel::dialer::options` takes a `knock_key`, and derives only when
    it is absent);
  - the ticker sleeping to the next deadline (`[driftwire-vpn]` test: an idle runner wakes no more
    than once per retransmit deadline).
- Measure on one device (named in the report), runs alternated:
  - peak `phys_footprint` under (a) Safari loading 10 heavy pages at once, (b) a 100 MB download,
    (c) 64 parallel `URLSession` tasks;
  - each with the HTTPS look on and off, and with the iOS limits of §2.1;
  - the ceiling itself, by growing an allocation in the extension until jetsam;
  - 30 minutes idle: energy and wakeups, tunnel on against off;
  - throughput against direct.
- Check the [Apple] behaviours:
  - the extension's own sockets bypass the tunnel with `includeAllNetworks`;
  - `excludeLocalNetworks` with and without `includeAllNetworks` (the NAS's local address
    reachable);
  - reconnect after Wi-Fi → cellular;
  - NAT64 (Internet Sharing);
  - excluded routes honoured with `includeAllNetworks` on;
  - the demo relay (§8) in the same extension: its footprint under (a).

**Verify (S1):** a written report in `docs/plans/phone-vpn.md` §2 with the numbers, quoting the
JSON or the Instruments export. Peak footprint under 60 % of the measured ceiling in (a)–(c)
with the HTTPS look on. Idle energy tunnel-on within the noise of tunnel-off. Every [Apple] row
above answered. If the footprint gate fails with a worker pool and the smaller limits, **stop**
and bring call 6 (multiplexing at the relay) to the owner.

### S2 — The credential, the item, the extension (iOS and `driftwire serve`)

**Prerequisite:** the phone branch must contain `main`'s Tunnel. Today
`claude/mobile-integration-c6a3b5` has no `netstack`, `vpn` or BoringSSL. Either PR #663 lands in
`main` first, or `main` is merged into it, following the merge rules for stacked branches. S1 can
run on a throwaway branch that has both.

- Call 1's mechanism: under A, `relay_forward` under an owner-granted op bit, negotiated, so an
  old service never sees it; the desktop card's "may use the relay as a VPN" per phone.
- The tunnel Keychain item, the App Group and `vpn_abi.*`.
- The extension target in `MediaViewerMobile.xcodeproj`, with the entitlements of §4.1.
- `tools/ios/vcpkg.json` + BoringSSL; `licence_check.py` and `acknowledgements.py` run over the
  `.app` with the `.appex`.

**Verify (S2):**

- `[phone-relay]` tests on a loopback relay: an allowed phone gets the credential, and a guest,
  a timed phone and a revoked one do not.
- The extension's entitlements cannot read the pairing record (`errSecItemNotFound`).
- `licence_check.py --binary MediaViewer.app` passes with the `.appex` inside.
- The device build archives with automatic signing.

### S3 — The tunnel, routes and IPv6 (shared code and iOS)

- `flow_device`; the provider's settings (§1.3) with IPv6.
- Excluded routes for the relay, the NAS's direct path and the local network.
- The kill switch.
- Reconnect on path change, and the reset of dead forwards.
- The iOS limits profile.

**Verify (S3)** on a device, with the tunnel on:

- Safari, a `URLSession` app and a UDP DNS query all leave through the relay. The relay's counts
  rise, and no host appears in its log.
- An IPv6-capable site reached over IPv6 also goes through the tunnel (the relay's count rises;
  a "what is my IP" page shows the relay's address for both families).
- The NAS is still reached directly at home and on its punched path away (the library's path
  picture says "direct", not "relayed", and the relay's forward count does not move).
- Wi-Fi → cellular reconnects within 5 s.
- With the kill switch on and the relay stopped, nothing leaves. Without it, traffic goes direct.
- The NAT64 test of §2.3 passes.
- S1's memory and idle gates hold.

### S4 — Settings, consent, listing (iOS)

- The toggle and its rows.
- The consent screen.
- Status in Settings (connected, reconnecting, the relay's problem in the window's words).
- On-demand, if call 4 says so.
- The demo (call 5).
- The privacy page section, the listing paragraph and the review notes.

**Verify (S4):**

- UI tests in `MobileFixtures`: the row is hidden with no relay or no credential, and the consent
  screen comes before the system alert.
- A reviewer path from a fresh install, following only the review notes, reaches a working tunnel
  (§8).
- `small-screen.sh` passes with the new rows.

### S5 — The IPv6 fix on the desktop halves (Mac, Windows; may run beside S3)

The Mac provider gains `ipv6Settings` and Windows gains `::/1` and `8000::/1` through Wintun, with
the relay's own IPv6 route kept. This is a follow-up to PR #669, not phone work. It is listed here
because S3 changes `vpn::settings`, which both halves read.

**Verify (S5):** on each OS with the kill switch off, an IPv6 site goes through the relay. Both
PR 1 present-loop gates hold.

## 6. Tests

- Loopback only, as all relay tests: an in-process relay on `127.0.0.1`/`::1`, never a public one.
- Filter by tag (`[driftwire-vpn]`, `[phone-relay]`, `[phone-vpn]`), never by name (memory:
  name filters run hidden live tests).
- The extension's C++ runs under the same `mv_tests` on macOS, with a `vpn::device` stand-in. Only
  `flow_device` and the provider need a device.

## 7. Risks

- **The footprint gate fails** even with a smaller profile. Then a stream per forward is the
  problem, and the fix is multiplexing many forwards over one sealed connection to the relay. That
  is a relay protocol change (call 6), and it would help the desktop too.
- **Review asks about 5.4 anyway, for a photo app with a VPN.** The consent screen, the listing
  and the review notes are written for that. If review asks for a VPN licence in a storefront,
  the answer is call 3.
- **The person's relay is on their home line** (often a NAS). The tunnel's speed is the home
  upload speed. The Settings line says "through your relay at home", and S1's throughput number
  goes in the docs, not marketing.
- **iOS updates change NetworkExtension behaviour** (it has before). The S3 verify becomes a
  checklist run on each new iOS major version.

## 8. SAMPLE: how a reviewer sees the VPN

The owner requires every new phone feature to be demoable in the SAMPLE library (2026-11-12). The
sample is a static, signed library read from a GitHub release. It has no NAS, no relay and no
key ([code] sample-library notes: `static_remote`, "the sample's too (which never relays)"). A VPN
needs a relay. The options (owner call 5):

| | What the reviewer gets | Cost |
|---|---|---|
| **A — a Tunnel demo inside the app (recommended)** | With the sample open, the VPN row reads **Try the tunnel (demo)**. It starts the real extension and the real `netstack`, dialer and sealed hop, against a **relay running inside the extension on loopback**. That relay forwards out of the phone's own connection. Safari works through it, iOS shows the VPN badge, Settings shows streams and flows, and every screen says **Demo: your traffic leaves from this phone, not through a relay**. | No server of the project's, so "no public relay will exist" stands and "Data Not Collected" stays true. It exercises everything except the network hop to a remote relay. It must never look like a privacy feature: the banner is permanent while the demo runs. The relay's forwarder code (`splice.*`) is linked into the extension and runs only in demo mode. **A relay server and its client in one extension roughly double the memory under a ceiling that is itself uncertain (§2.1)**, so S1 measures demo mode too. If it does not fit, the demo's relay keeps the client's limits halved. |
| B — a demo relay the project runs, for reviewers only | A real remote relay, its key in the review notes | **Reverses "no public relay will exist"** in practice: anyone who reads the notes, or extracts the key from a build that carries it, has a free VPN through the project's server. **"Data Not Collected" becomes false** while it carries anyone's traffic. A server to run and secure. Not recommended. |
| C — a screen recording in the review notes, with no demo | A video | Apple accepts videos for hardware-dependent features, but the owner's rule asks for a demo in SAMPLE. It also breaks the moment the UI changes. A fallback only. |

Under A, the demo is clearly a demo:

- the toggle's label says "demo";
- the consent screen says "your traffic will leave from this phone, as without a VPN; this shows
  how the tunnel works";
- the `localizedDescription` iOS shows is "MediaViewer — demo (no relay)";
- the demo turns itself off when the sample is closed.

## 9. Owner calls

| # | Call | Recommendation |
|---|---|---|
| 1 | **How a phone gets a forward credential** at the relay (§1.2). Today the phone has none, by design. | **A**: the NAS hands the relay key, inside the sealed session, to phones the owner marks on the desktop card, never to guests or timed phones. Revoking such a phone means rotating the relay key; the desktop card says so. Move to B (per-phone forward keys at the relay) if more than the owner's own phones ever need it. |
| 2 | **The VPN as a feature of this app at all**, with 5.4's consent screen and the listing paragraph (§4.2–4.3). | Yes, in the same app (§4.4 point 4). |
| 3 | **Storefronts:** VPN apps need a licence in China mainland, and Apple removes them there without one. | Remove China mainland from availability in the update that adds the VPN. Check the territories that require a VPN licence or have had VPN apps removed (Russia removed many in 2024) at that time; Apple publishes no list. If keeping China matters more, ship the VPN as a separate app instead (not recommended: a second pairing and listing). |
| 4 | **On-demand** (connect automatically). | Off in the first release. Add "any network, except home Wi-Fi" once S3 holds. |
| 5 | **SAMPLE: how a reviewer sees the VPN** (§8). | **A, the labelled in-app demo**: no project relay, so the 2026-11-12 "no public relay will exist" call and "Data Not Collected" both stand. |
| 6 | **If S1's memory gate fails:** multiplex forwards over one connection to the relay (a relay protocol change, shared with the desktop). | Decide only if S1 fails; S1 tries the smaller profile and a worker pool first. |
| 7 | **When** (§4.4). | A later update, after 1.0 is approved. Spikes and S2–S4 may run on TestFlight before then. |
| 8 | **The extension's App ID and group names** (§4.1). | `io.github.longtimeno-c.mediaviewer.mobile.vpn` and `group.io.github.longtimeno-c.mediaviewer.mobile`. They cannot be renamed after release without losing the saved VPN configuration. |

Not owner calls, but owed elsewhere and named so they are not lost:

- the D9 amendment for iOS as a companion (mobile.md call 2, not yet logged);
- the App Store additional permission naming extensions (call 1 there);
- the desktop IPv6 leak (S5).

## 10. How the claims were checked

- **[code]:** read on `origin/main` at `928b2e9d` (Tunnel, `netstack`, `vpn`, `tls_common.cpp`, the
  Mac provider) and on `origin/claude/mobile-integration-c6a3b5` (the phone's `Keychain.swift`,
  `link.cpp`, `SettingsScreen.swift`, `MediaViewer.xcconfig`, `licence_check.py`, `vcpkg.json`,
  `nas-relay.md`, `mobile.md`) on 2026-11-14. vcpkg's `boringssl` port declares Apache-2.0
  (`~/Dev/.mv-toolchain/vcpkg`).
- **[Apple]:** App Review Guidelines 5.4, 2.3, 2.5.1; the NetworkExtension reference
  (`NEPacketTunnelProvider`, `NETunnelProviderManager`, `NEVPNProtocol.includeAllNetworks`,
  `excludeLocalNetworks`, `excludeAPNs`, `excludeCellularServices`, `NEOnDemandRule`); Apple's
  "Network Extensions" capability page and the "App privacy details" page. The extension's
  memory ceiling is **not** published by Apple. S1 measures it.

## 11. Follow-ups outside this plan

- The desktop IPv6 leak (S5) affects the shipped Mac and Windows halves today, whatever happens to
  the phone.
