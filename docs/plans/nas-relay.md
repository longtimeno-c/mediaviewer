# Plan — Reaching the NAS away from home without a forwarded port

Piece 3 of the phone/NAS work (2026-11-12). Today the phone reaches `driftwire serve` at its local
address at home, or at a public name and forwarded port away from home ([mobile.md](mobile.md) §7.3).
With no forward (a router that cannot, carrier-grade NAT, a person who will not), the phone works only at
home. This plan adds a fallback through a relay, using Driftwire's own (`docs/design/28` "Relays"),
keeping the phone↔NAS handshake (Noise XX+psk0 with the access key, the device-key check) end to end so
the relay carries ciphertext only. The owner's calls are in §6.

## 1. What is reused

- **The relay server** (`tools/driftwire-relay`, `splice.*`): a rendezvous that pairs a *waiter* with a
  *sender* under a 16-byte token and then copies bytes. A waiter proves the server's key (CPace); a
  sender only names the token's id. Every hop to the server is sealed (Noise NNpsk0); the server learns
  two addresses, the time and the byte count. Caps: 64 waiters, 4 per source (an IPv4 /24 or IPv6 /64),
  8 splices, an optional rate per splice, 120 s idle.
- **The service and its client unchanged above the link**: `serve::client::open` takes any `link`; a
  spliced connection is one. The service's admission (limits, the access-key handshake, the device list)
  applies to it exactly as to a direct one.

Not reused: the DHT meeting and hole punching. Phones on mobile data are mostly behind symmetric NAT,
where punching fails, and a NAS on the public DHT would announce itself to anyone watching. A relay works
in every case a phone can reach the internet; punching would be a later optimisation (§7).

## 2. How it works

**The NAS waits at the relay.** With a relay configured, `driftwire serve` keeps one waiter registered per
paired device (at most 4, the server's per-source cap; a fifth device waits its turn), re-registering
every 60 s as the relay protocol already does. Each waiter's token is

    token(device) = BLAKE2b-128(key = access_key, "MediaViewer relay token v1" || device public key)

so the relay sees an opaque token per phone, unrelated across libraries, and a stranger without the
access key cannot name one. When a sender pairs with a waiter, the service runs its normal admission on
the spliced link (it is the same `serve::server`, fed a link instead of an accepted socket) and registers
a fresh waiter for that device.

**The phone tries it last.** The phone already races its local address and public name (`mobile.md`
§7.3). With a relay in its pairing, it adds the relay 1.5 s after the direct attempts start (Driftwire's
own race delay), so a working direct path always wins and the relay costs nothing at home. It computes
its own token from the access key and its device key: the QR carries only the relay's address.

**Pairing data, compatible both ways.** The QR stays `"v":1` and gains an optional
`"relay":{"address":"host:port"}`. An old app ignores the field (its parser reads only what it knows)
and keeps reaching the NAS as before. A new app with an old QR has no relay and behaves as today. The
service announces `"relay"` in its capabilities (`caps_msg`, already negotiated) so the card shows the
row only for a service that can wait at a relay; an old service never sees a relay request.

**The desktop sets it up.** Settings → Phone access gains **Relay (your own server)**: the address and
the server's key, as Driftwire's own field takes them. The desktop sends both to the service over the
owner's session (`serve_admin`, a new `set_relay` verb, owner only); the service keeps the key in its own
secret store. The key never reaches a phone or the QR.

## 3. Threat model

| Who | Can | Cannot |
|---|---|---|
| **The relay's operator** | See the phone's and the NAS's addresses, when they talk and how many bytes, and the opaque token per phone (stable for that phone until it re-pairs). Drop or delay the link. | Read or change anything (the Noise session is end to end). Learn the library, file names, or the access key. Pair a phone, or reach the NAS without the access key (it is a sender like any other: admission still applies). |
| **Someone on the path to the relay** | See that a MediaViewer talks to the relay (sealed hops, fixed shapes). | Learn the token (sealed in the hop), replay a hop, or reach the NAS. |
| **A stranger who knows the relay's address** | Connect as a sender and guess token ids (16 bytes: no). Fill the relay's waiters with their own (only with the server's key). | Reach the NAS: no token, and even with one, no access key. |
| **A revoked phone** | Still compute its token and reach the NAS through the relay, as it can reach a forwarded port today: admission refuses it after the handshake (`docs/design/28` "Revocation removes a device, not the access key"). The service drops a revoked device's waiter, so it is not paired at the relay at all. | Open a session. |

The relay adds no secret the NAS did not already guard, and the NAS stays silent to anyone without the
access key. What it adds is the operator's view of addresses and timing (§6 call 1).

## 4. Rate limits and cost

- **At the relay:** the existing caps; the operator's own rate per splice (owner's choice, §6 call 3).
  Four waiters per NAS means four phones away from home at once.
- **At the NAS:** a relayed connection counts against the same admission limits as a direct one, keyed by
  the relay's address. Because every relayed phone shares that address, the per-address limits would cap
  all of them together: relayed connections get their own table keyed by token, with the same numbers.
- **Bytes:** relaying doubles the operator's traffic for every byte (in and out). A catalogue sync is
  tens of MB; thumbnails a few KB each; originals and video are the cost.

## 5. On the phone: battery and data

- No keep-alive and no background connection: the phone connects through the relay only when it would
  have connected directly (opening the app, a sync, a picture), and only after the direct race failed.
- The relay path is marked on the connection (`ConnectionSnapshot`), and **originals and video over the
  relay follow the same mobile-data rule as today** (asked, not automatic). Catalogue sync over a relay on
  mobile data stays off unless the person turned it on, as now.
- Cost on the NAS: one TCP connection per waiter, a few bytes each minute.

## 6. Owner calls (answered 2026-11-12)

1. **No public relay will exist** (owner, 2026-11-12). Only a relay the person runs themselves
   (`driftwire-relay` on their own server) is supported. The card and the docs say plainly that reaching
   the NAS away from home without a forwarded port needs one's own relay.
2. **Everything goes over it**, under today's mobile-data rules (§5).
3. **No rate cap** on one's own relay.
4. **Hole punching: yes, after the relay.** Built second, as an extra that skips the relay where both
   NATs allow it, falling back to the relay or the direct address.

## 7. Later, not in this piece

Hole punching between phone and NAS comes right after the relay (call 4), tested on the loopback DHT
only. Also later: more than four
phones away at once (a waiter pool per NAS at the relay, a server change); relaying through the person's
own Mac (Driftwire option 9) instead of a server.

## 8. Order, once signed off

1. The token derivation and the service's waiters (`serve` over a relay link, the token-keyed admission
   table, `set_relay`), with tests on a loopback relay only: `tools/driftwire-relay` on `::1`, never the
   public DHT; tests filtered by tag (`[driftwire-relay]`, `[phone-relay]`), never by name.
