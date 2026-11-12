# Plan — Setting up the NAS service, guided

Piece 2 of the phone/NAS work (2026-11-12). Today the service is set up by hand: SSH in, install
`driftwire`, `driftwire serve init`, write `serve.json`, forward a port, then claim from the desktop
(`docs/design/28` "The NAS service"). This plan settles spike S5 ([mobile.md](mobile.md) §7.2, M1) as
far as it can be settled without a real Synology, says which models are supported, and designs a guided
path and unattended signed updates. Nothing here touches the owner's NAS until the owner says so.

## 1. Spike S5: what is known, and what only a real unit can say

From Synology's own toolchain archive, developer guide and SynoCommunity (desk research, 2026-11-12;
full notes and sources in the session notes, `s5-synology-research.md`):

| Question | Answer |
|---|---|
| C library | DSM 7.0 and 7.1: glibc 2.26 on every platform. DSM 7.2 and 7.3: glibc 2.36. |
| Our Linux build | Built on Ubuntu with glibc dynamic: **does not start on DSM 7.0/7.1**, and a 24.04 build (glibc 2.39) starts on none. It must be built against a glibc 2.26 floor (Synology's 7.1 toolchains) or fully static (musl). |
| Kernels | 3.10 on 32-bit ARM, 4.4 on most models, 5.10 on rtd1619b and EPYC. |
| Package format | DSM 7 SPK. Runs as its own package user (DSM 7 removed run-as-root). `port-config` puts the port in DSM's firewall and port-forwarding screens; `data-share` gives the package user access to an existing share picked in the install wizard; `var/` survives upgrades. |
| Signing | DSM 7 removed package signing, keyrings and trust levels. A third-party SPK installs by Package Center → Manual Install, always with a "not verified by Synology" warning. |
| Updates through Package Center | A feed checks only TLS. Auto-update for third-party packages is reported greyed out (unconfirmed). Assume none. |
| Task Scheduler | A boot-time root script outside Package Center: refused (root, no lifecycle). |
| Container Manager | DSM 7.2+, x86-64 and some ARMv8 (not rtd1296). No unattended image updates without a root-equivalent watcher. A fallback only. |
| Installing for the person | DSM's install API is internal, undocumented, needs an administrator and 2FA. SSH also needs an administrator. |

**Only a real unit can answer:** that the SPK installs and starts on DSM 7.2 and 7.1, that the package
user can read a share granted by `data-share`, that `port-config` opens the port, that the service sees
the claim token written over SMB, that a self-update replaces the binary and keeps `var/`, and the
throughput on a low-end ARM model. These are S5's real-unit checks (§6), owed until the owner says a NAS
may be used.

## 2. Supported list (provisional until §6 runs)

| | Platforms | DSM | Examples |
|---|---|---|---|
| **Supported** | x86-64 (apollolake, geminilake, r1000, v1000, denverton, broadwellnk, epyc) | 7.2, 7.3 | DS218+, DS220+, DS224+, DS420+, DS423+, DS720+, DS723+, DS920+, DS923+, DS1522+, DS1621+, DS1821+ |
| **Supported** | aarch64 (rtd1296, rtd1619b, armada37xx) | 7.2, 7.3 | DS218, DS118, DS418, DS220j, DS223, DS223j, DS124, DS423 |
| **Later, if asked** | the same | 7.0, 7.1 | needs the glibc 2.26 build (static musl covers it for free) |
| **Not supported** | 32-bit ARM (armada38x, alpine, monaco) | — | DS218j and older: kernel 3.10, and SynoCommunity treats them as legacy |

The model's platform is shown by DSM's Info Center (CPU) or `cat /proc/syno_platform`; the setup asks the
person to pick x86-64 or ARM from the model list rather than read it off the NAS.

## 3. The build

One static `driftwire` per architecture (x86-64, aarch64), linked against musl with libsodium and BLAKE3
built in, in a CI job beside the existing Linux packages (`cmake/driftwire-cli`). Static removes the glibc
floor (DSM 7.0 onward would work unchanged), and libsodium already falls back to `/dev/urandom` where
`getrandom` is missing. The program refuses root as it does now.

## 4. The package (`mediaviewer-nas.spk`)

- `INFO`: `package="mediaviewer-nas"`, `os_min_ver="7.2-64570"`, `arch` per build, `silent_upgrade="yes"`.
- `conf/privilege`: run as the package user.
- `conf/resource`: `port-config` for TCP 7878 (the port DSM's forwarding wizard then offers), and
  `data-share` for the share the person picks in the wizard, read-only for the package user.
- `scripts/start-stop-status`: runs `var/bin/driftwire` if a verified update is staged there, else
  `target/bin/driftwire`, as `driftwire serve --data var/serve`, and stops it.
- `scripts/postinst`: `driftwire serve init` (the identity and access key in `var/serve`, 0700) and a
  `serve.json` naming the picked share; nothing else. **The service starts unclaimed** and waits for a
  desktop on its own network (§5).
- `WIZARD_UIFILES/install_uifile`: one page, the share to give phones (a picker of existing shares).
- Built and checked by `tools/package/driftwire_spk.py` (structure, permissions, the INFO fields) with
  no Synology needed.

## 5. The guided path, from Settings → Phone access

The card's "Service on your NAS" row becomes **Set up the service…** when no service is known:

1. **Which NAS.** Synology x86-64, Synology ARM, or "Something else" (the manual guide). The card
   downloads the matching SPK from the release, checks its Ed25519 signature against the key pinned in
   the app, and saves it to Downloads.
2. **Install it.** Three short steps with the words DSM shows: Package Center → Manual Install → choose
   the file → accept the third-party warning → pick the share. The card waits.
3. **Found.** An unclaimed service advertises itself on its own network as `_mediaviewer-nas._tcp`
   (Bonjour/mDNS, unclaimed only, LAN only; no TXT beyond `v=1`). The card finds it, fills "Address on
   your network" and offers **Claim** (the claim proves the share can be written, as today).
4. **From outside.** The port to forward, with DSM's own words (Control Panel → External Access →
   Router Configuration, which lists the package's port), then the address check that exists today.

**The NAS password is never asked for or stored.** The desktop never logs in to DSM, never uses SSH,
and never calls DSM's install API (§7 call 4). The person installs the package; the desktop only
claims.

## 6. Unattended signed updates (§7.2 "Stay updated")

Package Center cannot be relied on, so the service updates itself:

- Once a day, with jitter, it fetches `<release site>/nas/latest.json`
  (`{"version","arch":{"x86_64":{"url","sha256","size"},"aarch64":…},"sig"}`) over HTTPS. The signature
  is Ed25519 over the canonical body with a key pinned in the binary (a new key only arrives inside a
  binary signed by the old one).
- A newer version is downloaded to `var/stage/`, its SHA-256 and size checked, made executable, and
  started once with `--self-test` (it opens its data folder read-only, checks the keys, prints its
  version, and exits 0). Only then is it moved to `var/bin/driftwire` and the service restarted by
  `start-stop-status`.
- If the new binary does not stay up for five minutes (`start-stop-status` counts restarts in
  `var/state`), the previous `var/bin` is restored and that version is skipped.
- `var/serve` (keys, `serve.json`, devices) is never touched by an update: pairings survive.
- The service never downloads anything else, never runs a script it fetched, and reveals only its
  address, its version and its architecture in the request (rule 6).

## 7. Owner calls (answered 2026-11-12)

1. **DSM floor: 7.2 and later only.**
2. **32-bit ARM: not supported.** Provisional: the recommendation, not yet put to the owner.
3. **Self-update: yes**, from the Ed25519-signed feed with staging, self-test and rollback (§6).
4. **Never log in to DSM** for the person: the person installs the SPK, the desktop only claims.
5. **A Package Center source: yes**, as discoverability beside §6. Provisional: the recommendation, not
   yet put to the owner.
6. **The real unit: yes, asking before each step** that installs or changes anything on the owner's
   Synology. Its model and DSM version are to be asked when the checks start.

## 8. Order

1. This plan; the owner's calls.
2. The static builds (CI) and `driftwire_spk.py` with its tests. No NAS needed.
3. `serve --self-test`, the update check, staging and rollback, with tests on loopback (a local HTTPS
   test server and a test signing key).
4. The unclaimed advert and the card's guided path (Mac and Windows).
5. S5's real-unit checks on the owner's NAS, once allowed; the supported list is then final and
   `docs/design/28` "Not built" is rewritten.

**Verify (M2b's, as far as this piece goes):** the SPK installs on a DSM 7.2 x86-64 and an aarch64
Synology, the service starts unclaimed, the desktop finds and claims it with no password, a phone pairs,
and a signed update installs itself unattended and keeps every pairing; a tampered update is refused.
