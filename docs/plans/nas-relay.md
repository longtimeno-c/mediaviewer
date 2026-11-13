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

## 9. A direct path after the relay (owner call 4, answered 2026-11-12)

The owner's call replaced the DHT design drafted here first: "after doing normal maybe it then
announces / connects to the client directly and that increases transfer speed", and "make mobile
data a toggle on in settings". **No DHT**: `driftwire serve` still never touches it, and the phone
looks nothing up.

### 9.1 How

1. **Connect as built** (§2): the direct addresses, then the person's own relay.
2. **Only on the relay, upgrade.** With the session up through the relay, the phone asks the
   service, inside the sealed session, to try a direct path (`serve_upgrade`, under a new op bit
   `kUpgradeOp` both sides negotiate: an old service or app never sees it). Each side opens a TCP
   simultaneous-open port (`net::tcp_rendezvous`, Driftwire's own) and gathers its candidates:
   - its own network's addresses at that port;
   - **its public address as the relay sees it**: from that same port it asks the relay (a new
     role, `kAsk`, in the relay's hello: the server answers the address and port it sees and
     closes; an older relay refuses the role and there is no public candidate);
   - the service's public name, when the code has one (an address with no forward still punches).
   The phone sends its candidates; the service answers with its own. Both are inside the end-to-end
   session, so the relay sees neither, and only the paired phone can ask.
3. **Punch.** Both sides connect from their port to every candidate of the other for up to 10 s
   (`tcp_rendezvous::next`); a SYN that crosses the other's opens one connection.
4. **Move over.** On the new connection the phone runs the whole handshake again (Noise XX+psk0, the
   device-key check, the grant): the service admits it as any connection. It can only be this
   phone (no one else holds its key with the access key). The phone then makes it the live link:
   reads already running finish on the relayed one, every later read and a playing clip's next
   range go direct, and the relayed session closes when its last read lets go (the swap
   `reconnect` already does). Nothing in flight is dropped.
5. **Otherwise stay on the relay**, silently, and try again later: after 1, 5, 15 minutes, then
   hourly, while the session lasts. A failed try costs a few SYNs and two small messages.

Not tried at home or over a direct route that answered (nothing to gain).

### 9.2 Mobile data

**Settings → "Faster direct connection on mobile data"**, on by default (owner): on Wi-Fi the upgrade
is always tried; on mobile data only with it on. The core reads it (`mv_link_set_upgrade_on_cellular`
and the network the host reports), not only the screen. It sits with the mobile-data rules in the
iPhone's Settings, and shows for every library, the sample's too (which never relays).

### 9.3 Tests and measurement

Loopback only: an in-process relay on 127.0.0.1, both sides' rendezvous on loopback; never the
public DHT or a public relay. Tag `[phone-upgrade]`. Cases: an upgrade that succeeds and moves the
reads (a read running across the move finishes); an old service (no `kUpgradeOp`); an old relay (no
`kAsk`: local candidates only); the toggle off on mobile data; a punch that fails (stays on the
relay); a stranger with the access key cannot ask. **Throughput**: relayed against upgraded on
loopback, the same file, runs alternated, in `[.phone-upgrade-bench]`; loopback shows the relay's
copying cost, not a real NAT's.
