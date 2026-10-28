# Driftwire: relaying for others (volunteer relays)

Status: **plan, not built.** It reverses an owner decision ("A relay is allowed only through the
person's own devices or a relay they configure themselves", [12](../design/12-decision-log.md#driftwire),
2026-10-17), so nothing here starts before the owner makes the calls in [Owner calls](#owner-calls)
and a dated row lands in the decision log.

## Why

Driftwire meets a peer anywhere with a code, client to client ([28](../design/28-driftwire.md#four-words-over-the-dht-472-pr-3)).
On 2026-10-28 a Mac on a university's Wi-Fi and a lab PC on the same campus stopped meeting. The
cause was the private-address gate from #499, fixed the same day (`dht::one_site`). Testing that
fix on the campus network measured the cases that are still left:

- **Two hard NATs.** The campus Wi-Fi is a symmetric NAT pool: one UDP socket gets a different,
  random public port for each destination (STUN, 2026-10-28). Two such sides cannot birthday-punch
  each other (28: "Two hard NATs do not birthday-punch each other"). TCP simultaneous open fails too
  when the NATs rewrite TCP ports. Two eduroam networks, two phone hotspots and two carrier NATs
  all look like this. **Only a third machine both can reach fixes it.**
- **A firewall that drops all unsolicited inbound traffic** (Windows' Domain profile,
  `BlockInbound`, with local rules ignored by policy). A lab PC like that answers only where it has
  sent first, so it can be met but cannot be a relay.
- **No UDP, or only the web out.** The DHT cannot run, so word codes cannot meet at all. That case
  is the person's own relay server on 443, not this plan ([Before this](#before-this)).

Two people who both sit behind hard NATs, with no computer or server of their own, have no
way to connect today. A volunteer, meaning another MediaViewer whose person switched
**Relay for others** on, can be that third machine.

## What a volunteer relay is

It is **a rendezvous, not a forwarder.** The own-device relay (option 9, `relay.h`) has R connect
*onward* to B's places, which is useless when B is behind a hard NAT too. A volunteer instead does
what the relay server does (option 10, `splice.h`): **both sides connect in to it**, and it pairs
the two connections under one token and copies bytes between them. Consequences:

- **R never opens a connection out** for anyone. It cannot be used to reach a host, R's own
  network included, so `relay::fence` is not needed on this path. A hard NAT always lets TCP out,
  so both sides can reach an R that has a way in.
- **R relays only Driftwire.** It pairs only connections that ran the rendezvous handshake (as
  `splice` protocol 3 does), and it copies the sender's bytes only once they start as an internet
  hello (`MVSP`, 2, `I`, as the own-device relay checks today). It is not a TCP tunnel.
- **End to end is unchanged.** A and B run CPace on the code, then Noise XX, through R. R sees
  ciphertext only and holds no key, and both ends see the same six-digit check. The tests check, as
  for both relays today, that no 32-byte run of the file crosses R in the clear.

## How it works

1. **Becoming a relay.** The toggle is in the internet section: **Relay for others**, off by
   default, available only with "Send and receive over the internet" and "Find each other over the
   internet" on. R needs a TCP way in: a public IPv4 or IPv6 address on an interface, or a router
   mapping (PCP, NAT-PMP, UPnP-IGD, from `mapping.h`, kept by `mapping::keeper`), on external 443
   when the router allows it, otherwise any port. Without one the window says why ("this network
   lets nothing in") and R does not announce itself. A firewall R cannot see (a policy on the PC,
   a campus edge) may still block it. Clients skip a relay that does not answer, and R learns
   nothing from that. A self-check is an open question ([Open](#open-questions)).
2. **Announcing.** R announces its TCP port on the public DHT with BEP 5 `announce_peer` under one
   fixed info-hash, SHA-1("MediaViewer Driftwire volunteer relays v1"), every 15 minutes while the
   toggle is on and the app or the background receiver runs. The DHT client today is BEP 44/42
   only. It needs `get_peers` and `announce_peer` (still read-only in BEP 43's sense: it answers no
   queries). Anyone can list relays' addresses this way, and the toggle's text says so.
3. **Picking one.** A receiver whose word code is waiting looks the info-hash up once and connects
   to up to five relays in parallel (the rendezvous hello only). It keeps the fastest that answers
   and registers there as a **waiter** under a token. It then adds the relay's address to its
   sealed DHT item, beside its candidates. The address is sealed under the code, so only the code's
   holders learn which relay it chose. A receiver that finds no relay publishes none, which is
   today's behaviour.
4. **The token** comes from the code: `crypto_kdf` over the word code's key set, under its own
   context, so the sender derives it without anything more on the wire. The relay sees only
   `token_id` (a keyed hash), as a relay server does today. Guessing a code from a token id costs
   the Argon2id work of a code per guess, under the hour's salt (#499).
5. **Sending.** The sender reads the receiver's item as today and punches. If no reply has come
   back within 3 s of "Connecting through both networks…" (the punch keeps running), it also
   connects to the relay named in the item as a sender. Whichever link carries a verified internet
   handshake first wins. The rest close before a byte of the session goes, as `race.h` does for
   direct places.
6. **Remembered devices** then use the same path under the pair's hourly meeting keys
   ([28](../design/28-driftwire.md#remembered-devices-472-pr-4)). The Wi-Fi → internet handover
   follows later, under the session's handover keys.

## What R allows (defaults, owner calls)

- **One relay at a time**, and none while R's own person is sending or receiving. R's own
  transfers come first and stop a relay cleanly.
- **A rate cap** (proposed: 2 MB/s each way) as `splice`'s token bucket. **A daily total**
  (proposed: 10 GB) that resets at local midnight. 120 s of silence ends a relay.
- **Only on mains power and an unmetered network**: `power.h` grows a reader for the power state,
  and the network's cost comes from `NWPathMonitor`'s `isExpensive` / `isConstrained` on the Mac
  and `NetworkInformation.GetInternetConnectionProfile().GetConnectionCost()` on Windows. Linux
  waits for its host half. While those don't hold, R stops announcing.
- **Per-source caps**, as the relay server has: 4 waiters and 4 handshakes per IPv4 /24 or IPv6
  /64, and 16 waiters in all.

## What each person sees

- **R** sees **Relaying for someone** (no name: R does not know who), the rate and the day's
  total, and **Stop relaying**. Nothing about either peer is shown or logged. R logs counts only
  (rule 6).
- **A and B** see **Through a volunteer relay** where the window today says "over the internet".
  The first time, a line explains it: "Another MediaViewer passes the encrypted bytes. It sees both
  computers' addresses, the time and the size, never the files." Whether to ask before using one
  is an owner call.
- A relayed send does not move between links. This is today's rule for relays: a rejoin would need
  R again. If R drops, the session's rejoin tries the punch again, then fails after 20 s. Moving to
  another relay is not built.

## What the person who switches it on gives

- Their upload and download bandwidth, within the caps.
- Their IP address: to both peers of every relay, and to anyone who lists the info-hash on the DHT,
  as a MediaViewer relay.
- Metadata: R learns two addresses, the time and the size of each relay. A malicious relay can drop
  transfers or log that pattern. It cannot read, change or replay anything (CPace and Noise end to
  end). A side that suspects its relay can turn relays off and fall back to the own-relay paths.

## Before this

These come first. They are within the current decisions and fix cases a volunteer pool cannot.

1. **The person's own relay server for word codes and remembered devices** (`tools/driftwire-relay`,
   on 443). Today only direct codes use it ([28](../design/28-driftwire.md#relays-472)). The relay
   address is already sealed into the receiver's DHT item format. It covers networks with no UDP:
   the receiver registers over TCP, and a sender with no UDP cannot meet over the DHT anyway, so
   the server's place has to reach it another way. That is an open question: a word code cannot
   carry an address, and a remembered device can keep the relay from its record.
2. **Private addresses** (#499's gate, narrowed on 2026-10-28 to one /16). Probe the other side's
   RFC 1918 addresses always (the probes carry the code's MAC), or keep the /16. The same change goes
   to direct codes (`engine.cpp`, `lan_places`), which still use "the router says it faces the
   internet from the code's address".

## Slices

| Slice | What | Verify line |
|---|---|---|
| 1 | DHT `get_peers` / `announce_peer` (BEP 5), still read-only | Loopback swarm: a client announces, another lists it; announces expire; private and loopback peers are dropped |
| 2 | R: the toggle, the announce, the rendezvous server in the add-on (`splice` protocol 3 with an open key: Noise NNpsk0 under a key from the token, no CPace on a server key), caps, power and metered gates | Loopback: R pairs a waiter and a sender under one token, refuses a non-hello, keeps every cap; R opens no outbound connection (a socket wrap counts them) |
| 3 | A and B: picking a relay, the item's relay field, the sender's 3 s join, the window's strings (Mac and Windows; CLI) | Stand-in NATs: **two hard NATs meet through a volunteer relay**, the check matches on both sides, no 32-byte run of the file crosses R in the clear; with relays off, nothing changes |
| 4 | Remembered devices through a volunteer relay | Stand-in NATs: two hard NATs, remembered both ways, send through R |

Every slice is dual-track (Mac and Windows hosts, the CLI on both), off by default, with nothing
running while the toggle is off. **Not measurable here:** the public DHT and real NATs. The owner
runs one real pair: two hard-NAT networks (for example two eduroam sites, or a hotspot and
eduroam), with R at home on a router that maps a port.

## Owner calls

1. **Reverse the 2026-10-17 relay decision** for volunteer relays, opt-in, off by default.
2. **Ask before relaying through a volunteer**, or only say so in the window.
3. **The default caps**: one at a time, 2 MB/s, 10 GB a day, mains and unmetered only.
4. **The CLI as a relay**: whether `driftwire` can run as a headless volunteer on a server (one
   flag), and whether the owner runs one at launch so the pool is not empty. That would be a
   project-run relay, which today's decision rules out.
5. **The info-hash**: one fixed one for all versions, or one per protocol version, so an old R is
   not picked by a new client.

## Open questions

- **A self-check for R.** R cannot see a firewall in front of it. Relays could test each other
  ("connect back to me"), but with no relays yet there is nobody to test against. Until then R
  announces when it has a way in, and clients skip relays that don't answer.
- **A relay for a side with no UDP.** It needs the relay's address without the DHT: a remembered
  device can keep the last relay from its record; a word code cannot.
- **Sybil relays.** One party could run many relays to see more metadata. Random choice among the
  fastest few spreads the risk but does not prevent it. The person's own relay, when set, is always
  preferred.

## Not in this plan

TURN (left out in #472), Tor (dropped, [spike](driftwire-tor-spike.md)), a project-run relay (owner
call 4), and relays moving a running transfer.
