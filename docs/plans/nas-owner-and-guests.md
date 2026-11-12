# Plan — the NAS owner's passphrase, and temporary and scoped phone access

Status: **plan; the owner made every call in §9 as recommended on 2026-11-12** (C5: option A). Slices 1–3 built
(service, CLI, phone) and folded into `docs/design/28-driftwire.md`; slice 4 (the Mac card) next. Reviewed
against the code by Opus on 2026-11-12; its findings are folded in (session binding, the claim's third
round trip, guests kept out of `devices`, guest codes in their own value, re-scan rules).
Builds on `driftwire serve` and the Phone access card as they are on `claude/mobile-integration-c6a3b5`
(`docs/design/28-driftwire.md` "Claiming and pairing" and "The NAS service", `docs/plans/mobile.md`
§7.2 and §7.4). The owner asked for a setup password, "especially if I want temporary connections".

## 0. The short version

1. **An owner passphrase**, set at `driftwire serve init` on the NAS (or, if init skipped it, at the
   Mac's first claim). It becomes a second factor next to what the owner has today (a device key, and
   for a claim the share token plus the write proof). The Mac stretches it with Argon2id into a
   signing key; the NAS keeps only the salt and that key's **public** half, never the passphrase or
   anything that can answer for it. The Mac signs a fresh challenge bound to the session. Five wrong answers start a lockout that
   doubles and survives a restart. Forgotten: `driftwire serve passphrase reset` on the NAS, over SSH,
   as the service's user.
2. **Grants per phone**: an expiry ("for 24 hours", "for 7 days", "until I remove it") and, later,
   which named folders it sees. A **guest QR** pairs a phone with such a grant. The NAS checks the
   grant on every connection and on running sessions (the same 2 s timer that cuts a revoked phone),
   and says `expired`, not `revoked`. The phone shows "Your access ended", distinct from "This phone
   was removed". The Mac card shows each phone's expiry and folders, and can extend, shorten or remove.
3. **Folder scope cannot be enforced by the NAS alone today**, because the phone works from one
   signed catalogue of the whole library (search, places, 64 px and 512 px thumbnails of every
   folder). §5.4 has the options; the recommendation is to ship expiry and guest QRs now and
   scope with per-scope catalogues as its own slice.

## 1. Threat model

What is protected: the originals and the catalogue (thumbnails, places, people, text), the device
list (who can read), the named folders (which NAS paths are exposed), and the library access key.