2. The phone's race with the relay (`src/mobile/link.*`), and the QR field, with old-QR and old-service
   tests.
3. The desktop row on Mac and Windows, and `docs/design/28`.

**Verify:** with no forward and the NAS reachable only through a loopback relay, a paired client lists,
range-reads and seeks; the relay's recorded bytes contain no 32-byte run of the file in the clear; a client
without the access key, and a revoked one, are refused through the relay as directly; an old QR and an old
service still pair and connect directly.

## 9. Hole punching (call 4): the design, for sign-off

Built after the relay (§8, done 2026-11-12). Not started until the owner answers §9.4.

### 9.1 How

Reuse Driftwire's remembered-device meeting (`docs/design/28` "Remembered devices", "Four words over
the DHT"), not new machinery:

- **The meeting place.** Phone and NAS each derive the pair's hourly DHT target from what they
  already share: `BLAKE2b(key = access_key, "MediaViewer punch v1" || device_pub || hour)`. A watcher
  of the DHT sees a target that changes every hour and means nothing without the access key, the
  same property remembered devices have today.
- **The NAS** keeps one BEP 44 item per paired device (at most the 4 relay waiters' worth, the same
  slot rule), refreshed each hour, saying its UDP mapping and NAT kind, sealed to that device.
- **The phone**, in its race, looks the item up at once (beside the direct routes) and punches with
  `udp_hub` (UDP first, TCP simultaneous open beside it, the birthday punch when exactly one NAT is
  hard). A punched stream is a `link`: `serve::client::open` and the service's admission run on it
  unchanged, Noise XX+psk0 end to end. The NAS binds a punched link to its device as the relay does.
- **Order of the race:** direct addresses at 0 s, the punch at 0 s (it needs the lookup first, so it
  finishes later), the relay at 1.5 s. The first handshake that completes wins; the rest are cut.

### 9.2 Cost

- The NAS keeps a UDP socket and a DHT routing table (a few hundred bytes a minute, Driftwire's
  measured idle cost), and one DHT put per device per hour.
- The phone does a DHT lookup only when it connects and no direct route answered, never in the
  background. On mobile data that is a few KB.

### 9.3 Tests

On the loopback DHT swarm (`tests/driftwire_swarm.h`) and its stand-in NATs only. Never the public
DHT, filtered by tag (`[phone-punch]`). Cases: a punch through two easy NATs, one hard and one easy
(the birthday), a revoked phone, a wrong access key, and falling back to the relay when the punch
fails.

### 9.4 Owner calls this needs

1. **The NAS joins the public DHT.** `docs/design/28` says today that `driftwire serve` "never
   touches the DHT". With this it would, through public bootstrap nodes, announcing hourly targets
   that mean nothing without the access key, as a desktop with remembered devices does. Yes or no?
2. **On by default, or a toggle** on the card ("Reach the NAS directly when the network allows")?
   Recommended: on whenever a relay is set (so there is always a fallback), off otherwise.
3. **The phone on mobile data looks up the DHT** (a few KB per connect that did not reach home
   directly). Recommended: yes, under today's mobile-data rules for what moves afterwards.