| # | Who | Today | With this plan |
|---|---|---|---|
| T1 | A stranger on the internet | Fails at the first AEAD (psk0); no key work, no reply | Unchanged |
| T2 | Someone on the NAS's own network who can write the share (a housemate's PC, malware on any PC with the share mounted, a guest Wi-Fi bridged to the LAN) | Can claim an **unowned** service, so a race on setup day | Cannot claim a service whose passphrase was set at `init`: the claim needs it. Can still make wrong guesses, which lock **claims** (not the owner's daily use, §3.5) |
| T3 | A thief of the owner's Mac, or malware on it, with the owner's device key from the Keychain | Full owner: adds devices, issues codes, changes folders | Reads like a phone, lists devices, can shorten or remove access (a nuisance the owner undoes). Cannot add a phone, issue a code, extend or widen a grant, change folders, or change the passphrase, unless the Mac was unlocked in the last 10 minutes (§3.4) |
| T4 | A guest phone after its time is up | — | Refused at connect (`expired`), running session cut within 2 s. **Still holds the access key and publisher key**, so it can make the NAS do the first handshake steps, never a session: the same residual a revoked phone has. Rotating the key stays the open decision it is in `28-driftwire.md` |
| T5 | A scoped phone reaching other folders | — | Originals and listings outside the grant are refused like an unknown root. The catalogue is the gap (§5.4) |
| T6 | A machine impersonating the NAS to the Mac to harvest the passphrase | — | The passphrase never crosses: only a signature over a nonce and the session's handshake hash. The owner session pins the NAS identity. A fake NAS during a **first** claim (the Mac has not pinned it) gets a signature, or at setup a public key, either of which it can attack offline at Argon2id cost per guess, so it needs to read the share token and be on the LAN; init over SSH shows the identity so the owner can compare it on the claim row (§3.6) |
| T7 | Someone with a copy of the NAS's secrets folder | Has the identity and access key: already the NAS | The store holds a public key, so the copy cannot answer a challenge. Recovering the passphrase (if reused elsewhere) costs one Argon2id (256 MiB, 3 passes) per guess |
| T8 | A NAS with a wrong clock | — | Expiry is the NAS's own clock. A clock set back lengthens a grant; a clock set forward ends it early. The Mac shows times the NAS reported, so the two agree on the card |

Out of scope, as today: root or the DSM admin on the NAS (they own the box and can read the shares),
a forwarded SMB or DSM page (the setup guide forbids it).

## 2. What changes, in one table

| Area | Today | Plan |
|---|---|---|
| Store (NAS) | `identity`, `library_access_key`, `owner`, `devices`, `library_pair_codes` | adds `owner_passphrase`, `owner_lockout`, `library_grants` (§6) |
| Claim | token + write proof, own network, once | the same, then a passphrase challenge when one is set; or setting one when none is (§3.3) |
| Owner session | owner key → `kAdminOp` → every `serve_admin` verb | owner key → read and reducing verbs; an **unlock** (passphrase challenge) for 10 min → additive verbs (§3.4) |
| Device list | one list, all equal | unlimited devices stay in `devices`; anyone with an expiry or scope lives in `library_grants` only, so an older NAS binary refuses them (§6.3) |
| Session check | paired? every 2 s | paired, not expired, and roots in scope; every 2 s and at connect (§5.2) |
| Refusal reasons | `not-paired`, `revoked`, `bad-code`, `too-many`, `busy`, `folders-changed` | adds `expired`; an older app reads it as `not-paired` (`serve_client.cpp:161`) (§4.3) |
| `serve_caps` | version, ops, chunks | adds `features` (to an owner) and `grant` (to a time-limited phone) (§4.1) |
| CLI | init, devices, add, revoke, pair-code, disown | init asks for a passphrase; `passphrase set|reset|status`; `add`/`pair-code`/`grant` take `--for` (and later `--folders`) (§7) |

## 3. The passphrase

### 3.1 The verifier

`K = crypto_pwhash(32 bytes, NFC(passphrase), salt[16], opslimit 3, memlimit 256 MiB, ARGON2ID13)`,
then `(pk, sk) = crypto_sign_seed_keypair(seed = BLAKE2b-256(key=K, "mv.serve.owner.seed.v1"))`.
The NAS stores `{salt, ops, mem, pk}` (§6.1) and nothing else: neither `K` nor `sk` is ever on
the NAS or on the wire. Parameters travel with the record, so
they can rise later without a migration. 256 MiB for ~1 s is what `crypto_pwhash_*_MODERATE` gives;
the Mac computes it for every unlock and the NAS only when the CLI sets or resets one (a DS220+ has
2 GB, a DS218j 512 MB: **S5 has to confirm the small models**; `--mem 64` is the fallback).

Minimum length 10 characters, nothing else (no composition rules). Stored only as `K`; the plaintext
is wiped (`sodium_memzero`) on every path that held it, and so are `K`, the seed and `sk`.

### 3.2 The proof

The service never receives the passphrase, `K` or `sk`. On an owner channel or a claim channel:

1. Client asks for a challenge (`{"do":"challenge"}` on the owner session; on a claim channel the
   service sends it unasked, §3.3).
2. Service answers `{"salt","ops","mem","nonce"}` with a fresh 32-byte nonce, kept for that session
   only, one outstanding at a time, valid 60 s.
3. Client computes `K` and `sk`, then signs
   `"mv.serve.owner.v1" ‖ channel.handshake_hash() ‖ nonce ‖ service_identity ‖ client_key`
   (`crypto_sign_detached`) and sends `{"do":"unlock","sig":hex}`.
4. Service verifies against `pk`, counts a miss (§3.5), and on a match marks the session unlocked
   until `now + 10 min`.

**Session binding needs a channel change.** `channel::pairing_secret()` cannot be used: a library
channel never sets the secret it hashes (`channel.cpp:724`), so it is the same constant on every
library session. Slice 1 adds `channel::handshake_hash()`, the Noise transcript hash `h` after the
handshake, kept by `open_library_v2` on both ends, with a test that two sessions differ and both
ends agree. With it a signature cannot be replayed on, or relayed into, another session; without
it the fresh nonce and the pinned identity were the only guard. The salt is told only to an owner
channel or a claim channel that has passed the write proof, never to a phone.

### 3.3 Setting it, and the claim

- **At init (recommended path).** `driftwire serve init` asks twice on the terminal (or reads one
  line from stdin with `--passphrase-stdin`; `--no-passphrase` skips). It prints the identity, so
  the Mac's claim row can show it for the owner to compare (§3.6).
- **The claim gets a third round trip.** Today: `serve_claim` → `serve_claim_check {nonce}` (sent
  *before* the write proof is checked) → `serve_claim` → `serve_claimed`. Nothing about the
  passphrase may go in `serve_claim_check`, or a reader of the share learns whether one is set. So a
  new Mac says `"passphrase":1` in its first `serve_claim`, and after the write proof is verified the
  service sends a new message, `serve_claim_challenge` (38): `{"passphrase":"set", salt, ops, mem,
  nonce}` or `{"passphrase":"none"}`. The Mac answers with a third `serve_claim` carrying `sig`, or
  (none set) `"set":{"salt","ops","mem","pk"}`; then `serve_claimed`. A wrong signature ends the
  claim with `"why":"passphrase"`, counted.
- **At the first claim**, when init skipped it: the `set` above. Only a public key crosses. This
  does not stop T2 racing the owner on setup day; only init does, and the card says so.
- **An old Mac** (no `"passphrase":1`): against a NAS with a passphrase it is closed with
  `"why":"passphrase"`, which it shows as a failed claim (an old client treats anything but
  `serve_claimed` as refused, `serve_client.cpp:467`); it must update. Against a NAS with none
  (`init --no-passphrase`) it claims the old way, and a new Mac later asks for one (next bullet).
- **An owned service with no passphrase** (claimed before this lands): the owner session may
  `set_passphrase` once with its device key alone, and the card asks for one at once. Until then the
  service behaves exactly as today (call **C4**).
- **Changing it**: `set_passphrase` on an unlocked session; the CLI's `passphrase set` locally.

### 3.4 What needs the passphrase (call **C1**)

| Verb | Today | Recommended |
|---|---|---|
| `devices`, `roots` (paths), `grants` | owner key | owner key |
| `revoke`, shortening a grant, narrowing a grant's folders | owner key | owner key |
| `add_device`, `pair_code`, `guest_code`, extending or widening a grant, `add_root`, `remove_root`, `set_passphrase` | owner key | owner key **and** unlocked |
| the claim | token + write proof + own network | the same **and** passphrase (once set) |

Why reductions stay on the device key: a thief can only take access away (T3), and the Mac's queued
revoke (`phone_revoke_pending`, retried for days while the NAS is away) keeps working without holding
the passphrase. The brief said "add or revoke"; making revoke need it too is possible, and then the
Mac has to keep `K` until the queue drains. **The owner decides.**

An unlock lives with the session: 10 minutes, ended by Lock on the card, by the session ending, or
by the passphrase changing. The Mac keeps `sk` in memory for those 10 minutes so a reconnect does not
ask again, and never writes it to the Keychain (call **C2**).

### 3.5 Rate limits and lockout

Two counters, persisted in `owner_lockout` (§6.2) so a restart does not reset them: **claim** (any
caller with the token and the write proof) and **owner** (the owner's device key). Separate so that
someone on the LAN guessing at claims cannot lock the owner's own Mac out of daily use.

- 5 misses free. The 6th and each later miss lock that counter for 15 min, doubling, at most 24 h.
- While locked, a challenge is answered `{"why":"locked","until":unix}` and no proof is checked.
- A success resets that counter. `passphrase reset` resets both.
- The claim counter counts only callers past the write proof. The owner's own re-claim (its device
  key is the stored owner, the lost-answer case) is counted on the **owner** counter, so a LAN
  guesser cannot block it. What a LAN guesser *can* still do is lock the claim counter for up to
  24 h before the first claim, and a Mac thief can lock the owner counter; both are cured by
  `passphrase reset` over SSH, and the card says so when it sees `locked`.
- One outstanding challenge per session; a new one replaces it. At most 1 challenge per second per
  counter across the service, so the Argon2id work is the client's and the NAS does an HMAC.

### 3.6 Recovery

Forgotten passphrase: on the NAS, as the service's user,

```
driftwire serve passphrase reset          # clears it and both lockouts; asks for a new one
```

It needs a shell on the NAS as that user (SSH, or the Synology Task Scheduler), which is the same
power that can already edit the store; nothing over the network can reset it. A reset is noted in
the service's output with a time and nothing else. The running service reads the verifier fresh for
each challenge, so no restart is needed, and a reset ends every unlocked session.

`driftwire serve status` prints the identity, whether a passphrase is set, and lockout state, so the
owner can compare the identity with the claim row on the Mac.

Other edges: `disown` keeps the passphrase (a new Mac claims with it). Re-running `init` never
replaces one that is set. A NAS rolled back to a binary from before this change ignores
`owner_passphrase` and gates nothing: that fails **open** for the passphrase, which only the signed
unattended updates `28-driftwire.md` lists as not built can prevent.

### 3.7 The CLI does not ask for it locally (call **C3**)

`devices`, `add`, `revoke`, `grant`, `pair-code` on the NAS edit the store directly as the service's
user. Asking for the passphrase there would be theatre: whoever can run them can edit the files, or
run `reset`. So they do not ask. The CLI's passphrase work is `init`, `passphrase set|reset|status`.

## 4. The wire

App Store, Mac and NAS update on different days, so every addition is optional and detected, and
`kVersion` stays 1.

### 4.1 `serve_caps`

- To an owner session: `"features":["passphrase","grants"]`, and `"passphrase":"set"|"none"`,
  `"unlocked":false`. A Mac that sees no `features` knows the NAS predates this and greys the new
  rows with "Update the NAS service to use this".
- To a phone with an expiry or scope: `"grant":{"expires":unix,"scoped":true|false}`. The phone shows
  it and can show the expired state even offline. Old apps ignore unknown keys (`parse_caps`).

### 4.2 New `serve_admin` verbs

`challenge`, `unlock`, `lock`, `set_passphrase`, `grants`, `grant` (`key`, `for_s` or `forever`,
`roots`), `guest_code` (`code`, `ttl_ms`, `for_s`, `roots`). An older NAS answers each
`end_code::unsupported`.

**`devices` keeps answering only unlimited devices**, exactly as today. An old Mac copies every row
of that answer into its own list and serves those phones nearby with no limit (`desk.cpp:419-439`,
`243-258`), so a guest must never appear there. A new Mac asks `grants` as well, which lists
restricted and recently expired devices with `expires`, `roots`, `guest`, `expired`.

An old Mac against a new NAS with a passphrase set: its `add_device`, `pair_code` or folder change is
answered `refused` with `{"why":"locked"}`, which it shows as a generic error. Only a new Mac or the
CLI can set a passphrase, so an old Mac meets this only after the owner chose it.

### 4.3 Refusals

New reason `expired`, at connect and mid-session. `serve_client` gets `refusal::expired`, `link.cpp`
`route_problem::expired`. An older app maps any unknown reason to `not_paired`
(`serve_client.cpp:161-164`), which the app shows as "This phone was removed… scan a new code"
(`LinkConnection.swift:509-516`): wrong but harmless, and the distinct "access ended" state is
only on apps built after this. (A revoked phone is told `not-paired` at connect and `revoked` only
when cut mid-session; that stays.) A grant narrowed
to fewer folders cuts with `folders-changed`, which apps already handle by reconnecting.

### 4.4 The guest QR

The QR stays `v:1` and gains `"grant":{"for_s":86400,"scoped":false}`, shown on the phone's
confirm screen before it redeems ("Guest access for 24 hours"). An older app ignores it and pairs
with the grant anyway, because the NAS holds the grant against the code, not the QR.

## 5. Grants

### 5.1 What a grant is

`{key, name, expires (unix, 0 = never), roots (absent = all), guest (bool), created}`. A guest code
carries `for_s` and `roots`; the clock starts **when the phone redeems it**, not when the QR was shown
(call **C7**). Durations on the card: 1 hour, 24 hours, 7 days, 30 days, Until I remove it. At most
365 days.

### 5.2 Enforcement

- At connect, after the device check: an expired grant is refused `expired`.
- The 2 s watch (`watch_loop` → `cut_unpaired`) also cuts sessions whose grant has passed, with
  `expired`, and sessions whose roots were narrowed, with `folders-changed`.
- `roots` answers only the granted roots. `list`, `stat`, `read_range` on any other root are
  `refused` before anything is touched, exactly like an unknown id.
- The Mac's nearby service enforces the same grants from its own copy of the list. That copy is
  refreshed when the Mac reads the NAS's lists, so an extension made with the CLI on the NAS reaches
  the nearby service only at the Mac's next refresh (an expiry needs no refresh: the clock does it).
- `roots` present and empty means **no folders**, never all: removing the last folder a grant lists
  cuts that phone like an expiry. The Catalogue root is always granted (it is the nearby service's
  only root, `desk.cpp:548`).

**A phone that scans a code while already known** (`session()`, `serve.cpp:672-677`, today spends
the code and goes on). The rule becomes:

- a phone with a grant (live or expired) that presents a code goes through `redeem()`, and the code's
  grant replaces its old one, so re-inviting a guest works;
- an unlimited phone that scans a guest code stays unlimited; the code is spent;
- a guest that scans an unlimited code becomes unlimited (the owner unlocked to make that code).

### 5.3 Expired grants stay listed for 7 days

So the card can show "Expired yesterday" with Extend. Then the service drops them (call **C8**).
Expired grants do not count toward the 16-device limit: when the lists are full, the oldest expired
grant is dropped first, so guest churn cannot fill the list.

### 5.4 Folder scope and the catalogue (call **C5**, the big one)

A phone searches and browses from the catalogue (`.mediaviewer/gen/N/catalog.db`, `tiny.pack`,
`thumbs/*.pack`), which covers every folder and is one signed generation; thumbnail packs mix
folders, and the phone reads them by offset. So the NAS can refuse a scoped phone's **originals**
from other folders, but not their thumbnails, places, people or text, without a format change.

- **A. Expiry and guest QRs now; scope later** (recommended). The wire and storage carry `roots`
  from day one, the NAS enforces it on named folders, and the card does not offer folder choice
  until B lands. Guests see the whole library for the time given, and the card says so.
- **B. Per-scope catalogues.** The publisher writes a filtered generation per distinct active scope
  under `.mediaviewer/scopes/<id>/` (filtered `catalog.db`, its own `tiny.pack` and thumbnail packs),
  and the NAS gives a scoped phone only that subtree of the Catalogue root. Real privacy; a publisher
  and phone sync change of its own (`docs/plans/mobile.md` §5), with storage per scope.
- **C. Originals only.** Offer folders now, enforce only originals. Not recommended: the card would
  promise a privacy the thumbnails break.

## 6. Storage (NAS, `secrets::file_store`, 0600)

### 6.1 `owner_passphrase`

`{"v":1,"alg":"argon2id13+ed25519","ops":3,"mem":268435456,"salt":"32 hex","pk":"64 hex","set":unix}`.
A value that is there and cannot be read is an error that refuses every unlock (never "no
passphrase"): failing open would hand the service to whoever claims next.

### 6.2 `owner_lockout`

`{"v":1,"claim":{"misses":n,"until":unix,"level":k},"owner":{…}}`. Unreadable: treated as locked for
15 minutes from start, and `reset` rewrites it.

### 6.3 `library_grants`

`{"v":1,"grants":[{"key","name","expires","roots":[…]?,"guest","created"}]}`. **A device with any
restriction is only here, never in `devices`.** An older NAS binary (a rollback, or a restore of an
older package) reads only `devices`, so it refuses every guest instead of letting them in for ever:
it fails closed. Making a phone unlimited moves it to `devices`; giving one an expiry moves it here.
Both lists are changed under the existing `devices_lock`, and `devices` + grants together stay within
`known::kMaxDevices` (16). The `devices` format (`known.h`) does not change.

### 6.4 `library_guest_codes`

CLI pairing codes that carry a grant go in this new value, `{"v":1,"codes":[{"hash","expires",
"for_s","roots"?}]}`, never in `library_pair_codes`. That value is fixed 40-byte rows
(`serve.cpp:1408-1423`); a mixed row would make it unreadable, and a guest code kept there would be
redeemed as an unlimited device by an older binary. An older binary never reads this one, so it
fails closed. The owner's guest codes are in memory, beside today's pair codes.

### 6.5 The Mac

`phone_devices` stays (unlimited phones only, from `devices`); `phone_grants` mirrors
`library_grants` from the NAS's `grants` answer, for the nearby service and the card while the NAS
is away. `sk`, while unlocked, lives in memory only.

## 7. The CLI

```
driftwire serve init [--passphrase-stdin | --no-passphrase]
driftwire serve status                     identity, passphrase set or not, lockouts, counts
driftwire serve passphrase set|reset       asks twice on the terminal (or --passphrase-stdin)
driftwire serve add NAME KEY [--for 24h|7d|30d|DURATION] [--folders ID,…]
driftwire serve grant NAME-OR-KEY (--for DURATION | --forever) [--folders ID,… | --all-folders]
driftwire serve pair-code [SECONDS] [--for DURATION] [--folders ID,…]
driftwire serve devices                    now with expiry and folders, and expired ones
```

`--folders` is accepted only once call C5 allows scope. No command prints `K` or the passphrase.

## 8. Interface

### 8.1 Mac card (SwiftUI, `PhoneLinkView.swift`; engine `engine_phone_link.cpp`, `desk.*`)

Following `docs/design/20-edit-workspace.md`'s craft rules; inline rows, no sheets.

- **Claim row**: when the NAS has a passphrase, a secure field "NAS passphrase" and the NAS identity's
  fingerprint to compare with `driftwire serve status`. When it has none, "Choose a passphrase for
  this NAS" twice, with one line: "Set it on the NAS with `driftwire serve init` to stop anyone else
  on your network claiming it first."
- **Lock state**: a small "Locked" / "Unlocked · 9 min" line in the header with Lock. A sensitive
  button on a locked card opens the passphrase field inline next to it; Return unlocks and does the
  action. Wrong: "That is not the NAS passphrase." Locked out: "Too many tries. Try again at 14:20,
  or reset it on the NAS."
- **Phones**: each row gets a detail line: "Until Fri 14:00" / "Expired yesterday" / no line for
  unlimited, and (after C5-B) the folder names. Buttons: Extend (menu of durations), Remove, and for
  an unlimited phone a "Limit…" menu.
- **Add phone**: "Show code" gains a duration menu next to it: Until I remove it (default), 1 hour,
  24 hours, 7 days, 30 days. A timed code shows "Guest code · access for 24 hours from when it is
  scanned".
- **Passphrase row**: "NAS passphrase · Set / Change". Forgotten: one line with the reset command.
- **Older NAS**: the new controls are disabled with "Update the NAS service to use this".

### 8.2 Phone (iOS, `src.ios/MobilePackage`)

- **Confirm screen** (before redeeming): "Guest access for 24 hours" under the library name and
  fingerprint when the QR has a grant.
- **Library notice `expired`** (new, beside `revoked`): "Your access to “Family photos” ended on
  14 November at 14:00. Ask the owner to extend it." Action: Try again (reconnects; an extension
  on the NAS lets it straight back in). Not Scan. `revoked` keeps "This phone was removed".
- **Settings**: "Access until Fri 14:00" for a timed grant.
- **What stays on the phone after expiry** (call **C6**): recommended, a guest grant's local copy
  (catalogue, thumbnail cache, originals cache) is deleted when the phone learns it expired, from the
  NAS or from its own clock past `expires`. An extension refetches. Honest copy: a guest who copied
  files keeps those copies.
- Fixture states `library.expired` and `setup.confirmGuest` for the small-screen tests.

### 8.3 Windows (D9, call **C9**)

All of the above sits in shared C++ (`serve.*`, `desk.*`, `engine_phone_link.cpp`), so Windows gets
the service, the CLI and the engine verbs with no Windows code. The Windows Phone access card does
not exist yet (`docs/plans/mobile.md` M2: "Still to do: … the Windows card"), and `Ai.cs` has no phone
entries. Recommended: this work adds the JSON fields and verbs to the engine, extends `Ai.cs` with
`phone_link` / `phone_link_json` mirrors, and the Windows card, when built, builds these rows with
it. Building the whole Windows card now is a separate task.

## 9. Owner calls (made 2026-11-12: every recommendation accepted)

| # | Call | Recommendation |
|---|---|---|
| C1 | Which owner actions need the passphrase | Additive and sensitive ones (§3.4); revoke, shorten and read on the device key alone |
| C2 | Where the Mac keeps the unlock | Memory, 10 minutes, never the Keychain |
| C3 | The CLI on the NAS | Does not ask locally; `passphrase reset` is the recovery |
| C4 | A NAS already claimed with no passphrase | Behaves as today until one is set; the card asks at once |
| C5 | Folder scope | A: expiry and guest QRs now, scope with per-scope catalogues as its own slice |
| C6 | A guest's local copy after expiry | Deleted on the phone when it learns of the expiry |
| C7 | When a guest's clock starts | At redemption |
| C8 | Expired grants | Listed 7 days for Extend, then dropped |
| C9 | Windows | Engine + `Ai.cs` now; the Windows card is its own task |
| C10 | Lockout numbers | 5 free, then 15 min doubling to 24 h; claim and owner counted apart |
| C11 | Argon2id cost | 3 passes, 256 MiB; 64 MiB if S5 finds a small NAS cannot |

## 10. Slices and tests

Each slice is one draft PR on its own branch from `claude/mobile-integration-c6a3b5`, Opus-reviewed
before it is pushed. Never merged to main here; nothing contacts the public DHT
(`[.driftwire-dht-live]` stays excluded).

1. **Service: passphrase.** `channel::handshake_hash()`, `owner_passphrase`, `owner_lockout`, the
   challenge and unlock verbs, `serve_claim_challenge` and the claim steps, the gating of §3.4, `serve_caps.features`, CLI `init`/`status`/`passphrase`.
   `[driftwire-serve]`: set at init then claim with right, wrong, none; lockout and its doubling across
   a restart; reset clears it and ends unlocked sessions; a proof from one session refused on another;
   an old-style claim against a set passphrase refused; an owned service with none behaves as today;
   the plaintext and `K` never reach the store (scan the store folder); the salt is never told to a
   phone; a copied store cannot answer a challenge; two sessions' handshake hashes differ; the
   owner's re-claim is not blocked by a locked claim counter; an old Mac claims a `--no-passphrase`
   NAS and is refused by one with a passphrase.
2. **Service: grants.** `library_grants`, the move between lists, enforcement at connect and on the
   watch (cut within 2.5 s of expiry), root filtering, guest codes from the owner and the CLI,
   `serve_caps.grant`, expired kept 7 days, `library_guest_codes`, the re-scan rules of §5.2.
   `[driftwire-serve]` and `[phone-pairing]`: an old-format store still loads; a downgrade simulated
   by reading only `devices` and `library_pair_codes` refuses guests and their codes; `devices` never
   lists a guest; an expired guest re-invited by a new code gets in; an emptied `roots` cuts.
3. **Clients.** `refusal::expired`, `route_problem::expired`, the iOS notice, confirm line and
   settings line, local copy deleted on expiry. `[phone-pairing]`, MobilePackage Swift tests, the
   small-screen UI test with the new fixtures.
4. **Mac.** `desk` unlock/claim/grant/guest-code, `phone_grants`, nearby enforcement, the card rows.
   `[phone-link]`, AIChrome Swift tests. Plus the `Ai.cs` mirror (compiled on the Mac per
   `msbuild -t:Compile`).
5. **Docs.** Fold into `docs/design/28-driftwire.md` "Claiming and pairing" and "The NAS service",
   `docs/plans/mobile.md` §7.2 and §7.4 (the "no NAS password to type" sentence changes), and
   `docs/DEVELOPMENT.md`'s CLI list.

**Verify line (proposed):** on loopback, a NAS initialised with a passphrase cannot be claimed
without it; a Mac unlocks with it, adds a 1-hour guest from a guest QR, and the guest phone's
session is cut with `expired` within 2.5 s of the hour ending (time injected) and shown as "access
ended", not "removed"; five wrong unlocks lock the owner counter across a restart, and
`driftwire serve passphrase reset` clears it; a phone app built before this change still pairs and
reads from a NAS built after it, and the reverse.

Not measured by any of this: a real Synology's Argon2id time (S5), a real forwarded port, Windows
at runtime.
